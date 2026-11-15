//
// Created by Cheng-Yuan Li on 2025/12/30.
//

#ifndef WAL_H
#define WAL_H

#include "mem_table.h"
#include "slice.h"
#include <cstdint>
#include <fstream>
#include <string>

class wal {
public:
  // const to pass in a read-only view
  // & to pass in memory address
  wal(const std::string& file_name);

  ~wal();

  // Throws std::runtime_error if the record can't be written
  void append(const slice& key, const slice& value);

  // Replays every intact record into mem_table. Stops at the first torn or
  // corrupt record and truncates the file there, so later appends stay readable.
  void recover(mem_table& mem_table);

  // needed whenever I flush the mem_table
  void clear();

private:
  // Record layout: [crc32 (4B)][key_size (8B)][value_size (8B)][key][value]
  // The CRC covers everything after itself.
  static constexpr size_t CRC_SIZE = sizeof(uint32_t);
  static constexpr size_t HEADER_SIZE = CRC_SIZE + 2 * sizeof(size_t);

  std::ofstream wal_file;
  std::string wal_file_name;
};

#endif //WAL_H
