#include "ss_table.h"
#include "coding.h"
#include "crc32c.h"
#include "file_util.h"
#include "log.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr size_t BLOCK_TRAILER = 4;   // crc32c
constexpr size_t RECORD_HEADER = 8;   // key_len u32 + value_len u32
constexpr double BLOOM_FP_RATE = 0.01;

void put_handle(std::string& dst, const block_handle& h) {
  coding::put_fixed64(dst, h.offset);
  coding::put_fixed64(dst, h.size);
}

block_handle decode_handle(const char* p) {
  return {coding::decode_fixed64(p), coding::decode_fixed64(p + 8)};
}

// Parse the record at block[pos]. Returns false if it runs past the end of
// the block (corruption — checksums make this unlikely but we never trust it).
bool parse_record(const std::string& block, size_t pos,
                  const char*& key, uint32_t& key_len,
                  const char*& value, uint32_t& value_len) {
  if (block.size() - pos < RECORD_HEADER) return false;
  key_len = coding::decode_fixed32(block.data() + pos);
  value_len = coding::decode_fixed32(block.data() + pos + 4);
  size_t remaining = block.size() - pos - RECORD_HEADER;
  if (key_len > remaining || value_len > remaining - key_len) return false;
  key = block.data() + pos + RECORD_HEADER;
  value = key + key_len;
  return true;
}

std::runtime_error corruption(const std::string& file, const std::string& what) {
  return std::runtime_error("corrupt SSTable " + file + ": " + what);
}
} // namespace

// ============================================================================
// sstable_builder
// ============================================================================

sstable_builder::sstable_builder(const std::string& file_name, size_t block_size)
    : file_name_(file_name), block_size_(block_size) {
  out_.open(file_name_, std::ios::binary | std::ios::trunc);
  if (!out_.is_open()) {
    throw std::runtime_error("cannot create SSTable " + file_name_);
  }
}

sstable_builder::~sstable_builder() {
  if (!finished_) {
    // Abandoned (error or exception): never leave a half-written table behind
    out_.close();
    std::error_code ec;
    std::filesystem::remove(file_name_, ec);
  }
}

void sstable_builder::add(const slice& key, const slice& value) {
  if (num_entries_ > 0 && key.compareTo(slice(last_key_)) <= 0) {
    throw std::logic_error("sstable_builder: keys must be strictly increasing");
  }
  if (num_entries_ == 0) {
    smallest_key_ = key.toString();
  }

  coding::put_fixed32(block_, static_cast<uint32_t>(key.size()));
  coding::put_fixed32(block_, static_cast<uint32_t>(value.size()));
  block_.append(key.data(), key.size());
  block_.append(value.data(), value.size());

  last_key_.assign(key.data(), key.size());
  key_hashes_.push_back(bloom_filter::hash(key));
  num_entries_++;

  // Close the block once it's big enough. A single large record makes one
  // oversized block, which is fine: reading it is still one pread.
  if (block_.size() >= block_size_) {
    flush_data_block();
  }
}

block_handle sstable_builder::write_block(const std::string& contents) {
  block_handle handle{offset_, contents.size()};
  std::string trailer;
  coding::put_fixed32(trailer, crc32c::value(contents.data(), contents.size()));
  out_.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  out_.write(trailer.data(), static_cast<std::streamsize>(trailer.size()));
  if (!out_) {
    throw std::runtime_error("write failed: " + file_name_);
  }
  offset_ += contents.size() + BLOCK_TRAILER;
  return handle;
}

void sstable_builder::flush_data_block() {
  if (block_.empty()) return;
  block_handle handle = write_block(block_);
  index_.push_back({last_key_, handle});
  block_.clear();
}

