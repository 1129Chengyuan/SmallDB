
#include "wal.h"
#include <iostream>

wal::wal(const std::string& file_name) {
  wal_file_name = file_name;
  wal_file.open(wal_file_name, std::ios::binary | std::ios::app);
  if (!wal_file.is_open()) {
    std::cout << wal_file_name << "failed to open" << std::endl;
  }
}

wal::~wal() {
  if (wal_file.is_open()) {
    // Should write to disk
    wal_file.close();
  }
}

void wal::append(const slice& key, const slice& value) {

}

void wal::recover(mem_table& mem_table) {

}

void wal::clear() {
  wal_file.close();
  wal_file.open(wal_file_name, std::ios::binary | std::ios::trunc);
}
