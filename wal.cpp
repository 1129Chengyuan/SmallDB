
#include "wal.h"
#include "log.h"
#include "crc32.h"
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

wal::wal(const std::string& file_name) {
  wal_file_name = file_name;
  wal_file.open(wal_file_name, std::ios::binary | std::ios::app);
  if (!wal_file.is_open()) {
    throw std::runtime_error("failed to open WAL " + wal_file_name);
  }
}

wal::~wal() {
  if (wal_file.is_open()) {
    // Should write to disk
    wal_file.close();
  }
}

// Push in [crc][key_size][value_size][key_data][value_data]
void wal::append(const slice& key, const slice& value) {
  if (!wal_file.is_open()) {
    throw std::runtime_error("WAL " + wal_file_name + " is not open");
  }
  // DON'T USE ENDL HERE! Storing newline chars will corrupt the WAL
  size_t key_size = key.size();
  size_t value_size = value.size();

  char header[HEADER_SIZE];
  std::memcpy(header + CRC_SIZE, &key_size, sizeof(size_t));
  std::memcpy(header + CRC_SIZE + sizeof(size_t), &value_size, sizeof(size_t));

  uint32_t crc = crc32::extend(0, header + CRC_SIZE, HEADER_SIZE - CRC_SIZE);
  crc = crc32::extend(crc, key.data(), key_size);
  crc = crc32::extend(crc, value.data(), value_size);
  std::memcpy(header, &crc, CRC_SIZE);

  wal_file.write(header, HEADER_SIZE);
  wal_file.write(key.data(), key_size);
  wal_file.write(value.data(), value_size);
  wal_file.flush();

  // A failed WAL write must not be acknowledged, or the write is lost on crash
  if (!wal_file) {
    throw std::runtime_error("failed to append to WAL " + wal_file_name);
  }
}

// Complexity is O(nlogn)
void wal::recover(mem_table& mem_table) {
  std::ifstream input_stream(wal_file_name, std::ios::binary);
  if (!input_stream.is_open()) {
    smalldb_log::error(wal_file_name, " failed to open for recovery");
    return;
  }

  const size_t file_size = fs::file_size(wal_file_name);
  size_t offset = 0;  // end of the last intact record
  size_t recovered = 0;

  char header[HEADER_SIZE];
  std::string key_buffer;
  std::string value_buffer;

  while (file_size - offset >= HEADER_SIZE) {
    if (!input_stream.read(header, HEADER_SIZE)) break;

    uint32_t stored_crc;
    size_t key_size;
    size_t value_size;
    std::memcpy(&stored_crc, header, CRC_SIZE);
    std::memcpy(&key_size, header + CRC_SIZE, sizeof(size_t));
    std::memcpy(&value_size, header + CRC_SIZE + sizeof(size_t), sizeof(size_t));

    // Validate lengths against the bytes actually left in the file *before*
    // allocating, so a garbage header can't trigger a huge allocation.
    size_t remaining = file_size - offset - HEADER_SIZE;
    if (key_size > remaining || value_size > remaining - key_size) break;

    key_buffer.resize(key_size);
    value_buffer.resize(value_size);
    if (!input_stream.read(key_buffer.data(), key_size)) break;
    if (!input_stream.read(value_buffer.data(), value_size)) break;

    uint32_t crc = crc32::extend(0, header + CRC_SIZE, HEADER_SIZE - CRC_SIZE);
    crc = crc32::extend(crc, key_buffer.data(), key_size);
    crc = crc32::extend(crc, value_buffer.data(), value_size);
    if (crc != stored_crc) break;

    // Insert into mem_table
    mem_table.set(slice(key_buffer), slice(value_buffer));
    offset += HEADER_SIZE + key_size + value_size;
    recovered++;
  }
  input_stream.close();

  if (offset < file_size) {
    // Torn write from a crash (or corruption): drop the tail so new records
    // are appended right after the last good one instead of after garbage.
    smalldb_log::error("WAL: discarding ", (file_size - offset), " bytes of corrupt/incomplete tail after ", recovered, " records");
    wal_file.close();
    fs::resize_file(wal_file_name, offset);
    wal_file.open(wal_file_name, std::ios::binary | std::ios::app);
    if (!wal_file.is_open()) {
      throw std::runtime_error("failed to reopen WAL " + wal_file_name);
    }
  }
}

void wal::clear() {
  // flushing buffer
  wal_file.close();
  wal_file.open(wal_file_name, std::ios::binary | std::ios::trunc);
  if (!wal_file.is_open()) {
    throw std::runtime_error("failed to truncate WAL " + wal_file_name);
  }
}
