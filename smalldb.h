#ifndef SMALLDB_SMALLDB_H
#define SMALLDB_SMALLDB_H

#include "mem_table.h"
#include "wal.h"
#include "ss_table.h"
#include "slice.h"
#include "bloom_filter.h"
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

// Threading model: put/get/remove/compact are called from a single client
// thread. An optional background thread runs compactions concurrently.
class smalldb {
public:
  smalldb(const std::string& data_dir, size_t memtable_threshold, bool enable_background_compaction = true);

  ~smalldb();

  // Public API
  void put(const slice& key, const slice& value);
  std::string get(const slice& key) const;
  void remove(const slice& key);

  // Manual compaction (blocks until complete)
  void compact();

  size_t get_num_sstables() const {
    std::lock_guard<std::mutex> lock(sstable_mutex_);
    return sstables_.size();
  }
  size_t get_memtable_size() const;

  // Wait for any pending background compaction to complete
  void wait_for_compaction();

private:
  void flush_memtable();

  void load_sstables();

  void recover_from_wal();

  // Generate next SSTable filename (thread-safe)
  std::string get_next_sstable_name();

  void maybe_compact();

  // Delete old SSTable files after compaction
  void delete_old_sstables(const std::vector<std::string>& files);

  // Atomically persist the current table list to MANIFEST.
  // Caller must hold sstable_mutex_.
  bool write_manifest_locked();

  // Background compaction thread
  void background_compaction_thread();

  // Trigger background compaction
  void schedule_compaction();

  // Perform actual compaction (called by both manual and background)
  void do_compact();

  // Directory to store data files
  std::string data_dir_;
  size_t memtable_threshold_;

  std::unique_ptr<mem_table> memtable_;
  std::unique_ptr<wal> write_ahead_log_;

  // Table cache: live SSTables, newest first, each opened once and kept open
  // (index, bloom filter, fd in memory). Guarded by sstable_mutex_; get()
  // holds the lock for the whole lookup.
  std::vector<std::shared_ptr<ss_table>> sstables_;

  // Incremented by both the client thread (flush) and the background thread
  // (compaction output), so it must be atomic to avoid duplicate file names.
  std::atomic<int> sstable_counter_;

  static constexpr size_t COMPACTION_THRESHOLD = 4;

  // Threading for background compaction
  bool background_compaction_enabled_;
  std::thread background_thread_;
  mutable std::mutex sstable_mutex_;  // Protects sstables_ and the MANIFEST file
  std::mutex compaction_mutex_;       // Ensures only one compaction at a time

  // Background scheduling state, guarded by bg_mutex_
  std::mutex bg_mutex_;
  std::condition_variable bg_cv_;
  bool compaction_pending_ = false;
  bool compaction_running_ = false;
  bool shutdown_ = false;
};

#endif // SMALLDB_SMALLDB_H
