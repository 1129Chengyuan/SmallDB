#ifndef MEMTABLE_H
#define MEMTABLE_H

#include <string>
#include <map>
#include "Slice.h"

class MemTable {
public:
  // Size threshold in bytes before flushing to SSTable
  MemTable(size_t threshold);

  void set(const Slice& key, const Slice& value);;

  std::string get(const Slice& key) const;

  bool isFull() const;

  void clear();

  // Read-only view of table. Use second const??
  const std::map<std::string, std::string>& getData() const;

private:
  //Should be able to use strings instead of custom pointers for mem management
  std::map<std::string, std::string> table;

  //not sure if const or not...maybe later add changeMax() for dynamic memtable size?
  size_t max_size;
  size_t curr_size;
};

#endif //MEMTABLE_H
