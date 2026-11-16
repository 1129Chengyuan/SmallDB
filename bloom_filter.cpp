#include "bloom_filter.h"
#include "coding.h"

bloom_filter::bloom_filter(size_t n, double p) {
  if (n == 0) n = 1;
  // Optimal size calculation: m = -n ln p / (ln 2)^2
  auto m = static_cast<size_t>(-(static_cast<double>(n) * std::log(p)) / (std::log(2) * std::log(2)));
  num_bits_ = m < 64 ? 64 : m;
  bits_.assign((num_bits_ + 7) / 8, 0);

  // Number of hash functions: k = (m / n) ln 2
  auto k = static_cast<uint32_t>((static_cast<double>(num_bits_) / n) * std::log(2));
  num_hashes_ = k < 1 ? 1 : (k > 30 ? 30 : k);
}

uint64_t bloom_filter::hash(const slice& key) {
  // FNV-1a over the bytes...
  uint64_t h = 1469598103934665603ULL;
  const auto* p = reinterpret_cast<const uint8_t*>(key.data());
  for (size_t i = 0; i < key.size(); ++i) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  // ...then a splitmix64 finalizer so every input bit affects every output bit
  h ^= h >> 30;
  h *= 0xBF58476D1CE4E5B9ULL;
  h ^= h >> 27;
  h *= 0x94D049BB133111EBULL;
  h ^= h >> 31;
  return h;
}

void bloom_filter::add_hash(uint64_t h) {
  // Double hashing: index_i = h1 + i * h2 (h2 odd)
  uint64_t h1 = h;
  uint64_t h2 = ((h >> 32) | (h << 32)) | 1;
  for (uint32_t i = 0; i < num_hashes_; ++i) {
    size_t bit = (h1 + i * h2) % num_bits_;
    bits_[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
  }
}

bool bloom_filter::contains_hash(uint64_t h) const {
  uint64_t h1 = h;
  uint64_t h2 = ((h >> 32) | (h << 32)) | 1;
  for (uint32_t i = 0; i < num_hashes_; ++i) {
    size_t bit = (h1 + i * h2) % num_bits_;
    if (!(bits_[bit / 8] & (1u << (bit % 8)))) {
      // We know that the key is definitely not present
      return false;
    }
  }
  // Possibly a false positive
  return true;
}

std::string bloom_filter::serialize() const {
  std::string out;
  coding::put_fixed32(out, num_hashes_);
  coding::put_fixed64(out, num_bits_);
  out.append(reinterpret_cast<const char*>(bits_.data()), bits_.size());
  return out;
}

std::unique_ptr<bloom_filter> bloom_filter::deserialize(const char* data, size_t n) {
  if (n < 12) return nullptr;
  uint32_t k = coding::decode_fixed32(data);
  uint64_t m = coding::decode_fixed64(data + 4);
  if (k < 1 || k > 30 || m == 0 || (m + 7) / 8 != n - 12) return nullptr;

  std::unique_ptr<bloom_filter> f(new bloom_filter());
  f->num_hashes_ = k;
  f->num_bits_ = static_cast<size_t>(m);
  f->bits_.assign(reinterpret_cast<const uint8_t*>(data + 12),
                  reinterpret_cast<const uint8_t*>(data + n));
  return f;
}
