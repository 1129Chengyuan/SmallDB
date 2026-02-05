#include "compaction.h"
#include "mem_table.h"
#include "ss_table.h"
#include <queue>
#include <fstream>
#include <memory>
#include <iostream>

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

// Old implementation - loads everything into RAM
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

// New streaming merge - only keeps iterators in memory, not all data!
size_t compaction::streaming_merge(const std::vector<std::string>& sstable_files, 
                                    const std::string& output_file) {
    // Open output file for writing
    std::ofstream output(output_file, std::ios::binary);
    if (!output.is_open()) {
        std::cerr << "Failed to open output file: " << output_file << std::endl;
        return 0;
    }

    // Create iterators for each input SSTable
    std::vector<std::unique_ptr<SSTableIterator>> iterators;
    for (const auto& file : sstable_files) {
        auto it = std::make_unique<SSTableIterator>(file);
        if (it->valid()) {
            iterators.push_back(std::move(it));
        }
    }

    if (iterators.empty()) {
        output.close();
        return 0;
    }

    // Min heap to track which iterator has the smallest current key
    std::priority_queue<MergeEntry, std::vector<MergeEntry>, std::greater<MergeEntry>> min_heap;

    // Initialize heap with first entry from each iterator
    for (size_t i = 0; i < iterators.size(); ++i) {
        if (iterators[i]->valid()) {
            min_heap.push({
                iterators[i]->key(),
                iterators[i]->value(),
                i
            });
        }
    }

    std::string last_key = "";
    size_t entries_written = 0;

    // K-way merge: process entries in sorted order
    while (!min_heap.empty()) {
        MergeEntry current = min_heap.top();
        min_heap.pop();

        // Only write if this is a new key (skip duplicates, take most recent)
        if (current.key != last_key) {
            // Skip tombstones
            if (current.value != mem_table::tombstone) {
                // Write entry to output file
                size_t key_size = current.key.size();
                size_t value_size = current.value.size();

                output.write(reinterpret_cast<const char*>(&key_size), sizeof(size_t));
                output.write(reinterpret_cast<const char*>(&value_size), sizeof(size_t));
                output.write(current.key.data(), key_size);
                output.write(current.value.data(), value_size);

                entries_written++;
            }
            last_key = current.key;
        }

        // Advance the iterator that just gave us this entry
        size_t iter_idx = current.table_index;
        if (iterators[iter_idx]->next()) {
            // Still has more entries, push next one
            min_heap.push({
                iterators[iter_idx]->key(),
                iterators[iter_idx]->value(),
                iter_idx
            });
        }
    }

    output.close();

    // Build index for the output file
    ss_table::build_index(output_file);

    return entries_written;
}