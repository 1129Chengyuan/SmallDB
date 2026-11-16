#ifndef SSTABLE_H
#define SSTABLE_H

#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "slice.h"
#include "mem_table.h"
#include "bloom_filter.h"

// ============================================================================
// SSTable file format (version 2, block-based)
//
//   [data block 0] ... [data block K-1] [filter block] [meta block] [index block] [footer]
//
// Every block is stored as [contents][crc32c(contents): u32].
//
//   data block    records sorted by key: [key_len u32][value_len u32][key][value] ...
//                 A block is closed once it reaches ~block_size bytes (4 KB by
//                 default). Records never span blocks, so a large value simply
//                 gets a large block of its own.
//   filter block  serialized bloom filter over every key in the table
//   meta block    [num_entries u64][smallest key (len-prefixed)][largest key (len-prefixed)]
//   index block   one entry per data block, in order:
//                 [last_key (len-prefixed)][offset u64][size u64]
//                 (size excludes the 4-byte checksum trailer)
//   footer        fixed 64 bytes at the very end of the file:
//                 [filter handle 16B][meta handle 16B][index handle 16B]
//                 [version u32][crc32c of the preceding 52 bytes u32][magic u64]
//
// All integers are little-endian. The footer is written last, so a file cut
// short by a crash has no valid magic number at its end.
// ============================================================================

// Location of a block inside the file
struct block_handle {
  uint64_t offset = 0;
  uint64_t size = 0;  // contents only, excluding the checksum trailer
};

// Index entry: the last key in a data block, and where that block is
struct IndexEntry {
  std::string last_key;
  block_handle handle;
};

// Streams sorted key/value pairs into a new SSTable file. Used by both
// memtable flush and compaction. Memory use is one data block plus 8 bytes
// per key (hashes for the bloom filter) plus the index.
class sstable_builder {
public:
  // Throws std::runtime_error if the file can't be created
  explicit sstable_builder(const std::string& file_name, size_t block_size = 4096);
  // Deletes the partial file unless finish() succeeded
  ~sstable_builder();
  sstable_builder(const sstable_builder&) = delete;
  sstable_builder& operator=(const sstable_builder&) = delete;

  // Keys must be strictly increasing. Throws on misuse or write failure.
  void add(const slice& key, const slice& value);

  // Writes filter/meta/index blocks and the footer, then fsyncs.
  // Returns false on I/O error (the partial file is removed).
  bool finish();

  uint64_t num_entries() const { return num_entries_; }

private:
  void flush_data_block();
  block_handle write_block(const std::string& contents);

  std::string file_name_;
  std::ofstream out_;
  size_t block_size_;
  uint64_t offset_ = 0;
  uint64_t num_entries_ = 0;

  std::string block_;            // data block being filled
  std::string last_key_;         // last key added (for ordering + index)
  std::string smallest_key_;
  std::vector<IndexEntry> index_;
  std::vector<uint64_t> key_hashes_;  // for the bloom filter, built in finish()
  bool finished_ = false;
};

// An open, immutable SSTable. One object per live file, created once when the
// table appears (flush, compaction, or startup) and kept until it's retired.
// Opening reads only the footer, filter, meta and index blocks; a lookup reads
// exactly one data block with a single pread.
//
// Lifetime: owned by shared_ptr. The file descriptor closes when the last
// reference drops, so the file stays readable even after compaction unlinks it.
class ss_table {
public:
  static constexpr uint64_t MAGIC = 0x3142445F4C4C414DULL;  // "MALL_DB1" little-endian
  static constexpr uint32_t FORMAT_VERSION = 2;
  static constexpr size_t FOOTER_SIZE = 64;

  // Opens an existing table and validates its footer and metadata blocks.
  // Throws std::runtime_error if the file is missing, truncated, corrupt, or
  // in an older format.
  static std::shared_ptr<ss_table> open(const std::string& file_name);

  ~ss_table();
  ss_table(const ss_table&) = delete;
  ss_table& operator=(const ss_table&) = delete;

  // nullopt if the key isn't in this table. A stored tombstone is returned as-is.
  // Thread-safe: const, and pread doesn't share a file position.
  // Throws std::runtime_error on I/O error or checksum mismatch (never silently
  // reports "missing", which could expose an older value from another table).
  std::optional<std::string> get(const slice& key) const;

  const std::string& path() const { return file_name_; }
  size_t num_blocks() const { return index_.size(); }
  uint64_t num_entries() const { return num_entries_; }

  // Writes and fsyncs a table containing every entry of the memtable
  // (tombstones included). Returns false on any I/O error.
  static bool flush_memtable(const mem_table& mem_table, const std::string& file_name);

private:
  friend class SSTableIterator;

  ss_table(const std::string& file_name, int fd);

  // Read a block and verify its checksum; throws on failure
  void read_block(const block_handle& handle, std::string& contents) const;

  std::string file_name_;
  int fd_;
  uint64_t file_size_ = 0;
  uint64_t num_entries_ = 0;
  std::vector<IndexEntry> index_;
  std::unique_ptr<bloom_filter> filter_;
  std::string smallest_;
  std::string largest_;
};

// Streams every entry of a table in key order, one data block at a time.
// Used by compaction. Throws std::runtime_error if the table can't be opened
// or a block is corrupt — an unreadable input must abort the compaction, not
// be treated as empty (which would delete its data).
class SSTableIterator {
public:
  explicit SSTableIterator(const std::string& filename);
  explicit SSTableIterator(std::shared_ptr<const ss_table> table);

  bool valid() const { return valid_; }
  // Advance; returns valid()
  bool next();

  const std::string& key() const { return key_; }
  const std::string& value() const { return value_; }

  void seek_to_first();

private:
  // Parse the record at pos_ in the current block, loading the next block
  // when this one is exhausted
  bool load_current();

  std::shared_ptr<const ss_table> table_;
  size_t block_index_ = 0;
  std::string block_;
  size_t pos_ = 0;
  bool valid_ = false;
  std::string key_;
  std::string value_;
};

#endif //SSTABLE_H
