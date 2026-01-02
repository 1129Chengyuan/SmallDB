#include "ss_table.h"
#include <fstream>
#include <iostream>

ss_table::ss_table(const std::string &file_name) {
        file_name_ = file_name;
}

void ss_table::flush_memtable(const mem_table &mem_table,
                              const std::string &file_name) {
  std::ofstream output_stream(file_name, std::ios::binary);
  if (!output_stream.is_open()) {
    std::cout << file_name << "Failed to create ss_table from mem_table" << std::endl;
    return;
  }
  const auto &data = mem_table.getData();
  for (const auto &pair : data) {
    size_t key_size = pair.first.size();
    size_t value_size = pair.second.size();

    output_stream.write(reinterpret_cast<const char *>(&key_size), sizeof(size_t));
    output_stream.write(reinterpret_cast<const char *>(&value_size), sizeof(size_t));
    output_stream.write(pair.first.data(), key_size);
    output_stream.write(pair.second.data(), value_size);
  }
  output_stream.close();
}

std::string ss_table::get(const slice &key) const {
  std::ifstream input_stream(file_name_, std::ios::binary);
  if (!input_stream.is_open()) return "";

  size_t key_size;
  size_t value_size;


  while (input_stream.read((char *)&key_size, sizeof(size_t))) {
    input_stream.read((char *)&value_size, sizeof(size_t));
    std::string key_buffer = std::string(key_size, 'a');

    input_stream.read(&key_buffer[0], key_size);

    if (slice(key_buffer).compareTo(key) == 0) {
      std::string value_buffer = std::string(value_size, 'a');
      input_stream.read(&value_buffer[0], value_size);
      input_stream.close();
      return value_buffer;
    } else {
      // Skip the value data
      input_stream.seekg(value_size, std::ios::cur);
    }
  }
  input_stream.close();
  return "";
}

std::map<std::string, std::string> ss_table::read_all(const std::string& file_name) {
  std::map<std::string, std::string> data;
  std::ifstream input(file_name, std::ios::binary);

  if (!input.is_open()) {
    return data;
  }

  size_t key_size, value_size;
  while (input.read((char*)&key_size, sizeof(size_t))) {
    input.read((char*)&value_size, sizeof(size_t));

    std::string key(key_size, '\0');
    std::string value(value_size, '\0');

    input.read(&key[0], key_size);
    input.read(&value[0], value_size);

    data[key] = value;
  }

  input.close();
  return data;
}

void ss_table::write_all(const std::string& file_name, const std::map<std::string, std::string>& data) {
  std::ofstream output(file_name, std::ios::binary);

  if (!output.is_open()) {
    std::cerr << "Failed to create SSTable: " << file_name << std::endl;
    return;
  }

  for (const auto& [key, value] : data) {
    size_t key_size = key.size();
    size_t value_size = value.size();

    output.write(reinterpret_cast<const char*>(&key_size), sizeof(size_t));
    output.write(reinterpret_cast<const char*>(&value_size), sizeof(size_t));
    output.write(key.data(), key_size);
    output.write(value.data(), value_size);
  }

  output.close();
}
