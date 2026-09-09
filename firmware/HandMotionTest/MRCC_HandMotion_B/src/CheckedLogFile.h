#pragma once
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// The SD library still mounts the card. Logging uses that same /sd VFS mount
// through descriptors: no extra FILE* buffer, and write/fsync/close errors are
// observable. This does not bypass FatFs/the card's own cache or prove power
// loss safety. Explicit readback and physical acceptance are still required.
struct LogPosixOps {
  static int open(const char* path, int flags) { return ::open(path, flags, 0666); }
  static ssize_t write(int fd, const void* p, size_t n) { return ::write(fd, p, n); }
  static ssize_t read(int fd, void* p, size_t n) { return ::read(fd, p, n); }
  static int sync(int fd) { return ::fsync(fd); }
  static int close(int fd) { return ::close(fd); }
  static int stat(int fd, struct stat* s) { return ::fstat(fd, s); }
  static off_t seek(int fd, off_t offset) { return ::lseek(fd, offset, SEEK_SET); }
};

template <typename Ops = LogPosixOps>
class BasicCheckedLogFile {
 public:
  BasicCheckedLogFile() = default;
  ~BasicCheckedLogFile() { close(); }
  BasicCheckedLogFile(const BasicCheckedLogFile&) = delete;
  BasicCheckedLogFile& operator=(const BasicCheckedLogFile&) = delete;
  BasicCheckedLogFile(BasicCheckedLogFile&& other) noexcept { take(other); }
  BasicCheckedLogFile& operator=(BasicCheckedLogFile&& other) noexcept {
    if (this != &other) { close(); take(other); }
    return *this;
  }
  explicit operator bool() const { return fd_ >= 0; }
  int errorNumber() const { return error_; }

  static BasicCheckedLogFile open(const char* path, int flags) {
    BasicCheckedLogFile file;
    file.fd_ = Ops::open(path, flags);
    if (file.fd_ < 0) file.fail(errno);
    return file;
  }
  static BasicCheckedLogFile invalid(int code) {
    BasicCheckedLogFile file; file.fail(code); return file;
  }
  size_t write(const uint8_t* p, size_t n) {
    if (error_) return 0;
    if (fd_ < 0) { fail(EBADF); return 0; }
    ssize_t written = Ops::write(fd_, p, n);
    if (written < 0) { fail(errno); return 0; }
    if (static_cast<size_t>(written) != n) fail(EIO);
    return static_cast<size_t>(written);
  }
  size_t print(const char* s) {
    return write(reinterpret_cast<const uint8_t*>(s), strlen(s));
  }
  size_t print(unsigned long value) {
    char text[24];
    snprintf(text, sizeof(text), "%lu", value);
    return print(text);
  }
  size_t println(const char* s) {
    size_t n = print(s);
    if (n != strlen(s)) return n;
    return n + print("\r\n");
  }
  bool flush() {
    if (fd_ < 0) { fail(EBADF); return false; }
    if (Ops::sync(fd_) != 0) { fail(errno); return false; }
    return error_ == 0;
  }
  bool close() {
    if (fd_ < 0) return true;
    int fd = fd_;
    fd_ = -1; // do not retry close on an fd that may already have been reused
    if (Ops::close(fd) != 0) { fail(errno); return false; }
    return true;
  }
  size_t size() const {
    struct stat info;
    if (Ops::stat(fd_, &info) != 0) { fail(errno); return 0; }
    if (info.st_size < 0) { fail(EIO); return 0; }
    return static_cast<size_t>(info.st_size);
  }
  bool seek(size_t offset) {
    if (offset > static_cast<size_t>(LONG_MAX)) { fail(EOVERFLOW); return false; }
    if (Ops::seek(fd_, static_cast<off_t>(offset)) < 0) { fail(errno); return false; }
    return true;
  }
  int read(uint8_t* p, size_t n) {
    if (n > INT_MAX) { fail(EOVERFLOW); return -1; }
    ssize_t received = Ops::read(fd_, p, n);
    if (received < 0) { fail(errno); return -1; }
    return static_cast<int>(received);
  }

 private:
  void fail(int code) const { if (!error_) error_ = code ? code : EIO; }
  void take(BasicCheckedLogFile& other) {
    fd_ = other.fd_; error_ = other.error_; other.fd_ = -1;
  }
  int fd_ = -1;
  mutable int error_ = 0;
};

template <typename Ops = LogPosixOps>
class BasicLogFileSystem {
 public:
  explicit BasicLogFileSystem(const char* mount = "/sd") : mount_(mount) {}
  BasicCheckedLogFile<Ops> open(const char* name, const char* mode) {
    int flags;
    if (strcmp(mode, "r") == 0) flags = O_RDONLY;
    else if (strcmp(mode, "w") == 0) flags = O_WRONLY | O_CREAT | O_TRUNC;
    else if (strcmp(mode, "a") == 0) flags = O_WRONLY | O_APPEND;
    else return BasicCheckedLogFile<Ops>::invalid(EINVAL);
    char path[128];
    int n = snprintf(path, sizeof(path), "%s%s", mount_, name);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(path))
      return BasicCheckedLogFile<Ops>::invalid(ENAMETOOLONG);
    return BasicCheckedLogFile<Ops>::open(path, flags);
  }
 private:
  const char* mount_;
};

using CheckedLogFile = BasicCheckedLogFile<>;
using LogFileSystem = BasicLogFileSystem<>;