bool sstable_builder::finish() {
  try {
    flush_data_block();

    // Filter block: bloom filter over every key
    bloom_filter filter(key_hashes_.size(), BLOOM_FP_RATE);
    for (uint64_t h : key_hashes_) filter.add_hash(h);
    block_handle filter_handle = write_block(filter.serialize());

    // Meta block: entry count and key range
    std::string meta;
    coding::put_fixed64(meta, num_entries_);
    coding::put_length_prefixed(meta, smallest_key_.data(), smallest_key_.size());
    coding::put_length_prefixed(meta, last_key_.data(), last_key_.size());
    block_handle meta_handle = write_block(meta);

    // Index block: last key + location of every data block
    std::string index;
    for (const auto& entry : index_) {
      coding::put_length_prefixed(index, entry.last_key.data(), entry.last_key.size());
      put_handle(index, entry.handle);
    }
    block_handle index_handle = write_block(index);

    // Footer: written last, fixed size, so readers can always find it
    std::string footer;
    put_handle(footer, filter_handle);
    put_handle(footer, meta_handle);
    put_handle(footer, index_handle);
    coding::put_fixed32(footer, ss_table::FORMAT_VERSION);
    coding::put_fixed32(footer, crc32c::value(footer.data(), footer.size()));
    coding::put_fixed64(footer, ss_table::MAGIC);
    out_.write(footer.data(), static_cast<std::streamsize>(footer.size()));

    out_.close();
    if (!out_) {
      smalldb_log::error("Failed to write SSTable ", file_name_);
      return false;
    }
    // Durable before the caller commits it to the MANIFEST / truncates the WAL
    if (!file_util::sync_file(file_name_)) {
      smalldb_log::error("fsync failed for SSTable ", file_name_);
      return false;
    }
  } catch (const std::exception& e) {
    smalldb_log::error("Failed to finish SSTable ", file_name_, ": ", e.what());
    return false;
  }
  finished_ = true;
  return true;
}

// ============================================================================
// ss_table
// ============================================================================

ss_table::ss_table(const std::string& file_name, int fd)
    : file_name_(file_name), fd_(fd) {}

ss_table::~ss_table() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

void ss_table::read_block(const block_handle& handle, std::string& contents) const {
  // Every block must lie entirely before the footer
  uint64_t limit = file_size_ - FOOTER_SIZE;
  if (handle.offset > limit || handle.size > limit - handle.offset ||
      BLOCK_TRAILER > limit - handle.offset - handle.size) {
    throw corruption(file_name_, "block handle out of range");
  }

  size_t n = static_cast<size_t>(handle.size) + BLOCK_TRAILER;
  contents.resize(n);
  size_t done = 0;
  while (done < n) {
    ssize_t got = ::pread(fd_, contents.data() + done, n - done,
                          static_cast<off_t>(handle.offset + done));
    if (got < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error("read failed in " + file_name_ + ": " + std::strerror(errno));
    }
    if (got == 0) {
      throw corruption(file_name_, "unexpected end of file");
    }
    done += static_cast<size_t>(got);
  }

  uint32_t stored = coding::decode_fixed32(contents.data() + handle.size);
  if (crc32c::value(contents.data(), handle.size) != stored) {
    throw corruption(file_name_, "block checksum mismatch at offset " + std::to_string(handle.offset));
  }
  contents.resize(handle.size);
}

std::shared_ptr<ss_table> ss_table::open(const std::string& file_name) {
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
  table->file_size_ = static_cast<uint64_t>(st.st_size);
  if (table->file_size_ < FOOTER_SIZE) {
    throw corruption(file_name, "file too small to contain a footer");
  }

  // 1. Footer: last 64 bytes
  std::string footer(FOOTER_SIZE, '\0');
  if (::pread(fd, footer.data(), FOOTER_SIZE,
              static_cast<off_t>(table->file_size_ - FOOTER_SIZE)) != static_cast<ssize_t>(FOOTER_SIZE)) {
    throw std::runtime_error("cannot read footer of " + file_name);
  }
  if (coding::decode_fixed64(footer.data() + 56) != MAGIC) {
    throw corruption(file_name, "bad magic number (truncated file, or an old-format SSTable)");
  }
  if (crc32c::value(footer.data(), 52) != coding::decode_fixed32(footer.data() + 52)) {
    throw corruption(file_name, "footer checksum mismatch");
  }
  uint32_t version = coding::decode_fixed32(footer.data() + 48);
  if (version != FORMAT_VERSION) {
    throw corruption(file_name, "unsupported format version " + std::to_string(version));
  }
  block_handle filter_handle = decode_handle(footer.data());
  block_handle meta_handle = decode_handle(footer.data() + 16);
  block_handle index_handle = decode_handle(footer.data() + 32);

  // 2. Meta block: entry count and key range
  std::string meta;
  table->read_block(meta_handle, meta);
  if (meta.size() < 8) throw corruption(file_name, "meta block too small");
  table->num_entries_ = coding::decode_fixed64(meta.data());
  const char* p = meta.data() + 8;
  const char* limit = meta.data() + meta.size();
  if (!coding::get_length_prefixed(p, limit, table->smallest_) ||
      !coding::get_length_prefixed(p, limit, table->largest_)) {
    throw corruption(file_name, "bad meta block");
  }

  // 3. Index block
  std::string index;
  table->read_block(index_handle, index);
  p = index.data();
  limit = index.data() + index.size();
  uint64_t expected_offset = 0;  // data blocks are contiguous from offset 0
  while (p < limit) {
    IndexEntry entry;
    if (!coding::get_length_prefixed(p, limit, entry.last_key) || limit - p < 16) {
      throw corruption(file_name, "bad index block");
    }
    entry.handle = decode_handle(p);
    p += 16;
    if (entry.handle.offset != expected_offset ||
        (!table->index_.empty() &&
         slice(table->index_.back().last_key).compareTo(slice(entry.last_key)) >= 0)) {
      throw corruption(file_name, "index entries out of order");
    }
    expected_offset = entry.handle.offset + entry.handle.size + BLOCK_TRAILER;
    table->index_.push_back(std::move(entry));
  }
  if ((table->num_entries_ == 0) != table->index_.empty() ||
      (!table->index_.empty() && table->index_.back().last_key != table->largest_)) {
    throw corruption(file_name, "index does not match meta block");
  }

  // 4. Filter block
  std::string filter;
  table->read_block(filter_handle, filter);
  table->filter_ = bloom_filter::deserialize(filter.data(), filter.size());
  if (!table->filter_) {
    throw corruption(file_name, "bad filter block");
  }

  return table;
}

