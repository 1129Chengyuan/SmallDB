#ifndef MEMTABLE_H
#define MEMTABLE_H

#include "slice.h"
#include <map>
#include <string>

class mem_table {
public:
  // USED TO MARK DELETED ENTRIES
  static constexpr const char* tombstone = "<<TOMBSTONE>>";

  // Size threshold in bytes before flushing to SSTable
  mem_table(size_t threshold);

  void set(const slice& key, const slice& value);

  std::string get(const slice& key) const;

  void remove(const slice& key);

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
