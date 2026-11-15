#ifndef SMALLDB_CRC32_H
#define SMALLDB_CRC32_H

#include <array>
#include <cstddef>
#include <cstdint>

// Standard CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320).
// Used to detect torn or corrupted WAL records on recovery.
namespace crc32 {

inline const std::array<uint32_t, 256>& table() {
  static const std::array<uint32_t, 256> t = [] {
    std::array<uint32_t, 256> tbl{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      }
      tbl[i] = c;
    }
    return tbl;
  }();
  return t;
}

// Continue a running CRC over more bytes (start with crc = 0)
inline uint32_t extend(uint32_t crc, const char* data, size_t n) {
  const auto& t = table();
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) {
    crc = t[(crc ^ static_cast<uint8_t>(data[i])) & 0xFF] ^ (crc >> 8);
  }
  return ~crc;
}

} // namespace crc32

#endif // SMALLDB_CRC32_H
