#include "smalldb.h"
#include "log.h"
#include "compaction.h"
#include "file_util.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace fs = std::filesystem;

namespace {
constexpr const char* MANIFEST_NAME = "MANIFEST";

// "sstable_12.sst" -> 12, or -1 if the name doesn't match
int sstable_number(const std::string& filename) {
  if (filename.rfind("sstable_", 0) != 0) return -1;
  size_t start = filename.find('_') + 1;
  size_t end = filename.find('.');
  if (end == std::string::npos || end <= start) return -1;
  try {
    return std::stoi(filename.substr(start, end - start));
  } catch (const std::exception&) {
    return -1;
  }
}

std::string index_path_for(const std::string& sstable_path) {
  return sstable_path.substr(0, sstable_path.find_last_of('.')) + ".idx";
}

void remove_table_files(const std::string& sstable_path) {
  std::error_code ec;
  fs::remove(sstable_path, ec);
  fs::remove(index_path_for(sstable_path), ec);
}
} // namespace

smalldb::smalldb(const std::string& data_dir, size_t memtable_threshold, bool enable_background_compaction)
    : data_dir_(data_dir), memtable_threshold_(memtable_threshold), sstable_counter_(0),
      background_compaction_enabled_(enable_background_compaction) {

  // Create data directory if it doesn't exist
  if (!fs::exists(data_dir_)) {
    fs::create_directories(data_dir_);
  }

  memtable_ = std::make_unique<mem_table>(memtable_threshold_);

  std::string wal_path = data_dir_ + "/wal.log";
  write_ahead_log_ = std::make_unique<wal>(wal_path);

  load_sstables();
  recover_from_wal();

  // Start background compaction thread if enabled
  if (background_compaction_enabled_) {
    background_thread_ = std::thread(&smalldb::background_compaction_thread, this);
  }
}

smalldb::~smalldb() {
  // Signal background thread to stop
  {
    std::lock_guard<std::mutex> lock(bg_mutex_);
    shutdown_ = true;
  }
  bg_cv_.notify_all();

  // Wait for background thread to finish
  if (background_thread_.joinable()) {
    background_thread_.join();
  }

  // Flush any remaining data. Never let an exception escape a destructor;
  // if this fails the data is still in the WAL and will be recovered.
  try {
    if (memtable_ && !memtable_->getData().empty()) {
      flush_memtable();
    }
  } catch (const std::exception& e) {
    smalldb_log::error("Flush on close failed (data remains in WAL): ", e.what());
  }
}

void smalldb::put(const slice& key, const slice& value) {
  write_ahead_log_->append(key, value);
  memtable_->set(key, value);

  if (memtable_->isFull()) {
    flush_memtable();
  }
}

std::string smalldb::get(const slice& key) const {
  // A tombstone in the memtable shadows any older value in the SSTables,
  // so "deleted here" must stop the search, not fall through.
  std::string value;
  if (memtable_->find(key, value)) {
    return value == mem_table::tombstone ? "" : value;
  }

  // Lock while accessing the SSTable list and bloom filters
  std::lock_guard<std::mutex> lock(sstable_mutex_);

  // Newest to oldest; the first table that has the key wins
  for (const auto& table : sstables_) {
    if (table.filter && !table.filter->contains(key)) {
      continue; // Definitely not in this SSTable
    }

    // Either no bloom filter or bloom says "maybe present"
    ss_table sstable(table.path);
    if (auto result = sstable.get(key)) {
      return *result == mem_table::tombstone ? "" : *result;
    }
  }

  return "";
}

void smalldb::remove(const slice& key) {
  put(key, slice(mem_table::tombstone));
}

void smalldb::compact() {
  // Manual compaction - block until complete
  do_compact();
}

