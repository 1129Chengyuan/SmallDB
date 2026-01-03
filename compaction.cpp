#include "compaction.h"
#include "mem_table.h"
#include <queue>

// Helper to track an entry and which SSTable it belongs to
struct MergeEntry {
    std::string key;
    std::string value;
    size_t table_index;
    size_t entry_index;

    bool operator>(const MergeEntry& other) const {
        if (key != other.key) {
            return key > other.key;
        }
        // If keys are equal, higher table_index is "smaller" (more recent)
        return table_index > other.table_index;
    }
};

std::map<std::string, std::string> compaction::merge(const std::vector<std::map<std::string, std::string>>& sstables) {
    std::map<std::string, std::string> merged_data;
    std::priority_queue<MergeEntry, std::vector<MergeEntry>, std::greater<MergeEntry>> min_heap;

    std::vector<std::map<std::string, std::string>::const_iterator> iterators;
    for (size_t i = 0; i < sstables.size(); ++i) {
        if (!sstables[i].empty()) {
            auto it = sstables[i].begin();
            min_heap.push({it->first, it->second, i, 0});
            iterators.push_back(it);
        } else {
            iterators.push_back(sstables[i].end());
        }
    }

    std::string last_key = "";

    while (!min_heap.empty()) {
        MergeEntry current = min_heap.top();
        min_heap.pop();

        if (current.key != last_key) {
            if (current.value != mem_table::tombstone) {
                merged_data[current.key] = current.value;
            }
            last_key = current.key;
        }

        auto& it = iterators[current.table_index];
        ++it;
        if (it != sstables[current.table_index].end()) {
            min_heap.push({it->first, it->second, current.table_index, 0});
        }
    }

    return merged_data;
}