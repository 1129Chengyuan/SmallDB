//
// Created by Cheng-Yuan Li on 2025/12/30.
//

#include "mem_table.h"

mem_table::mem_table(size_t threshold) {
  max_size = threshold;
  curr_size = 0;
}

void mem_table::set(const slice& key, const slice& value) {
  std::string string_key = key.toString();
  std::string string_value = value.toString();
  // O(logn) operation
  auto iterator = table.find(string_key);

  if (iterator != table.end()) {
    curr_size -= iterator->second.size();
    iterator->second = string_value;
    curr_size += iterator->second.size();
  } else {
    curr_size += (string_key.size() + string_value.size());
    table.emplace(std::move(string_key), std::move(string_value));
  }
}

// O(log n) lookup due to underlying red-black tree
std::string mem_table::get(const slice& key) const {
  std::string stringKey = key.toString();
  auto iterator = table.find(stringKey);
  if (iterator != table.end()) {
    if (iterator->second == tombstone) {
      return "";
    }
    // in C++ this should be the value
    return iterator->second;
  }
  return "";
}

void mem_table::remove(const slice& key) {
  this->set(key, slice(tombstone));
}

bool mem_table::isFull() const {
  return curr_size >= max_size;
}

void mem_table::clear() {
  table.clear();
  curr_size = 0;
}

const std::map<std::string, std::string>& mem_table::getData() const {
  return table;
}