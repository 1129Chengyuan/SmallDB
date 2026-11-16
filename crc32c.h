#ifndef SMALLDB_CRC32C_H
#define SMALLDB_CRC32C_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#elif defined(__SSE4_2__)
#include <nmmintrin.h>
#endif

// CRC-32C (Castagnoli), the checksum LevelDB/RocksDB use for SSTable blocks.
// Unlike the WAL's CRC-32, modern CPUs compute CRC-32C in hardware (ARMv8 CRC
// extension, x86 SSE4.2), so verifying every block on every read costs only a
// fraction of a microsecond. Falls back to a lookup table elsewhere.
namespace crc32c {

inline uint32_t software_extend(uint32_t crc, const char* data, size_t n) {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0x82F63B78u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  for (size_t i = 0; i < n; ++i) {
    crc = table[(crc ^ static_cast<uint8_t>(data[i])) & 0xFF] ^ (crc >> 8);
  }
  return crc;
}

inline uint32_t value(const char* data, size_t n) {
  uint32_t crc = 0xFFFFFFFFu;
#if defined(__ARM_FEATURE_CRC32)
  while (n >= 8) {
    uint64_t v;
    std::memcpy(&v, data, 8);
    crc = __crc32cd(crc, v);
    data += 8;
    n -= 8;
  }
  while (n--) crc = __crc32cb(crc, static_cast<uint8_t>(*data++));
#elif defined(__SSE4_2__)
  while (n >= 8) {
    uint64_t v;
    std::memcpy(&v, data, 8);
    crc = static_cast<uint32_t>(_mm_crc32_u64(crc, v));
    data += 8;
    n -= 8;
  }
  while (n--) crc = _mm_crc32_u8(crc, static_cast<uint8_t>(*data++));
#else
  crc = software_extend(crc, data, n);
#endif
  return ~crc;
}

} // namespace crc32c

#endif // SMALLDB_CRC32C_H
