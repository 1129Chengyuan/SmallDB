#ifndef SSTABLE_H
#define SSTABLE_H

#include <string>
#include <map>
#include "slice.h"
#include "mem_table.h"

class ss_table {
public:
  ss_table(const std::string& file_name);

  static void flush_memtable(const mem_table& mem_table, const std::string& file_name);

  std::string get(const slice& key) const;

  static std::map<std::string, std::string> read_all(const std::string& file_name);

  static void write_all(const std::string& file_name, const std::map<std::string, std::string>& data);


private:
  std::string file_name_;
  // Add index later to optimize
};

#endif //SSTABLE_H
