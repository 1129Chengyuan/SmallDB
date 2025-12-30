//
// Created by Cheng-Yuan Li on 2025/12/30.
//

#ifndef SLICE_H
#define SLICE_H

#include <string>
#include <cstring>

// Bypass the pass-by-value cost of std::string
class Slice {
public:
  // Pointer and size
  Slice(const char* data, size_t size) {
    data_ = data;
    size_ = size;
  }

  Slice(const std::string& s) {
    data_ = s.data();
    size_ = s.size();
  }

  // Getter methods
  const char* data() const { return data_; }
  size_t size() const { return size_; }

  // compareTo function like in Java: -1 if less, 0 if equal, 1 if greater
  int compareTo(const Slice& other) const {

    // find min length to compare, only use min length to check
    const size_t min_len = size_ < other.size_ ? size_ : other.size_;

    int result = std::memcmp(data_, other.data_, min_len);
    // If result is equal, check the length
    if (result == 0) {
      if (size_ < other.size_)
        return -1;
      if (size_ > other.size_)
        return 1;
      return 0;
    }
  }

private:
  const char* data_;
  size_t size_;
};

#endif //SLICE_H
