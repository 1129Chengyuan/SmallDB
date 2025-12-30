//
// Created by Cheng-Yuan Li on 2025/12/30.
//

#include "MemTable.h"

MemTable::MemTable(size_t threshold) {
  max_size = threshold;
  curr_size = 0;
}

void MemTable::set(const Slice& key, const Slice& value) {

}

// O(log n) lookup due to underlying red-black tree
std::string MemTable::get(const Slice& key) const {
  std::string stringKey = std::string(key.data(), key.size());
  auto iterator = table.find(stringKey);
  if (iterator != table.end()) {
    // in C++ this should be the value
    return iterator->second;
  }
  return "";
}

bool MemTable::isFull() const {
  return curr_size >= max_size;
}

void MemTable::clear() {
  table.clear();
  curr_size = 0;
}

const std::map<std::string, std::string>& MemTable::getData() const {
  return table;
}