void smalldb::do_compact() {
  // Only one compaction at a time
  std::lock_guard<std::mutex> compact_lock(compaction_mutex_);

  // Snapshot the inputs. We merge ALL current tables, so the output may drop
  // tombstones: no older table survives that a tombstone would need to shadow.
  // Tables flushed while we merge are newer and land at the front of the list.
  std::vector<std::string> inputs;
  {
    std::lock_guard<std::mutex> lock(sstable_mutex_);
    if (sstables_.size() < 2) {
      smalldb_log::info("Not enough SSTables to compact");
      return;
    }
    for (const auto& table : sstables_) {
      inputs.push_back(table.path);
    }
  }

  smalldb_log::info("Starting streaming compaction of ", inputs.size(), " SSTables...");

  // Numbered after the inputs, before anything flushed during the merge,
  // so newest-first ordering by number stays correct.
  std::string compacted_file = get_next_sstable_name();

  // Streaming merge without holding sstable_mutex_ (can take a while)
  std::optional<size_t> entries_written = compaction::streaming_merge(inputs, compacted_file);
  if (!entries_written.has_value()) {
    // Inputs are untouched and still live; nothing to undo
    return;
  }

  // Build the output's bloom filter outside the lock too
  auto filter = build_filter(compacted_file);

  {
    std::lock_guard<std::mutex> lock(sstable_mutex_);

    // Only this function removes tables and it is serialized by
    // compaction_mutex_, so the inputs are still exactly the oldest tables.
    size_t keep = sstables_.size() - inputs.size();
    for (size_t i = 0; i < inputs.size(); ++i) {
      if (sstables_[keep + i].path != inputs[i]) {
        smalldb_log::error("Compaction aborted: table list changed unexpectedly");
        remove_table_files(compacted_file);
        return;
      }
    }

    // Replace only the inputs; keep any tables flushed during the merge
    std::vector<sstable_handle> replaced;
    for (size_t i = keep; i < sstables_.size(); ++i) {
      replaced.push_back(std::move(sstables_[i]));
    }
    sstables_.resize(keep);
    sstables_.push_back({compacted_file, std::move(filter)});

    // Commit point: once the MANIFEST names the output, the inputs are dead.
    if (!write_manifest_locked()) {
      smalldb_log::error("Compaction aborted: failed to write MANIFEST");
      sstables_.pop_back();
      for (auto& table : replaced) {
        sstables_.push_back(std::move(table));
      }
      remove_table_files(compacted_file);
      return;
    }
  }

  // Safe to delete now: no reader can see them and the MANIFEST no longer
  // lists them, so a crash here just leaves orphans that startup removes.
  delete_old_sstables(inputs);

  smalldb_log::info("Streaming compaction complete. Created ", compacted_file);
  smalldb_log::info("Compacted data contains ", *entries_written, " entries");
}

size_t smalldb::get_memtable_size() const {
  return memtable_->getData().size();
}

void smalldb::flush_memtable() {
  if (memtable_->getData().empty()) {
    return;
  }

  std::string sstable_name = get_next_sstable_name();
  smalldb_log::info("Flushing memtable to ", sstable_name);

  // Create bloom filter BEFORE flushing
  auto filter = std::make_unique<bloom_filter>(memtable_->getData().size(), 0.01);
  for (const auto& [key, value] : memtable_->getData()) {
    filter->add(slice(key));
  }

  // Table + index are fsync'd before we touch the WAL. On failure the data
  // is still in the memtable and the WAL, so nothing is lost.
  if (!ss_table::flush_memtable(*memtable_, sstable_name)) {
    remove_table_files(sstable_name);
    throw std::runtime_error("failed to flush memtable to " + sstable_name);
  }

  // Lock while updating SSTable list
  {
    std::lock_guard<std::mutex> lock(sstable_mutex_);
    sstables_.insert(sstables_.begin(), {sstable_name, std::move(filter)});
    if (!write_manifest_locked()) {
      sstables_.erase(sstables_.begin());
      remove_table_files(sstable_name);
      throw std::runtime_error("failed to write MANIFEST after flushing " + sstable_name);
    }
  }

  // Only now is the WAL redundant
  memtable_->clear();
  write_ahead_log_->clear();

  maybe_compact();
}

bool smalldb::write_manifest_locked() {
  // Write-to-temp + fsync + rename is atomic: after a crash the MANIFEST is
  // either the old list or the new one, never a partial file.
  const std::string manifest = data_dir_ + "/" + MANIFEST_NAME;
  const std::string tmp = manifest + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out.is_open()) return false;
    for (const auto& table : sstables_) {
      out << fs::path(table.path).filename().string() << '\n';
    }
    out.close();
    if (!out) return false;
  }
  if (!file_util::sync_file(tmp)) return false;

  std::error_code ec;
  fs::rename(tmp, manifest, ec);
  if (ec) return false;

  // Persist the rename and any newly created table files' directory entries
  return file_util::sync_dir(data_dir_);
}

std::unique_ptr<bloom_filter> smalldb::build_filter(const std::string& path) {
  // Stream keys only; don't load the whole table (values included) into RAM
  std::vector<std::string> keys;
  for (SSTableIterator it(path); it.valid(); it.next()) {
    keys.push_back(it.key());
  }
  auto filter = std::make_unique<bloom_filter>(keys.empty() ? 1 : keys.size(), 0.01);
  for (const auto& key : keys) {
    filter->add(slice(key));
  }
  return filter;
}

