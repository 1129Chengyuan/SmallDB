#include "ss_table.h"
#include <fstream>
#include <iostream>
#include <algorithm>

// ============================================================================
// SSTableIterator Implementation
// ============================================================================

SSTableIterator::SSTableIterator(const std::string& filename)
    : filename_(filename), valid_(false) {
  file_.open(filename_, std::ios::binary);
  if (!file_.is_open()) {
    std::cerr << "Failed to open SSTable for iteration: " << filename_ << std::endl;
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

ss_table::ss_table(const std::string &file_name) {
  file_name_ = file_name;
  load_index();
}

std::string ss_table::get_index_file_path(const std::string& sstable_file) {
  // Convert "sstable_0.sst" to "sstable_0.idx"
  size_t pos = sstable_file.find_last_of('.');
  if (pos != std::string::npos) {
    return sstable_file.substr(0, pos) + ".idx";
  }
  return sstable_file + ".idx";
}

void ss_table::load_index() {
  std::string index_file = get_index_file_path(file_name_);
  std::ifstream input(index_file, std::ios::binary);
  
  if (!input.is_open()) {
    // No index file exists, SSTable will work but slower
    return;
  }
  
  sparse_index_.clear();
  
  // Read index entries: [key_size][offset][key_data]
  size_t key_size;
  size_t offset;
  
  while (input.read((char*)&key_size, sizeof(size_t))) {
    input.read((char*)&offset, sizeof(size_t));
    
    std::string key(key_size, '\0');
    input.read(&key[0], key_size);
    
    sparse_index_.push_back({key, offset});
  }
  
  input.close();
}

void ss_table::build_index(const std::string& sstable_file, size_t index_interval) {
  std::string index_file = get_index_file_path(sstable_file);
  std::ifstream input(sstable_file, std::ios::binary);
  std::ofstream output(index_file, std::ios::binary);
  
  if (!input.is_open() || !output.is_open()) {
    std::cerr << "Failed to build index for " << sstable_file << std::endl;
    return;
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
}

size_t ss_table::find_scan_start_offset(const slice& key) const {
  if (sparse_index_.empty()) {
    return 0;  // No index, scan from beginning
  }
  
  // Binary search to find the largest index entry with key <= search key
  auto it = std::upper_bound(
    sparse_index_.begin(), 
    sparse_index_.end(),
    key,
    [](const slice& k, const IndexEntry& entry) {
      return slice(entry.key).compareTo(k) > 0;
    }
  );
  
  // upper_bound returns first element > key, so go back one
  if (it != sparse_index_.begin()) {
    --it;
    return it->offset;
  }
  
  return 0;
}

void ss_table::flush_memtable(const mem_table &mem_table,
                              const std::string &file_name) {
  std::ofstream output_stream(file_name, std::ios::binary);
  if (!output_stream.is_open()) {
    std::cout << file_name << "Failed to create ss_table from mem_table" << std::endl;
    return;
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
  
  // Build index after writing SSTable
  build_index(file_name);
}

std::string ss_table::get(const slice &key) const {
  std::ifstream input_stream(file_name_, std::ios::binary);
  if (!input_stream.is_open()) return "";

  // Use sparse index to skip to the right area
  size_t start_offset = find_scan_start_offset(key);
  input_stream.seekg(start_offset);

  size_t key_size;
  size_t value_size;

  while (input_stream.read((char *)&key_size, sizeof(size_t))) {
    input_stream.read((char *)&value_size, sizeof(size_t));
    std::string key_buffer = std::string(key_size, '\0');

    input_stream.read(&key_buffer[0], key_size);

    int cmp = slice(key_buffer).compareTo(key);
    
    if (cmp == 0) {
      // Found it!
      std::string value_buffer = std::string(value_size, '\0');
      input_stream.read(&value_buffer[0], value_size);
      input_stream.close();
      return value_buffer;
    } else if (cmp > 0) {
      // We've gone past the key (SSTables are sorted), so it doesn't exist
      input_stream.close();
      return "";
    } else {
      // Keep scanning forward
      input_stream.seekg(value_size, std::ios::cur);
    }
  }
  input_stream.close();
  return "";
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
    std::cerr << "Failed to create SSTable: " << file_name << std::endl;
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