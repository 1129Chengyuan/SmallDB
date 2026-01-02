
#include "compaction.h"
#include "mem_table.h"

std::map<std::string, std::string> compaction::merge(const std::vector<std::map<std::string, std::string>>& sstables) {
  std::map<std::string, std::string> merged_data;
  for (auto iterator = sstables.rbegin(); iterator != sstables.rend(); ++iterator) {
    for (const auto& pair : *iterator) {
      merged_data[pair.first] = pair.second;
    }
    for (auto iterator2 = merged_data.begin(); iterator2 != merged_data.end(); ) {
      if (iterator2->second == mem_table::tombstone) {
        iterator2 = merged_data.erase(iterator2);
      } else {
        ++iterator2;
      }
    }
  }
  return merged_data;
}