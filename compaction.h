#ifndef SMALLDB_COMPACTION_H
#define SMALLDB_COMPACTION_H

#include <string>
#include <vector>
#include <map>

class compaction {
public:

  static std::map<std::string, std::string> merge(const std::vector<std::map<std::string, std::string>>& sstables_data);

};

#endif // SMALLDB_COMPACTION_H
