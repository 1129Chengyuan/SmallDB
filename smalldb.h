#ifndef SMALLDB_SMALLDB_H
#define SMALLDB_SMALLDB_H

#include "mem_table.h"
#include "wal.h"
#include "ss_table.h"
#include "slice.h"
#include <string>
#include <vector>
#include <memory>

class smalldb {
public:
  smalldb(const std::string& data_dir, size_t memtable_threshold);

  ~smalldb();

  // Public API
  void put(const slice& key, const slice& value);
  std::string get(const slice& key) const;
  void remove(const slice& key);

  void compact();

  size_t get_num_sstables() const { return sstable_files_.size(); }
  size_t get_memtable_size() const;

private:
  void flush_memtable();

  void load_sstables();

  void recover_from_wal();

  // Generate next SSTable filename
  std::string get_next_sstable_name();

  void maybe_compact();

  // Delete old SSTable files after compaction
  void delete_old_sstables(const std::vector<std::string>& files);

  // Directory to store data files
  std::string data_dir_;
  size_t memtable_threshold_;

  std::unique_ptr<mem_table> memtable_;
  std::unique_ptr<wal> write_ahead_log_;

  std::vector<std::string> sstable_files_;

  int sstable_counter_;

  static constexpr size_t COMPACTION_THRESHOLD = 4;
};

#endif // SMALLDB_SMALLDB_H