std::optional<std::string> ss_table::get(const slice& key) const {
  // 1. Key range and bloom filter: both in memory, no I/O
  if (index_.empty()) {
    return std::nullopt;  // empty table
  }
  if (key.compareTo(slice(smallest_)) < 0 || key.compareTo(slice(largest_)) > 0) {
    return std::nullopt;
  }
  if (!filter_->contains(key)) {
    return std::nullopt;  // definitely not here
  }

  // 2. Binary search: first block whose last key >= key. Always exists,
  //    because key <= largest_ == index_.back().last_key.
  auto it = std::lower_bound(
      index_.begin(), index_.end(), key,
      [](const IndexEntry& entry, const slice& k) {
        return slice(entry.last_key).compareTo(k) < 0;
      });

  // 3. One pread for that block, checksum verified
  std::string block;
  read_block(it->handle, block);

  // 4. Scan the block in memory, comparing keys without allocating
  size_t pos = 0;
  while (pos < block.size()) {
    const char* rec_key; const char* rec_value;
    uint32_t key_len, value_len;
    if (!parse_record(block, pos, rec_key, key_len, rec_value, value_len)) {
      throw corruption(file_name_, "bad record in data block");
    }
    int cmp = slice(rec_key, key_len).compareTo(key);
    if (cmp == 0) {
      return std::string(rec_value, value_len);
    }
    if (cmp > 0) {
      return std::nullopt;  // sorted: we've passed where it would be
    }
    pos += RECORD_HEADER + key_len + value_len;
  }
  return std::nullopt;
}

bool ss_table::flush_memtable(const mem_table& mem_table, const std::string& file_name) {
  try {
    sstable_builder builder(file_name);
    for (const auto& [key, value] : mem_table.getData()) {
      builder.add(slice(key), slice(value));
    }
    return builder.finish();
  } catch (const std::exception& e) {
    smalldb_log::error("Failed to flush memtable to ", file_name, ": ", e.what());
    return false;
  }
}

// ============================================================================
// SSTableIterator
// ============================================================================

SSTableIterator::SSTableIterator(const std::string& filename)
    : SSTableIterator(std::shared_ptr<const ss_table>(ss_table::open(filename))) {}

SSTableIterator::SSTableIterator(std::shared_ptr<const ss_table> table)
    : table_(std::move(table)) {
  seek_to_first();
}

void SSTableIterator::seek_to_first() {
  block_index_ = 0;
  block_.clear();
  pos_ = 0;
  valid_ = load_current();
}

bool SSTableIterator::next() {
  if (!valid_) return false;
  pos_ += RECORD_HEADER + key_.size() + value_.size();
  valid_ = load_current();
  return valid_;
}

bool SSTableIterator::load_current() {
  // Move to the next block when the current one is exhausted
  while (pos_ >= block_.size()) {
    if (block_index_ >= table_->index_.size()) {
      return false;  // end of table
    }
    table_->read_block(table_->index_[block_index_].handle, block_);
    block_index_++;
    pos_ = 0;
  }
  const char* key; const char* value;
  uint32_t key_len, value_len;
  if (!parse_record(block_, pos_, key, key_len, value, value_len)) {
    throw corruption(table_->path(), "bad record in data block");
  }
  key_.assign(key, key_len);
  value_.assign(value, value_len);
  return true;
}
