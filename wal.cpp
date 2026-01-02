
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

// Push in [key_size][value_size][key_data][value_data]
void wal::append(const slice& key, const slice& value) {
  if (!wal_file.is_open()) {
    std::cout << wal_file_name << "failed to append" << std::endl;
    return;
  }
  // DON'T USE ENDL HERE! Storing newline chars will corrupt the WAL
  size_t key_size = key.size();
  size_t value_size = value.size();

  // Write the metadata of the key and value strings to map data
  wal_file.write(reinterpret_cast<const char*>(&key_size), sizeof(size_t));
  wal_file.write(reinterpret_cast<const char*>(&value_size), sizeof(size_t));
  // Write actual data
  wal_file.write(key.data(), key_size);
  wal_file.write(value.data(), value_size);
  wal_file.flush();
}

// Complexity is O(nlogn)
void wal::recover(mem_table& mem_table) {
  std::ifstream input_stream = std::ifstream(wal_file_name, std::ios::binary);
  if (!input_stream.is_open()) {
    std::cout << wal_file_name << "failed to open" << std::endl;
    return;
  }
  size_t key_size;
  size_t value_size;
  // While we are still able to read the key metadata
  while (input_stream.read((char*)&key_size, sizeof(size_t))) {
    // Init value metadata
    input_stream.read((char*)&value_size, sizeof(size_t));
    // Make temp strings of size key_size and value_size
    std::string key_buffer = std::string(key_size, 'a');
    std::string value_buffer = std::string(value_size, 'a');

    input_stream.read(&key_buffer[0], key_size);
    input_stream.read(&value_buffer[0], value_size);

    // Insert into mem_table
    mem_table.set(slice(key_buffer), slice(value_buffer));
  }
  input_stream.close();
}

void wal::clear() {
  // flushing buffer
  wal_file.close();
  wal_file.open(wal_file_name, std::ios::binary | std::ios::trunc);
}
