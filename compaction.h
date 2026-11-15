#ifndef SMALLDB_COMPACTION_H
#define SMALLDB_COMPACTION_H

#include <string>
#include <vector>
#include <map>
#include <cstddef>
#include <optional>

class compaction {
public:
  // Old interface - loads all data into RAM (deprecated but kept for compatibility)
  static std::map<std::string, std::string> merge(const std::vector<std::map<std::string, std::string>>& sstables_data);

  // New streaming merge - writes output directly without loading everything into RAM.
  // sstable_files must be ordered newest first; tombstones are dropped, so the
  // inputs must include every table older than the newest one being merged.
  // Returns the number of entries written, or nullopt on I/O failure (in which
  // case the partial output is removed and the inputs are untouched).
  static std::optional<size_t> streaming_merge(const std::vector<std::string>& sstable_files, 
                                 const std::string& output_file);
};

#endif // SMALLDB_COMPACTION_H