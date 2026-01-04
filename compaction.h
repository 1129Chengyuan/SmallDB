#ifndef SMALLDB_COMPACTION_H
#define SMALLDB_COMPACTION_H

#include <string>
#include <vector>
#include <map>

class compaction {
public:

  // Return map b/c we can guarantee order before writing to I/O
  // k-way merge like in merge sort
  static std::map<std::string, std::string> merge(const std::vector<std::map<std::string, std::string>>& sstables_data);

};

#endif // SMALLDB_COMPACTION_H
