#include "compaction.h"
#include "log.h"
#include "mem_table.h"
#include "ss_table.h"
#include <queue>
#include <memory>
#include <iostream>
#include <filesystem>

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
        // If keys are equal, the LOWER table_index is more recent (tables are
        // ordered newest first), so it must pop first from the min-heap
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
std::optional<size_t> compaction::streaming_merge(const std::vector<std::string>& sstable_files, 
                                    const std::string& output_file) {
    try {
        // Writes the block-based output; deletes the partial file if we
        // throw or return before finish() succeeds
        sstable_builder builder(output_file);

        // Create iterators for each input SSTable (newest first). Opening or
        // reading a corrupt input throws, which aborts the whole compaction:
        // treating it as empty would delete its data with the inputs.
        std::vector<std::unique_ptr<SSTableIterator>> iterators;
        for (const auto& file : sstable_files) {
            auto it = std::make_unique<SSTableIterator>(file);
            if (it->valid()) {
                iterators.push_back(std::move(it));
            }
        }

        // Min heap to track which iterator has the smallest current key
        std::priority_queue<MergeEntry, std::vector<MergeEntry>, std::greater<MergeEntry>> min_heap;

        // Initialize heap with first entry from each iterator
        for (size_t i = 0; i < iterators.size(); ++i) {
            min_heap.push({iterators[i]->key(), iterators[i]->value(), i});
        }

        std::string last_key;
        bool have_last = false;

        // K-way merge: process entries in sorted order
        while (!min_heap.empty()) {
            MergeEntry current = min_heap.top();
            min_heap.pop();

            // Only write if this is a new key (skip duplicates, take most recent)
            if (!have_last || current.key != last_key) {
                // Skip tombstones
                if (current.value != mem_table::tombstone) {
                    builder.add(slice(current.key), slice(current.value));
                }
                last_key = current.key;
                have_last = true;
            }

            // Advance the iterator that just gave us this entry
            size_t iter_idx = current.table_index;
            if (iterators[iter_idx]->next()) {
                min_heap.push({iterators[iter_idx]->key(), iterators[iter_idx]->value(), iter_idx});
            }
        }

        if (!builder.finish()) {
            smalldb_log::error("Compaction failed (write error): ", output_file);
            return std::nullopt;
        }
        return builder.num_entries();
    } catch (const std::exception& e) {
        smalldb_log::error("Compaction failed (", e.what(), "): ", output_file);
        std::error_code ec;
        std::filesystem::remove(output_file, ec);
        return std::nullopt;
    }
}
