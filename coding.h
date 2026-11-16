#ifndef SMALLDB_CODING_H
#define SMALLDB_CODING_H

#include <cstdint>
#include <cstring>
#include <string>

// Fixed-width little-endian encoding for the on-disk SSTable format.
// Explicit byte order (instead of memcpy'ing native integers) makes files
// portable across CPU architectures and 32/64-bit builds.
namespace coding {

inline void put_fixed32(std::string& dst, uint32_t v) {
  char buf[4];
  for (int i = 0; i < 4; ++i) buf[i] = static_cast<char>((v >> (8 * i)) & 0xFF);
  dst.append(buf, 4);
}

inline void put_fixed64(std::string& dst, uint64_t v) {
  char buf[8];
  for (int i = 0; i < 8; ++i) buf[i] = static_cast<char>((v >> (8 * i)) & 0xFF);
  dst.append(buf, 8);
}

inline uint32_t decode_fixed32(const char* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<uint8_t>(p[i])) << (8 * i);
  return v;
}

inline uint64_t decode_fixed64(const char* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(static_cast<uint8_t>(p[i])) << (8 * i);
  return v;
}

// Length-prefixed string: [len u32][bytes]
inline void put_length_prefixed(std::string& dst, const char* data, size_t n) {
  put_fixed32(dst, static_cast<uint32_t>(n));
  dst.append(data, n);
}

// Reads a length-prefixed string at p, advancing p. Returns false if it would
// run past limit (corrupt input).
inline bool get_length_prefixed(const char*& p, const char* limit, std::string& out) {
  if (limit - p < 4) return false;
  uint32_t n = decode_fixed32(p);
  p += 4;
  if (static_cast<size_t>(limit - p) < n) return false;
  out.assign(p, n);
  p += n;
  return true;
}

} // namespace coding

#endif // SMALLDB_CODING_H
