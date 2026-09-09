#pragma once
#include <stddef.h>
#include <stdint.h>

// Accepted writes are NOT committed rows. At the existing flush cadence,
// close the writer (also commits file metadata), read back just this batch,
// then resume append. No full-file scan and no per-row close.
//
// A readback is evidence from the filesystem, not a guarantee against a
// defective card or power loss during a later FAT update. Hardware power-off
// tests and checkpoint timing measurements remain necessary.
class LogCheckpoint {
 public:
  void reset(size_t initialBytes) {
    committedBytes_ = expectedBytes_ = initialBytes;
    verifiedLines_ = pendingLines_ = 0;
    pendingHash_ = hashSeed;
    readbackBytes_ = 0;
    readbackSizeKnown_ = false;
    error_ = nullptr;
    ioError_ = 0;
  }

  unsigned long verifiedLines() const { return verifiedLines_; }
  bool pending() const { return pendingLines_ != 0; }
  const char* error() const { return error_; }
  int ioErrorNumber() const { return ioError_; }
  size_t expectedBytes() const { return expectedBytes_; }
  size_t committedBytes() const { return committedBytes_; }
  unsigned long pendingLines() const { return pendingLines_; }
  size_t readbackBytes() const { return readbackBytes_; }
  bool readbackSizeKnown() const { return readbackSizeKnown_; }

  template <typename FileType>
  bool append(FileType& file, const char* line, size_t len) {
    if (error_) return false;
    readbackSizeKnown_ = false; // previous checkpoint size is not this batch
    // A handle can hold a descriptor and still be unwritable - its mount torn
    // down, or an earlier error already recorded. Say so, with that errno,
    // instead of letting a write that never happened look like a short one.
    if (!file) { ioError_ = file.errorNumber(); error_ = "append handle"; return false; }
    if (len > SIZE_MAX - 2 || expectedBytes_ > SIZE_MAX - len - 2) {
      error_ = "file size overflow";
      return false;
    }
    const uint8_t newline[] = {'\r', '\n'};
    const auto* data = reinterpret_cast<const uint8_t*>(line);
    // Check CRLF as well: a partial newline is not a complete CSV row.
    size_t wrote = file.write(data, len);
    size_t wroteNewline = (wrote == len) ? file.write(newline, 2) : 0;
    if (wrote != len || wroteNewline != 2) {
      ioError_ = file.errorNumber();
      error_ = "short write";
      // Bytes the filesystem did accept are on the card. Count them so the
      // reported expected size is the file, not the last complete row.
      expectedBytes_ += wrote + wroteNewline;
      return false;
    }
    pendingHash_ = hash(pendingHash_, data, len);
    pendingHash_ = hash(pendingHash_, newline, 2);
    expectedBytes_ += len + 2;
    ++pendingLines_;
    return true;
  }

  template <typename FileType, typename FileSystem>
  bool commit(FileType& writer, FileSystem& fs, const char* name) {
    if (error_) { writer.close(); return false; }
    if (!pendingLines_) return true;
    if (!writer) { ioError_ = writer.errorNumber(); error_ = "checkpoint handle"; return false; }
    if (!writer.flush()) {
      ioError_ = writer.errorNumber(); error_ = "fsync";
      writer.close(); return false;
    }
    if (!writer.close()) {
      ioError_ = writer.errorNumber(); error_ = "close"; return false;
    }
    FileType reader = fs.open(name, "r");
    if (!reader) { ioError_ = reader.errorNumber(); error_ = "readback open"; return false; }
    if (!reader.querySize(readbackBytes_)) {
      ioError_ = reader.statErrorNumber(); reader.close();
      error_ = "readback stat"; return false;
    }
    readbackSizeKnown_ = true;
    if (readbackBytes_ != expectedBytes_) {
      reader.close(); error_ = "readback size"; return false;
    }
    if (!reader.seek(committedBytes_)) {
      ioError_ = reader.errorNumber(); reader.close(); error_ = "readback seek"; return false;
    }
    size_t remaining = expectedBytes_ - committedBytes_;
    uint32_t actualHash = hashSeed;
    uint8_t buffer[64];
    while (remaining) {
      size_t wanted = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
      int received = reader.read(buffer, wanted);
      if (received <= 0 || static_cast<size_t>(received) > wanted) {
        ioError_ = reader.errorNumber(); reader.close(); error_ = "readback data"; return false;
      }
      actualHash = hash(actualHash, buffer, static_cast<size_t>(received));
      remaining -= static_cast<size_t>(received);
    }
    if (!reader.close()) {
      ioError_ = reader.errorNumber(); error_ = "readback close"; return false;
    }
    if (actualHash != pendingHash_) {
      error_ = "readback checksum"; return false;
    }
    // Always append: opening with "w" would erase earlier checkpoints.
    writer = fs.open(name, "a");
    if (!writer) { ioError_ = writer.errorNumber(); error_ = "append reopen"; return false; }
    committedBytes_ = expectedBytes_;
    verifiedLines_ += pendingLines_;
    pendingLines_ = 0;
    pendingHash_ = hashSeed;
    return true;
  }

 private:
  static constexpr uint32_t hashSeed = 2166136261U;
  // FNV-1a: bounded-memory check for accidental corruption in this batch.
  static uint32_t hash(uint32_t value, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) value = (value ^ data[i]) * 16777619U;
    return value;
  }
  size_t committedBytes_ = 0, expectedBytes_ = 0;
  size_t readbackBytes_ = 0;
  bool readbackSizeKnown_ = false;
  unsigned long verifiedLines_ = 0, pendingLines_ = 0;
  uint32_t pendingHash_ = hashSeed;
  int ioError_ = 0;
  const char* error_ = nullptr;
};
