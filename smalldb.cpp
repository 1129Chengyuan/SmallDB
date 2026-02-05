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

  // Check Bloom filters before reading SSTables
  for (size_t i = 0; i < sstable_files_.size(); ++i) {
    // Check if bloom filter exists for this SSTable
    if (i < sstable_bloom_filters_.size() &&
        sstable_bloom_filters_[i].second != nullptr) {
      if (!sstable_bloom_filters_[i].second->contains(key)) {
        continue; // Definitely not in this SSTable
      }
    }

    // Either no bloom filter or bloom says "maybe present"
    ss_table sstable(sstable_files_[i]);
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

  std::cout << "Starting streaming compaction of " << sstable_files_.size()
            << " SSTables..." << std::endl;

  std::string compacted_file = get_next_sstable_name();
  
  // Use streaming merge - no RAM pressure!
  size_t entries_written = compaction::streaming_merge(sstable_files_, compacted_file);

  std::vector<std::string> old_files = sstable_files_;

  sstable_files_.clear();
  sstable_files_.push_back(compacted_file);

  // Rebuild bloom filter for compacted SSTable
  // We need to scan the file once to build the bloom filter
  sstable_bloom_filters_.clear();
  auto filter = std::make_unique<bloom_filter>(
      entries_written > 0 ? entries_written : 1, 
      0.01
  );
  
  // Use iterator to build bloom filter without loading all data
  SSTableIterator it(compacted_file);
  while (it.valid()) {
    filter->add(slice(it.key()));
    it.next();
  }
  
  sstable_bloom_filters_.push_back({compacted_file, std::move(filter)});

  // Delete old SSTables
  delete_old_sstables(old_files);

  std::cout << "Streaming compaction complete. Created " << compacted_file << std::endl;
  std::cout << "Compacted data contains " << entries_written << " entries" << std::endl;
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

  // Create bloom filter BEFORE flushing
  auto filter = std::make_unique<bloom_filter>(memtable_->getData().size(), 0.01);
  for (const auto& [key, value] : memtable_->getData()) {
    filter->add(slice(key));
  }

  ss_table::flush_memtable(*memtable_, sstable_name);

  sstable_files_.insert(sstable_files_.begin(), sstable_name);
  sstable_bloom_filters_.insert(
      sstable_bloom_filters_.begin(),
      {sstable_name, std::move(filter)}
  );

  memtable_->clear();
  write_ahead_log_->clear();

  maybe_compact();
}

void smalldb::load_sstables() {
  sstable_files_.clear();
  sstable_bloom_filters_.clear();

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

  // Rebuild bloom filters and check for index files
  for (const auto& [num, filepath] : files) {
    sstable_files_.push_back(filepath);

    // Check if index file exists, rebuild if missing
    std::string index_path = filepath.substr(0, filepath.find_last_of('.')) + ".idx";
    if (!fs::exists(index_path)) {
      std::cout << "Building missing index for " << filepath << std::endl;
      ss_table::build_index(filepath);
    }

    // Read SSTable and rebuild bloom filter
    auto data = ss_table::read_all(filepath);
    auto filter = std::make_unique<bloom_filter>(
        data.size() > 0 ? data.size() : 1,
        0.01
    );
    for (const auto& [key, value] : data) {
      filter->add(slice(key));
    }
    sstable_bloom_filters_.push_back({filepath, std::move(filter)});
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
      // Also delete the corresponding index file
      std::string index_file = file.substr(0, file.find_last_of('.')) + ".idx";
      if (fs::exists(index_file)) {
        fs::remove(index_file);
      }
    } catch (const std::exception& e) {
      std::cerr << "Failed to delete " << file << ": " << e.what() << std::endl;
    }
  }
}