#ifndef SMALLDB_LOG_H
#define SMALLDB_LOG_H

#include <iostream>
#include <mutex>

// The client thread and the background compaction thread both log, and
// iostreams aren't safe to share across threads, so serialize every line.
namespace smalldb_log {

inline std::mutex& mutex() {
  static std::mutex m;
  return m;
}

template <class... Args>
void info(const Args&... args) {
  std::lock_guard<std::mutex> lock(mutex());
  (std::cout << ... << args) << std::endl;
}

template <class... Args>
void error(const Args&... args) {
  std::lock_guard<std::mutex> lock(mutex());
  (std::cerr << ... << args) << std::endl;
}

} // namespace smalldb_log

#endif // SMALLDB_LOG_H
