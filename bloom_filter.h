#ifndef SMALLDB_BLOOM_FILTER_H
#define SMALLDB_BLOOM_FILTER_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <cmath>
#include "slice.h"

class bloom_filter {
public:
  // n = expected number of keys, p = target false-positive rate
  bloom_filter(size_t n, double p);

  void add(const slice& key) { add_hash(hash(key)); }

  // Not definitive, may return false positives
  bool contains(const slice& key) const { return contains_hash(hash(key)); }

  void add_hash(uint64_t h);
  bool contains_hash(uint64_t h) const;

  // Stable 64-bit hash (FNV-1a + splitmix64 finalizer). Filters are stored on
  // disk, so this must give the same result on every build and platform —
  // std::hash makes no such guarantee, and a mismatch would make the filter
  // reject keys that are actually present.
  static uint64_t hash(const slice& key);

  // On-disk form: [num_hashes u32][num_bits u64][bit bytes]
  std::string serialize() const;
  // nullptr if the bytes aren't a valid filter
  static std::unique_ptr<bloom_filter> deserialize(const char* data, size_t n);

  size_t num_bits() const { return num_bits_; }
  uint32_t num_hashes() const { return num_hashes_; }

private:
  bloom_filter() = default;

  std::vector<uint8_t> bits_;
  size_t num_bits_ = 0;
  uint32_t num_hashes_ = 0;
};

#endif // SMALLDB_BLOOM_FILTER_H
