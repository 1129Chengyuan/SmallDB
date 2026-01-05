#ifndef SMALLDB_BLOOM_FILTER_H
#define SMALLDB_BLOOM_FILTER_H

#include <vector>
#include <cmath>
#include "slice.h"

class bloom_filter {
public:
  bloom_filter(size_t n, double p);

  void add(const slice& key);

  // Not definitive, may return false positives
  bool contains(const slice& key) const;

private:
  std::pair<size_t, size_t> hash(const slice& key) const {
    std::string key_str = key.toString();
    size_t h1 = std::hash<std::string>{}(key_str);
    size_t h2 = (h1 >> 32) | (h1 << 32);
    // Make sure h2 is odd
    h2 = h2 | 1;
    return {h1, h2};
  }

  std::vector<bool> bit_array_;
  size_t num_hashes_;
};

#endif // SMALLDB_BLOOM_FILTER_H