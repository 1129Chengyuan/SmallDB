#include "compaction.h"
#include "mem_table.h"
#include <queue>

// Helper to track an entry and which SSTable it belongs to
struct MergeEntry {
    std::string key;
    std::string value;
    size_t table_index;

    // Custom comparison!
    bool operator>(const MergeEntry& other) const {
        if (key != other.key) {
            return key > other.key;
        }
        // If keys are equal, higher table_index is more recent
        return table_index > other.table_index;
    }
};

std::map<std::string, std::string> compaction::merge(const std::vector<std::map<std::string, std::string>>& sstables) {
    std::map<std::string, std::string> merged_data;

    // min_heap to get smallest key across all SSTables
    std::priority_queue<MergeEntry, std::vector<MergeEntry>, std::greater<MergeEntry>> min_heap;

    // A vector to track current iterators for each SSTable
    std::vector<std::map<std::string, std::string>::const_iterator> iterators;

    // Building the heap with the first entry from each SSTable
    for (size_t i = 0; i < sstables.size(); ++i) {
        if (!sstables[i].empty()) {
            auto it = sstables[i].begin();
            min_heap.push({it->first, it->second, i});
            iterators.push_back(it);
        } else {
            // NEED THIS or else INDEXING WILL BE OFF
            iterators.push_back(sstables[i].end());
        }
    }

    // We need to track the last key added to avoid duplicates
    std::string last_key = "";

    // K-way merge process: only ever have n members in the heap, one from each SSTable
    // Pop the smallest, push the next from that SSTable
    while (!min_heap.empty()) {
        // Get the smallest entry from the heap
        MergeEntry current = min_heap.top();
        min_heap.pop();

        // If this key is different from the last added key, add it
        // Very cool trick: we only add the most recent since the min_heap orders by table_index
        // So we only need to check if the key is different from last_key
        if (current.key != last_key) {
            if (current.value != mem_table::tombstone) {
                merged_data[current.key] = current.value;
            }
            last_key = current.key;
        }

        // Move current ss_table iterator forward
        auto& it = iterators[current.table_index];
        ++it;
        // If not the end, push the next entry from this SSTable to min_heap
        if (it != sstables[current.table_index].end()) {
            min_heap.push({it->first, it->second, current.table_index});
        }
    }

    return merged_data;
}