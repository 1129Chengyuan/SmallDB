#include "smalldb.h"
#include "compaction.h"
#include <filesystem>
#include <iostream>
#include <algorithm>

namespace fs = std::filesystem;

smalldb::smalldb(const std::string& data_dir, size_t memtable_threshold)
    : data_dir_(data_dir), memtable_threshold_(memtable_threshold), sstable_counter_(0) {

  // Create data directory if it doesn't exist
  if (!fs::exists(data_dir_)) {
    fs::create_directories(data_dir_);
  }

  memtable_ = std::make_unique<mem_table>(memtable_threshold_);

  std::string wal_path = data_dir_ + "/wal.log";
  write_ahead_log_ = std::make_unique<wal>(wal_path);

  load_sstables();

  recover_from_wal();
}

smalldb::~smalldb() {
  if (memtable_ && !memtable_->getData().empty()) {
    flush_memtable();
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
  std::string result = memtable_->get(key);
  if (!result.empty() || result == mem_table::tombstone) {
    return result == mem_table::tombstone ? "" : result;
  }

  for (const auto& sstable_file : sstable_files_) {
    ss_table sstable(sstable_file);
    result = sstable.get(key);
    if (!result.empty()) {
      return result == mem_table::tombstone ? "" : result;
    }
  }

  return "";
}

void smalldb::remove(const slice& key) {
  put(key, slice(mem_table::tombstone));
}

void smalldb::compact() {
  if (sstable_files_.size() < 2) {
    std::cout << "Not enough SSTables to compact" << std::endl;
    return;
  }

  std::cout << "Starting compaction of " << sstable_files_.size()
            << " SSTables..." << std::endl;

  std::vector<std::map<std::string, std::string>> tables;
  for (const auto& file : sstable_files_) {
    tables.push_back(ss_table::read_all(file));
  }

  auto merged = compaction::merge(tables);

  std::string compacted_file = get_next_sstable_name();
  ss_table::write_all(compacted_file, merged);

  delete_old_sstables(sstable_files_);

  sstable_files_.clear();
  sstable_files_.push_back(compacted_file);

  std::cout << "Compaction complete. Created " << compacted_file << std::endl;
  std::cout << "Compacted data contains " << merged.size() << " entries" << std::endl;
}

size_t smalldb::get_memtable_size() const {
  return memtable_->getData().size();
}

void smalldb::flush_memtable() {
  if (memtable_->getData().empty()) {
    return;
  }

  std::string sstable_name = get_next_sstable_name();
  std::cout << "Flushing memtable to " << sstable_name << std::endl;

  ss_table::flush_memtable(*memtable_, sstable_name);

  sstable_files_.insert(sstable_files_.begin(), sstable_name);

  memtable_->clear();
  write_ahead_log_->clear();

  maybe_compact();
}

void smalldb::load_sstables() {
  sstable_files_.clear();

  if (!fs::exists(data_dir_)) {
    return;
  }

  std::vector<std::pair<int, std::string>> files;

  for (const auto& entry : fs::directory_iterator(data_dir_)) {
    if (entry.path().extension() == ".sst") {
      std::string filename = entry.path().filename().string();
      size_t start = filename.find('_') + 1;
      size_t end = filename.find('.');
      if (start != std::string::npos && end != std::string::npos) {
        int num = std::stoi(filename.substr(start, end - start));
        files.push_back({num, entry.path().string()});
        sstable_counter_ = std::max(sstable_counter_, num + 1);
      }
    }
  }

  std::sort(files.begin(), files.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });

  for (const auto& file : files) {
    sstable_files_.push_back(file.second);
  }

  std::cout << "Loaded " << sstable_files_.size() << " SSTables" << std::endl;
}

void smalldb::recover_from_wal() {
  std::string wal_path = data_dir_ + "/wal.log";

  if (fs::exists(wal_path) && fs::file_size(wal_path) > 0) {
    std::cout << "Recovering from WAL..." << std::endl;
    write_ahead_log_->recover(*memtable_);
    std::cout << "Recovery complete" << std::endl;
  }
}

std::string smalldb::get_next_sstable_name() {
  return data_dir_ + "/sstable_" + std::to_string(sstable_counter_++) + ".sst";
}

void smalldb::maybe_compact() {
  if (sstable_files_.size() >= COMPACTION_THRESHOLD) {
    std::cout << "Auto-triggering compaction (" << sstable_files_.size()
              << " SSTables)" << std::endl;
    compact();
  }
}

void smalldb::delete_old_sstables(const std::vector<std::string>& files) {
  for (const auto& file : files) {
    try {
      fs::remove(file);
    } catch (const std::exception& e) {
      std::cerr << "Failed to delete " << file << ": " << e.what() << std::endl;
    }
  }
}