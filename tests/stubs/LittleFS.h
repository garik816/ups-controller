#pragma once

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <stdint.h>

// Deterministic host filesystem with shared file contents, sequential stream
// IO, rename semantics, and injected short writes / failed renames. It tests
// the production persistence flow without pretending to emulate flash timing.
class File {
public:
  File() : position_(0), writable_(false), write_left_(0) {}
  File(std::shared_ptr<std::string> bytes, bool writable, size_t limit)
      : bytes_(bytes), position_(0), writable_(writable), write_left_(limit) {}
  explicit operator bool() const { return static_cast<bool>(bytes_); }
  size_t size() const { return bytes_ ? bytes_->size() : 0; }
  int read() {
    if (!bytes_ || position_ >= bytes_->size()) return -1;
    return static_cast<unsigned char>((*bytes_)[position_++]);
  }
  size_t readBytes(char *buffer, size_t length) {
    if (!bytes_) return 0;
    const size_t count = std::min(length, bytes_->size() - position_);
    std::memcpy(buffer, bytes_->data() + position_, count);
    position_ += count;
    return count;
  }
  size_t write(uint8_t byte) { return write(&byte, 1); }
  size_t write(const uint8_t *buffer, size_t length) {
    if (!bytes_ || !writable_) return 0;
    const size_t count = std::min(length, write_left_);
    bytes_->append(reinterpret_cast<const char *>(buffer), count);
    position_ += count;
    write_left_ -= count;
    return count;
  }
  void flush() {}
  void close() { bytes_.reset(); }
private:
  std::shared_ptr<std::string> bytes_;
  size_t position_;
  bool writable_;
  size_t write_left_;
};

class MemoryLittleFS {
public:
  bool mount_ok = true;
  size_t write_limit = std::numeric_limits<size_t>::max();
  unsigned fail_rename_number = 0;

  bool begin() { return mount_ok; }
  bool exists(const char *path) const { return files_.count(path) != 0; }
  File open(const char *path, const char *mode) {
    if (!mount_ok) return File();
    if (mode[0] == 'w') {
      std::shared_ptr<std::string> bytes(new std::string());
      files_[path] = bytes;
      return File(bytes, true, write_limit);
    }
    const auto found = files_.find(path);
    return found == files_.end() ? File() : File(found->second, false, 0);
  }
  bool remove(const char *path) { return files_.erase(path) != 0; }
  bool rename(const char *from, const char *to) {
    if (fail_rename_number && --fail_rename_number == 0) return false;
    const auto found = files_.find(from);
    if (found == files_.end() || exists(to)) return false;
    files_[to] = found->second;
    files_.erase(found);
    return true;
  }
  void seed(const char *path, const std::string &text) {
    files_[path] = std::make_shared<std::string>(text);
  }
  std::string contents(const char *path) const {
    const auto found = files_.find(path);
    return found == files_.end() ? std::string() : *found->second;
  }
  void reset() {
    files_.clear();
    mount_ok = true;
    write_limit = std::numeric_limits<size_t>::max();
    fail_rename_number = 0;
  }
private:
  std::map<std::string, std::shared_ptr<std::string> > files_;
};

extern MemoryLittleFS LittleFS;
