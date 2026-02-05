#ifndef SMALLDB_COMPACTION_H
#define SMALLDB_COMPACTION_H

#include <string>
#include <vector>
#include <map>
#include <cstddef>

class compaction {
public:
  // Old interface - loads all data into RAM (deprecated but kept for compatibility)
  static std::map<std::string, std::string> merge(const std::vector<std::map<std::string, std::string>>& sstables_data);

  // New streaming merge - writes output directly without loading everything into RAM
  // Returns the number of entries written
  static size_t streaming_merge(const std::vector<std::string>& sstable_files, 
                                 const std::string& output_file);
};

#endif // SMALLDB_COMPACTION_H