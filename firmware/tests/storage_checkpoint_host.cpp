// A file boundary fake, not a second logger implementation. Accepted writes
// remain volatile until close (the observed S/D workaround); powerLoss drops
// them. Faults can also lie about write success and about committed contents.
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include "LogCheckpoint.h"

struct Disk {
  std::string bytes = "header\r\n";
  bool loseOnClose = false, corruptOnClose = false;
  bool failSync = false, failClose = false;
  bool failReadOpen = false, failAppendOpen = false, failSeek = false;
  bool failRead = false, shortNewline = false;
  unsigned opens = 0, closes = 0, readBytes = 0;
};
struct Handle {
  Disk* disk;
  std::string contents;
  bool append, open = true;
  size_t cursor = 0;
};
struct FakeFile {
  std::shared_ptr<Handle> h;
  explicit operator bool() const { return h && h->open; }
  size_t write(const uint8_t* p, size_t n) {
    if (!*this || !h->append) return 0;
    if (h->disk->shortNewline && n == 2 && p[0] == '\r') --n;
    h->contents.append(reinterpret_cast<const char*>(p), n);
    return n;
  }
  size_t print(const char* text) { return write(reinterpret_cast<const uint8_t*>(text), std::strlen(text)); }
  size_t print(unsigned long value) { return print(std::to_string(value).c_str()); }
  size_t println(const char* text) { return print(text) + print("\r\n"); }
  struct Text : std::string {
    using std::string::string;
    bool startsWith(const char* prefix) const { return rfind(prefix, 0) == 0; }
  };
  Text readStringUntil(char delimiter) {
    Text out;
    while (h->cursor < h->contents.size()) {
      char c = h->contents[h->cursor++];
      if (c == delimiter) break;
      out.push_back(c);
    }
    return out;
  }
  size_t size() const { return h->contents.size(); }
  int ioError = 0;
  int errorNumber() const { return ioError; }
  bool flush() { if (h->disk->failSync) { ioError = EIO; return false; } return true; }
  bool close() {
    if (!*this) return true;
    auto& d = *h->disk;
    if (h->append && !d.loseOnClose) {
      d.bytes = h->contents;
      if (d.corruptOnClose && !d.bytes.empty()) d.bytes.back() ^= 1;
    }
    ++d.closes;
    h->open = false;
    if (d.failClose) { ioError = EIO; return false; }
    return true;
  }
  bool seek(size_t pos) {
    if (h->disk->failSeek || pos > h->contents.size()) return false;
    h->cursor = pos; return true;
  }
  int read(uint8_t* p, size_t n) {
    if (h->disk->failRead) return 0;
    n = std::min(n, h->contents.size() - h->cursor);
    std::memcpy(p, h->contents.data() + h->cursor, n);
    h->cursor += n; h->disk->readBytes += n;
    return static_cast<int>(n);
  }
  void powerLoss() { if (h) h->open = false; }
};
struct FakeFS {
  Disk disk;
  bool exists(const char*) { return false; }
  FakeFile open(const char*, const char* mode) {
    bool append = mode[0] != 'r';
    ++disk.opens;
    if ((append && disk.failAppendOpen) || (!append && disk.failReadOpen)) return {};
    return {std::make_shared<Handle>(Handle{&disk, mode[0] == 'w' ? "" : disk.bytes, append})};
  }
};

static void require(bool ok, const char* reason) {
  if (!ok) throw std::runtime_error(reason);
}

