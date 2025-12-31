//
// Created by Cheng-Yuan Li on 2025/12/30.
//

#ifndef WAL_H
#define WAL_H

#include "mem_table.h"
#include "slice.h"
#include <fstream>
#include <string>

class wal {
public:
  // const to pass in a read-only view
  // & to pass in memory address
  wal(const std::string& file_name);

  ~wal();

  void append(const slice& key, const slice& value);

  void recover(mem_table& mem_table);

  // needed whenever I flush the mem_table
  void clear();

private:
  std::ofstream wal_file;
  std::string wal_file_name;
};

#endif //WAL_H