void smalldb::load_sstables() {
  sstables_.clear();

  if (!fs::exists(data_dir_)) {
    return;
  }

  // Never reuse a number that exists on disk, even an orphan's
  int max_number = -1;
  std::vector<std::pair<int, std::string>> on_disk;
  for (const auto& entry : fs::directory_iterator(data_dir_)) {
    if (entry.path().extension() == ".sst") {
      int num = sstable_number(entry.path().filename().string());
      if (num >= 0) {
        on_disk.push_back({num, entry.path().string()});
        max_number = std::max(max_number, num);
      }
    }
  }
  sstable_counter_.store(max_number + 1);

  std::vector<std::string> live;
  const std::string manifest = data_dir_ + "/" + MANIFEST_NAME;

  if (fs::exists(manifest)) {
    // The MANIFEST is the source of truth for which tables are live
    std::ifstream in(manifest);
    std::string name;
    while (std::getline(in, name)) {
      if (name.empty()) continue;
      std::string path = data_dir_ + "/" + name;
      if (!fs::exists(path)) {
        throw std::runtime_error("MANIFEST lists missing SSTable " + path);
      }
      live.push_back(path);
    }
  } else {
    // Legacy directory with no MANIFEST: fall back to every .sst, newest first
    std::sort(on_disk.begin(), on_disk.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& [num, path] : on_disk) {
      live.push_back(path);
    }
  }

  // Remove anything not in the live set: outputs of a compaction or flush that
  // crashed before committing, inputs of a compaction that crashed before
  // deleting them, and stray temp files. Loading them could resurrect deletes.
  std::unordered_set<std::string> live_stems;
  for (const auto& path : live) {
    live_stems.insert(fs::path(path).stem().string());
  }
  for (const auto& entry : fs::directory_iterator(data_dir_)) {
    const auto ext = entry.path().extension();
    bool orphan_table = (ext == ".sst" || ext == ".idx") &&
                        !live_stems.count(entry.path().stem().string());
    if (orphan_table || ext == ".tmp") {
      smalldb_log::info("Removing orphaned file ", entry.path().string());
      std::error_code ec;
      fs::remove(entry.path(), ec);
    }
  }

  // Rebuild bloom filters and check for index files
  for (const auto& filepath : live) {
    // Check if index file exists, rebuild if missing
    if (!fs::exists(index_path_for(filepath))) {
      smalldb_log::info("Building missing index for ", filepath);
      ss_table::build_index(filepath);
    }
    sstables_.push_back({filepath, build_filter(filepath)});
  }

  if (!fs::exists(manifest) && !write_manifest_locked()) {
    throw std::runtime_error("failed to write initial MANIFEST in " + data_dir_);
  }

  smalldb_log::info("Loaded ", sstables_.size(), " SSTables");
}

void smalldb::recover_from_wal() {
  std::string wal_path = data_dir_ + "/wal.log";

  if (fs::exists(wal_path) && fs::file_size(wal_path) > 0) {
    smalldb_log::info("Recovering from WAL...");
    write_ahead_log_->recover(*memtable_);
    smalldb_log::info("Recovery complete");
  }
}

std::string smalldb::get_next_sstable_name() {
  return data_dir_ + "/sstable_" + std::to_string(sstable_counter_.fetch_add(1)) + ".sst";
}

void smalldb::maybe_compact() {
  size_t num_tables;
  {
    std::lock_guard<std::mutex> lock(sstable_mutex_);
    num_tables = sstables_.size();
  }
  if (num_tables < COMPACTION_THRESHOLD) {
    return;
  }

  smalldb_log::info("Triggering compaction (", num_tables, " SSTables)");
  if (background_compaction_enabled_) {
    schedule_compaction();
  } else {
    do_compact();
  }
}

void smalldb::delete_old_sstables(const std::vector<std::string>& files) {
  for (const auto& file : files) {
    try {
      fs::remove(file);
      // Also delete the corresponding index file
      std::string index_file = index_path_for(file);
      if (fs::exists(index_file)) {
        fs::remove(index_file);
      }
    } catch (const std::exception& e) {
      smalldb_log::error("Failed to delete ", file, ": ", e.what());
    }
  }
}

void smalldb::background_compaction_thread() {
  std::unique_lock<std::mutex> lock(bg_mutex_);
  while (true) {
    // Wait for compaction to be scheduled or shutdown
    bg_cv_.wait(lock, [this] { return compaction_pending_ || shutdown_; });
    if (shutdown_) {
      break;
    }

    // Consume the request BEFORE running, so a request that arrives while we
    // compact sets pending again and triggers another round (no lost wakeup).
    compaction_pending_ = false;
    compaction_running_ = true;
    lock.unlock();

    smalldb_log::info("[Background] Starting compaction...");
    try {
      do_compact();
    } catch (const std::exception& e) {
      smalldb_log::error("[Background] Compaction failed: ", e.what());
    }
    smalldb_log::info("[Background] Compaction complete");

    lock.lock();
    compaction_running_ = false;
    bg_cv_.notify_all();  // wake wait_for_compaction()
  }
}

void smalldb::schedule_compaction() {
  {
    std::lock_guard<std::mutex> lock(bg_mutex_);
    compaction_pending_ = true;
  }
  bg_cv_.notify_all();
}

void smalldb::wait_for_compaction() {
  if (!background_compaction_enabled_) {
    return;  // compactions already ran inline
  }
  // Block (no polling) until nothing is queued or running
  std::unique_lock<std::mutex> lock(bg_mutex_);
  bg_cv_.wait(lock, [this] {
    return shutdown_ || (!compaction_pending_ && !compaction_running_);
  });
}