int main(int argc, char** argv) {
  try {
    require(argc == 2, "scenario required");
    std::string scenario = argv[1];
    FakeFS fs;
    FakeFile file = fs.open("/FLIGHT003.CSV", "a");
    LogCheckpoint checkpoint;
    checkpoint.reset(fs.disk.bytes.size());
    if (scenario == "partial_newline") fs.disk.shortNewline = true;
    const std::string row = "1,2.17,complete sensor row";
    bool accepted = checkpoint.append(file, row.c_str(), row.size());
    require(checkpoint.verifiedLines() == 0, "accepted write advertised as persisted");
    if (scenario == "partial_newline") {
      require(!accepted, "partial CRLF was accepted as a complete CSV row");
      require(!checkpoint.commit(file, fs, "/FLIGHT003.CSV"), "partial write committed");
      require(checkpoint.verifiedLines() == 0, "partial row counted");
      return 0;
    }
    require(accepted, "ordinary append failed");
    if (scenario == "volatile") {
      file.powerLoss();
      require(fs.disk.bytes == "header\r\n", "fake accidentally persisted before close");
      return 0;
    }
    fs.disk.loseOnClose = scenario == "silent_loss";
    fs.disk.corruptOnClose = scenario == "corrupt";
    fs.disk.failReadOpen = scenario == "read_open";
    fs.disk.failAppendOpen = scenario == "append_open";
    fs.disk.failRead = scenario == "read_failure";
    fs.disk.failSeek = scenario == "seek_failure";
    fs.disk.failSync = scenario == "sync_failure";
    fs.disk.failClose = scenario == "close_failure";
    bool fault = scenario != "persist" && scenario != "batches";
    require(checkpoint.commit(file, fs, "/FLIGHT003.CSV") == !fault,
            "checkpoint result did not reflect persistent data/reopen failure");
    if (fault) {
      require(checkpoint.verifiedLines() == 0, "failed checkpoint advanced count");
      require(!file, "failed checkpoint left writable handle active");
      require(checkpoint.error() != nullptr, "failure has no diagnosis");
      if (scenario == "sync_failure" || scenario == "close_failure")
        require(checkpoint.ioErrorNumber() == EIO, "lost checkpoint IO error");
      require(checkpoint.expectedBytes() == 8 + row.size() + 2, "wrong expected byte diagnostic");
      require(checkpoint.committedBytes() == 8, "lost checkpoint base diagnostic");
      require(checkpoint.pendingLines() == 1, "lost pending row diagnostic");
      if (scenario == "silent_loss") {
        require(checkpoint.readbackSizeKnown(), "missing observed size diagnostic");
        require(checkpoint.readbackBytes() == 8, "observed size not preserved after close");
      }
      if (scenario == "read_open")
        require(!checkpoint.readbackSizeKnown(), "read open failure pretends size was measured");
      return 0;
    }
    require(checkpoint.verifiedLines() == 1, "verified count missing");
    require(checkpoint.readbackSizeKnown(), "successful readback size missing");
    const std::string first = fs.disk.bytes;
    if (scenario == "batches") {
      unsigned before = fs.disk.readBytes;
      const std::string large(300, 'x');
      for (int i = 0; i < 10; ++i)
        require(checkpoint.append(file, large.c_str(), large.size()), "batch append failed");
      require(checkpoint.verifiedLines() == 1, "pending batch advertised prematurely");
      require(checkpoint.commit(file, fs, "/FLIGHT003.CSV"), "second checkpoint failed");
      require(checkpoint.verifiedLines() == 11, "second batch not counted");
      require(fs.disk.readBytes - before == 3020, "readback must read only new batch");
      std::string expected = first;
      for (int i = 0; i < 10; ++i) expected += large + "\r\n";
      require(fs.disk.bytes == expected, "checkpoint overwrote previous rows");
    }
    unsigned opens = fs.disk.opens;
    require(checkpoint.commit(file, fs, "/FLIGHT003.CSV"), "empty checkpoint failed");
    require(fs.disk.opens == opens, "empty checkpoint reopened file unnecessarily");
    const auto durable = fs.disk.bytes;
    require(checkpoint.append(file, row.c_str(), row.size()), "post-checkpoint append failed");
    require(!checkpoint.readbackSizeKnown(), "new append exposes stale prior readback size");
    file.powerLoss(); // no S, D, or final graceful close
    require(fs.disk.bytes == durable, "power-off lost a verified batch");
    require(durable.find(row) != std::string::npos, "first checkpoint has no data");
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n'; return 1;
  }
}
