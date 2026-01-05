#include "bloom_filter.h"

bloom_filter::bloom_filter(size_t n, double p) {
  // Optimal size calculation
  auto m = static_cast<size_t>(-(n * std::log(p)) / (std::log(2) * std::log(2)));
  bit_array_.resize(m, false);

  // Number of hash functions
  num_hashes_ = static_cast<size_t>((m / (double)n) * std::log(2));
  num_hashes_ = num_hashes_ > 0 ? num_hashes_ : 1;
}

void bloom_filter::add(const slice& key) {
  std::pair<size_t, size_t> hashes = hash(key);
  for (size_t i = 0; i < num_hashes_; ++i) {
    // The formula for double hashing is (h1 + i * h2)
    size_t index = (hashes.first + i * hashes.second) % bit_array_.size();
    bit_array_[index] = true;
  }
}

bool bloom_filter::contains(const slice& key) const {
  auto hashes = hash(key);
  for (size_t i = 0; i < num_hashes_; ++i) {
    size_t index = (hashes.first + i * hashes.second) % bit_array_.size();
    if (!bit_array_[index]) {
      // We know that the key is definitely not present
      return false;
    }
  }
  // Possibly a false positive
  return true;
}