#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include "CheckedLogFile.h"
#include "LogCheckpoint.h"

struct FaultOps : LogPosixOps {
  static bool writeFail, syncFail, closeFail, statFail;
  static ssize_t write(int fd, const void* p, size_t n) {
    if (writeFail) { errno = ENOSPC; return -1; }
    return LogPosixOps::write(fd, p, n);
  }
  static int sync(int fd) {
    if (syncFail) { errno = EIO; return -1; }
    return LogPosixOps::sync(fd);
  }
  static int close(int fd) {
    int rc = LogPosixOps::close(fd); // still release the real test fd
    if (closeFail) { errno = EIO; return -1; }
    return rc;
  }
  static int stat(int fd, struct stat* s) {
    if (statFail) { errno = EBADF; return -1; }
    return LogPosixOps::stat(fd, s);
  }
};
bool FaultOps::writeFail = false;
bool FaultOps::syncFail = false;
bool FaultOps::closeFail = false;
bool FaultOps::statFail = false;
static void require(bool value, const char* msg) {
  if (!value) throw std::runtime_error(msg);
}
int main(int argc, char** argv) {
  try {
    require(argc == 3, "scenario and isolated mount directory required");
    const std::string scenario = argv[1];
    BasicLogFileSystem<FaultOps> fs(argv[2]);
    auto file = fs.open("/FLIGHT001.CSV", "w");
    require(bool(file), "open create failed");
    require(file.println("PKT,T") == 7, "header write failed");
    require(file.flush() && file.close(), "header commit failed");
    file = fs.open("/FLIGHT001.CSV", "a");
    require(bool(file), "append open failed");
    if (scenario.rfind("checkpoint_", 0) == 0) {
      LogCheckpoint checkpoint;
      checkpoint.reset(7);
      require(checkpoint.append(file, "1,2.17", 6), "checkpoint append failed");
      FaultOps::syncFail = scenario == "checkpoint_sync";
      FaultOps::closeFail = scenario == "checkpoint_close";
      bool expected = scenario == "checkpoint_ok";
      require(checkpoint.commit(file, fs, "/FLIGHT001.CSV") == expected,
              "real adapter/checkpoint result incorrect");
      require(checkpoint.verifiedLines() == (expected ? 1UL : 0UL), "false verified count");
      if (!expected) {
        require(checkpoint.ioErrorNumber() == EIO, "lost real checkpoint errno");
        require(!file, "failed checkpoint left writer live");
      }
      return 0;
    }
    if (scenario == "stat_error") {
      // The status line's only job is to report. A card that will not answer
      // fstat must show up in that field, not stop the flight log.
      FaultOps::statFail = true;
      size_t bytes = 1;
      require(!file.querySize(bytes) && bytes == 0, "stat failure not observed");
      require(file.statErrorNumber() == EBADF, "stat errno lost");
      require(bool(file) && file.errorNumber() == 0, "a size report disabled the writer");
      FaultOps::statFail = false;
      require(file.println("1,2.17") == 8, "write refused after a failed size report");
      require(file.flush() && file.close(), "commit refused after a failed size report");
      file = fs.open("/FLIGHT001.CSV", "r");
      require(file.querySize(bytes) && bytes == 15, "row lost after a failed size report");
      return 0;
    }
    if (scenario == "dead_handle") {
      FaultOps::writeFail = true;
      require(file.println("1,2.17") == 0, "write failure not injected");
      FaultOps::writeFail = false;
      require(!file, "a handle that cannot write still tested as writable");
      require(file.isOpen(), "the descriptor is still owned and must still be closed");
      LogCheckpoint checkpoint;
      checkpoint.reset(7);
      require(!checkpoint.append(file, "1,2.17", 6), "dead handle accepted a row");
      require(std::strcmp(checkpoint.error(), "append handle") == 0,
              "a write that never happened was reported as a short write");
      require(checkpoint.ioErrorNumber() == ENOSPC, "dead handle lost the real errno");
      require(checkpoint.expectedBytes() == 7, "nothing was written, so nothing is expected");
      return 0;
    }
    if (scenario == "write_error") FaultOps::writeFail = true;
    size_t written = file.println("1,2.17");
    if (scenario == "write_error") {
      require(written == 0 && file.errorNumber() == ENOSPC, "lost real write errno");
      file.close();
      require(file.errorNumber() == ENOSPC, "cleanup erased first error");
      return 0;
    }
    require(written == 8, "append byte count wrong");
    size_t onDisk = 0;
    require(file.querySize(onDisk) && onDisk == 15, "direct write hidden in stdio buffer");
    if (scenario == "sync_error") {
      FaultOps::syncFail = true;
      require(!file.flush() && file.errorNumber() == EIO, "fsync failure hidden");
      file.close();
      require(file.errorNumber() == EIO, "close erased sync failure");
      return 0;
    }
    require(file.flush(), "sync failed");
    if (scenario == "close_error") {
      FaultOps::closeFail = true;
      require(!file.close() && file.errorNumber() == EIO, "close failure hidden");
      require(!file, "failed close kept descriptor live");
      return 0;
    }
    require(file.close(), "close failed");
    file = fs.open("/FLIGHT001.CSV", "r");
    require(file.querySize(onDisk) && onDisk == 15, "reopen lost appended row");
    uint8_t bytes[15];
    require(file.read(bytes, sizeof(bytes)) == 15, "readback short");
    require(std::memcmp(bytes, "PKT,T\r\n1,2.17\r\n", 15) == 0, "append overwrote file");
    require(file.seek(7), "seek failed");
    require(file.read(bytes, 8) == 8 && std::memcmp(bytes, "1,2.17\r\n", 8) == 0, "seek/read failed");
    auto moved = std::move(file);
    require(!file && bool(moved), "file ownership was copied, not moved");
    require(moved.close(), "moved handle close failed");
    auto missing = fs.open("/missing.csv", "r");
    require(!missing && missing.errorNumber() == ENOENT, "open error missing");
    static_assert(!std::is_copy_constructible<decltype(file)>::value, "must not duplicate fd owner");
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n'; return 1;
  }
}
