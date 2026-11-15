#include "ss_table.h"
#include "log.h"
#include <fstream>
#include <iostream>
#include <algorithm>
#include "file_util.h"
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// ============================================================================
// SSTableIterator Implementation
// ============================================================================

SSTableIterator::SSTableIterator(const std::string& filename)
    : filename_(filename), valid_(false) {
  file_.open(filename_, std::ios::binary);
  if (!file_.is_open()) {
    smalldb_log::error("Failed to open SSTable for iteration: ", filename_);
    return;
  }
  // Read first entry
  valid_ = read_next();
}

SSTableIterator::~SSTableIterator() {
  if (file_.is_open()) {
    file_.close();
  }
}

bool SSTableIterator::next() {
  if (!valid_) {
    return false;
  }
  valid_ = read_next();
  return valid_;
}

bool SSTableIterator::valid() const {
  return valid_;
}

std::string SSTableIterator::key() const {
  return current_key_;
}

std::string SSTableIterator::value() const {
  return current_value_;
}

void SSTableIterator::seek_to_first() {
  file_.clear();
  file_.seekg(0, std::ios::beg);
  valid_ = read_next();
}

bool SSTableIterator::read_next() {
  if (!file_.is_open()) {
    return false;
  }

  size_t key_size, value_size;
  
  if (!file_.read(reinterpret_cast<char*>(&key_size), sizeof(size_t))) {
    return false; // EOF or error
  }
  
  if (!file_.read(reinterpret_cast<char*>(&value_size), sizeof(size_t))) {
    return false;
  }

  current_key_.resize(key_size);
  current_value_.resize(value_size);

  if (!file_.read(&current_key_[0], key_size)) {
    return false;
  }

  if (!file_.read(&current_value_[0], value_size)) {
    return false;
  }

  return true;
}

// ============================================================================
// ss_table Implementation
// ============================================================================

ss_table::ss_table(const std::string& file_name, int fd)
    : file_name_(file_name), fd_(fd) {}

ss_table::~ss_table() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

std::shared_ptr<ss_table> ss_table::open(const std::string& file_name,
                                         std::unique_ptr<bloom_filter> filter) {
  int fd = ::open(file_name.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    throw std::runtime_error("cannot open SSTable " + file_name + ": " + std::strerror(errno));
  }
  // From here on the destructor owns the fd, including if we throw below
  std::shared_ptr<ss_table> table(new ss_table(file_name, fd));

  struct stat st;
  if (::fstat(fd, &st) != 0) {
    throw std::runtime_error("cannot stat SSTable " + file_name + ": " + std::strerror(errno));
  }
  table->file_size_ = static_cast<size_t>(st.st_size);

  if (!table->load_index()) {
    smalldb_log::info("Rebuilding missing or invalid index for ", file_name);
    if (!build_index(file_name) || !table->load_index()) {
      throw std::runtime_error("cannot build a valid index for " + file_name);
    }
  }

  if (!table->sparse_index_.empty()) {
    // The first record is always indexed, so this is the exact smallest key
    table->smallest_ = table->sparse_index_.front().key;
    table->largest_ = table->find_largest_key();
  }

  table->filter_ = filter ? std::move(filter) : table->build_filter();
  return table;
}

std::string ss_table::get_index_file_path(const std::string& sstable_file) {
  // Convert "sstable_0.sst" to "sstable_0.idx"
  size_t pos = sstable_file.find_last_of('.');
  if (pos != std::string::npos) {
    return sstable_file.substr(0, pos) + ".idx";
  }
  return sstable_file + ".idx";
}

bool ss_table::load_index() {
  sparse_index_.clear();

  std::string index_file = get_index_file_path(file_name_);
  std::ifstream input(index_file, std::ios::binary | std::ios::ate);
  if (!input.is_open()) {
    return false;
  }

  // Read the whole index in one go, then parse with bounds checks
  std::string data(static_cast<size_t>(input.tellg()), '\0');
  input.seekg(0);
  if (!input.read(data.data(), data.size())) {
    return false;
  }

  // Index entries: [key_size][offset][key_data]
  constexpr size_t HEADER = 2 * sizeof(size_t);
  std::vector<IndexEntry> entries;
  size_t pos = 0;
  while (pos < data.size()) {
    if (data.size() - pos < HEADER) return false;
    size_t key_size, offset;
    std::memcpy(&key_size, data.data() + pos, sizeof(size_t));
    std::memcpy(&offset, data.data() + pos + sizeof(size_t), sizeof(size_t));
    pos += HEADER;
    if (key_size > data.size() - pos) return false;
    std::string key(data.data() + pos, key_size);
    pos += key_size;

    // Offsets must point inside the data file and keys must be increasing,
    // otherwise lookups would read garbage or binary-search the wrong chunk
    if (offset >= file_size_) return false;
    if (!entries.empty() &&
        (offset <= entries.back().offset || slice(entries.back().key).compareTo(slice(key)) >= 0)) {
      return false;
    }
    entries.push_back({std::move(key), offset});
  }

  // An empty table has an empty index; a non-empty one must index record 0
  if (file_size_ == 0) {
    if (!entries.empty()) return false;
  } else if (entries.empty() || entries.front().offset != 0) {
    return false;
  }

  sparse_index_ = std::move(entries);
  return true;
}

