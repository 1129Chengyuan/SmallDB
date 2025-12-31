//
// Created by Cheng-Yuan Li on 2025/12/30.
//

#include "mem_table.h"

mem_table::mem_table(size_t threshold) {
  max_size = threshold;
  curr_size = 0;
}

void mem_table::set(const slice& key, const slice& value) {
  std::string string_key = std::string(key.data(), key.size());
  std::string string_value = std::string(value.data(), value.size());
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
  std::string stringKey = std::string(key.data(), key.size());
  auto iterator = table.find(stringKey);
  if (iterator != table.end()) {
    // in C++ this should be the value
    return iterator->second;
  }
  return "";
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