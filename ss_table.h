#ifndef SSTABLE_H
#define SSTABLE_H

#include <string>
#include <map>
#include <vector>
#include <fstream>
#include <optional>
#include "slice.h"
#include "mem_table.h"

// Sparse index entry: stores a key and its byte offset in the file
struct IndexEntry {
  std::string key;
  size_t offset;  // byte offset in the SSTable file
};

// Iterator for streaming reads from SSTable
class SSTableIterator {
public:
  SSTableIterator(const std::string& filename);
  ~SSTableIterator();

  // Move to next entry
  bool next();

  // Check if iterator is valid
  bool valid() const;

  // Get current key (only valid if valid() == true)
  std::string key() const;

  // Get current value (only valid if valid() == true)
  std::string value() const;

  // Seek to beginning
  void seek_to_first();

private:
  std::string filename_;
  std::ifstream file_;
  bool valid_;
  std::string current_key_;
  std::string current_value_;

  // Read next entry from file
  bool read_next();
};

class ss_table {
public:
  ss_table(const std::string& file_name);

  static void flush_memtable(const mem_table& mem_table, const std::string& file_name);

  std::string get(const slice& key) const;

  static std::map<std::string, std::string> read_all(const std::string& file_name);

  static void write_all(const std::string& file_name, const std::map<std::string, std::string>& data);

  // Load sparse index from index file
  void load_index();

  // Build and save sparse index for an SSTable
  static void build_index(const std::string& sstable_file, size_t index_interval = 32);

private:
  std::string file_name_;
  
  // Sparse index: every Nth key with its file offset
  std::vector<IndexEntry> sparse_index_;
  
  // Helper to get index file path from SSTable file path
  static std::string get_index_file_path(const std::string& sstable_file);
  
  // Binary search the sparse index to find the offset range to scan
  size_t find_scan_start_offset(const slice& key) const;
};

#endif //SSTABLE_H