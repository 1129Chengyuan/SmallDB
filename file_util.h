#ifndef SMALLDB_FILE_UTIL_H
#define SMALLDB_FILE_UTIL_H

#include <fcntl.h>
#include <unistd.h>
#include <string>

// std::ofstream::flush() only hands bytes to the OS page cache. These helpers
// force them to stable storage so a power loss can't lose acknowledged data.
// (On macOS, fsync doesn't flush the drive's own cache; F_FULLFSYNC would.)
namespace file_util {

inline bool sync_path(const std::string& path, int flags) {
  int fd = ::open(path.c_str(), flags);
  if (fd < 0) return false;
  bool ok = ::fsync(fd) == 0;
  ::close(fd);
  return ok;
}

// fsync a regular file's contents
inline bool sync_file(const std::string& path) {
  return sync_path(path, O_RDONLY);
}

// fsync a directory so newly created / renamed entries survive a crash
inline bool sync_dir(const std::string& dir) {
  return sync_path(dir, O_RDONLY | O_DIRECTORY);
}

} // namespace file_util

#endif // SMALLDB_FILE_UTIL_H
