#ifndef SSTABLE_H
#define SSTABLE_H

#include <string>
#include <map>
#include <memory>
#include <vector>
#include <fstream>
#include <optional>
#include "slice.h"
#include "mem_table.h"
#include "bloom_filter.h"

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

// An open, immutable SSTable. One object per live file, created once when the
// table appears (flush, compaction, or startup) and kept until it's retired.
// Everything a lookup needs stays in memory; a lookup does a single pread.
//
// Lifetime: owned by shared_ptr. The file descriptor closes when the last
// reference drops, so the file stays readable even after compaction unlinks it.
class ss_table {
public:
  // Opens an existing table: keeps the fd open, loads the index (rebuilding
  // it if missing or corrupt), finds the key range, and builds the bloom
  // filter unless one is supplied. Throws std::runtime_error on failure.
  static std::shared_ptr<ss_table> open(const std::string& file_name,
                                        std::unique_ptr<bloom_filter> filter = nullptr);

  ~ss_table();
  ss_table(const ss_table&) = delete;
  ss_table& operator=(const ss_table&) = delete;

  // nullopt if the key isn't in this table. A stored tombstone is returned as-is.
  // Thread-safe: const, and pread doesn't share a file position.
  // Throws std::runtime_error on I/O error or corruption (never silently
  // reports "missing", which could expose an older value from another table).
  std::optional<std::string> get(const slice& key) const;

  const std::string& path() const { return file_name_; }
  size_t index_size() const { return sparse_index_.size(); }

  // Writes and fsyncs the table and its index. Returns false on any I/O error.
  static bool flush_memtable(const mem_table& mem_table, const std::string& file_name);

  static std::map<std::string, std::string> read_all(const std::string& file_name);

  static void write_all(const std::string& file_name, const std::map<std::string, std::string>& data);

  // Build, save and fsync sparse index for an SSTable. Returns false on I/O error.
  static bool build_index(const std::string& sstable_file, size_t index_interval = 32);

private:
  ss_table(const std::string& file_name, int fd);

  // Load the .idx into memory and validate it against the data file.
  // Returns false (leaving the index empty) if it's missing or inconsistent.
  bool load_index();

  // Read exactly n bytes at offset into buf (loops on short reads / EINTR)
  bool read_at(size_t offset, size_t n, std::string& buf) const;

  // Last key in the table, found by scanning the final index chunk
  std::string find_largest_key() const;

  std::unique_ptr<bloom_filter> build_filter() const;

  std::string file_name_;
  int fd_;
  size_t file_size_ = 0;

  // Sparse index: every Nth key with its file offset
  std::vector<IndexEntry> sparse_index_;
  std::unique_ptr<bloom_filter> filter_;

  // Key range; only meaningful when sparse_index_ is non-empty
  std::string smallest_;
  std::string largest_;

  // Helper to get index file path from SSTable file path
  static std::string get_index_file_path(const std::string& sstable_file);
};

#endif //SSTABLE_H