bool ss_table::read_at(size_t offset, size_t n, std::string& buf) const {
  buf.resize(n);
  size_t done = 0;
  while (done < n) {
    ssize_t got = ::pread(fd_, buf.data() + done, n - done, static_cast<off_t>(offset + done));
    if (got < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (got == 0) return false;  // unexpected EOF
    done += static_cast<size_t>(got);
  }
  return true;
}

namespace {
constexpr size_t RECORD_HEADER = 2 * sizeof(size_t);

// Parse the record at buf[pos]: [key_size][value_size][key][value].
// Returns false if it runs past the end of the buffer (corruption).
bool parse_record(const std::string& buf, size_t pos,
                  const char*& key, size_t& key_size,
                  const char*& value, size_t& value_size) {
  if (buf.size() - pos < RECORD_HEADER) return false;
  std::memcpy(&key_size, buf.data() + pos, sizeof(size_t));
  std::memcpy(&value_size, buf.data() + pos + sizeof(size_t), sizeof(size_t));
  size_t remaining = buf.size() - pos - RECORD_HEADER;
  if (key_size > remaining || value_size > remaining - key_size) return false;
  key = buf.data() + pos + RECORD_HEADER;
  value = key + key_size;
  return true;
}
} // namespace

std::string ss_table::find_largest_key() const {
  size_t start = sparse_index_.back().offset;
  std::string buf;
  if (!read_at(start, file_size_ - start, buf)) {
    throw std::runtime_error("cannot read last chunk of " + file_name_);
  }
  std::string last;
  size_t pos = 0;
  while (pos < buf.size()) {
    const char* key; const char* value;
    size_t key_size, value_size;
    if (!parse_record(buf, pos, key, key_size, value, value_size)) {
      throw std::runtime_error("corrupt record in " + file_name_);
    }
    last.assign(key, key_size);
    pos += RECORD_HEADER + key_size + value_size;
  }
  return last;
}

std::unique_ptr<bloom_filter> ss_table::build_filter() const {
  // Stream keys only; don't load the whole table (values included) into RAM
  std::vector<std::string> keys;
  for (SSTableIterator it(file_name_); it.valid(); it.next()) {
    keys.push_back(it.key());
  }
  auto filter = std::make_unique<bloom_filter>(keys.empty() ? 1 : keys.size(), 0.01);
  for (const auto& key : keys) {
    filter->add(slice(key));
  }
  return filter;
}

bool ss_table::build_index(const std::string& sstable_file, size_t index_interval) {
  std::string index_file = get_index_file_path(sstable_file);
  std::ifstream input(sstable_file, std::ios::binary);
  std::ofstream output(index_file, std::ios::binary | std::ios::trunc);
  
  if (!input.is_open() || !output.is_open()) {
    smalldb_log::error("Failed to build index for ", sstable_file);
    return false;
  }
  
  size_t entry_count = 0;
  size_t current_offset = 0;
  size_t key_size, value_size;
  
  while (input.read((char*)&key_size, sizeof(size_t))) {
    input.read((char*)&value_size, sizeof(size_t));
    
    // Every Nth entry goes into the index
    if (entry_count % index_interval == 0) {
      std::string key(key_size, '\0');
      input.read(&key[0], key_size);
      
      // Write to index file: [key_size][offset][key_data]
      output.write(reinterpret_cast<const char*>(&key_size), sizeof(size_t));
      output.write(reinterpret_cast<const char*>(&current_offset), sizeof(size_t));
      output.write(key.data(), key_size);
      
      // Skip the value
      input.seekg(value_size, std::ios::cur);
    } else {
      // Skip this entire entry
      input.seekg(key_size + value_size, std::ios::cur);
    }
    
    // Update offset for next entry
    current_offset += sizeof(size_t) * 2 + key_size + value_size;
    entry_count++;
  }
  
  input.close();
  output.close();
  if (!output) {
    smalldb_log::error("Failed to write index ", index_file);
    return false;
  }
  return file_util::sync_file(index_file);
}

bool ss_table::flush_memtable(const mem_table &mem_table,
                              const std::string &file_name) {
  std::ofstream output_stream(file_name, std::ios::binary | std::ios::trunc);
  if (!output_stream.is_open()) {
    smalldb_log::error(file_name, ": failed to create ss_table from mem_table");
    return false;
  }
  const auto &data = mem_table.getData();
  for (const auto &pair : data) {
    size_t key_size = pair.first.size();
    size_t value_size = pair.second.size();

    output_stream.write(reinterpret_cast<const char *>(&key_size), sizeof(size_t));
    output_stream.write(reinterpret_cast<const char *>(&value_size), sizeof(size_t));
    output_stream.write(pair.first.data(), key_size);
    output_stream.write(pair.second.data(), value_size);
  }
  output_stream.close();
  if (!output_stream) {
    smalldb_log::error(file_name, ": failed to write ss_table");
    return false;
  }

  // Durable before the caller truncates the WAL that backs this data
  if (!file_util::sync_file(file_name)) {
    smalldb_log::error(file_name, ": fsync failed");
    return false;
  }

  // Build index after writing SSTable
  return build_index(file_name);
}

std::optional<std::string> ss_table::get(const slice& key) const {
  // 1. Key range and bloom filter: both in memory, no I/O
  if (sparse_index_.empty()) {
    return std::nullopt;  // empty table
  }
  if (key.compareTo(slice(smallest_)) < 0 || key.compareTo(slice(largest_)) > 0) {
    return std::nullopt;
  }
  if (filter_ && !filter_->contains(key)) {
    return std::nullopt;  // definitely not here
  }

  // 2. Binary search: last index entry with entry.key <= key. Never begin()
  //    because key >= smallest_ == sparse_index_[0].key.
  auto it = std::upper_bound(
      sparse_index_.begin(), sparse_index_.end(), key,
      [](const slice& k, const IndexEntry& entry) {
        return slice(entry.key).compareTo(k) > 0;
      });
  --it;

  // 3. The key, if present, lies between this index entry and the next one
  size_t start = it->offset;
  size_t end = (it + 1 != sparse_index_.end()) ? (it + 1)->offset : file_size_;

  // 4. One pread for the whole chunk
  std::string buf;
  if (!read_at(start, end - start, buf)) {
    throw std::runtime_error("read failed in " + file_name_ + ": " + std::strerror(errno));
  }

  // 5. Scan the chunk in memory, comparing keys without allocating
  size_t pos = 0;
  while (pos < buf.size()) {
    const char* rec_key; const char* rec_value;
    size_t key_size, value_size;
    if (!parse_record(buf, pos, rec_key, key_size, rec_value, value_size)) {
      throw std::runtime_error("corrupt record in " + file_name_);
    }
    int cmp = slice(rec_key, key_size).compareTo(key);
    if (cmp == 0) {
      return std::string(rec_value, value_size);
    }
    if (cmp > 0) {
      return std::nullopt;  // sorted: we've passed where it would be
    }
    pos += RECORD_HEADER + key_size + value_size;
  }
  return std::nullopt;
}

std::map<std::string, std::string> ss_table::read_all(const std::string& file_name) {
  std::map<std::string, std::string> data;
  std::ifstream input(file_name, std::ios::binary);

  if (!input.is_open()) {
    return data;
  }

  size_t key_size, value_size;
  while (input.read((char*)&key_size, sizeof(size_t))) {
    input.read((char*)&value_size, sizeof(size_t));

    std::string key(key_size, '\0');
    std::string value(value_size, '\0');

    input.read(&key[0], key_size);
    input.read(&value[0], value_size);

    data[key] = value;
  }

  input.close();
  return data;
}

void ss_table::write_all(const std::string& file_name, const std::map<std::string, std::string>& data) {
  std::ofstream output(file_name, std::ios::binary);

  if (!output.is_open()) {
    smalldb_log::error("Failed to create SSTable: ", file_name);
    return;
  }

  for (const auto& [key, value] : data) {
    size_t key_size = key.size();
    size_t value_size = value.size();

    output.write(reinterpret_cast<const char*>(&key_size), sizeof(size_t));
    output.write(reinterpret_cast<const char*>(&value_size), sizeof(size_t));
    output.write(key.data(), key_size);
    output.write(value.data(), value_size);
  }

  output.close();
  
  // Build index after writing SSTable
  build_index(file_name);
}