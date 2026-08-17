#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "lsm/status.h"

namespace lsm {

// Cursor over one prefix-compressed data block.
//
// Entries store only the bytes by which their key differs from the previous
// one, so a key can only be materialised by decoding forward from a restart
// point. That is why this is a cursor rather than a random-access accessor, and
// why both the point-lookup path and the table iterator go through it instead
// of each parsing entries themselves.
//
// The block bytes are borrowed, not owned -- the caller holds the cache handle
// that keeps them alive.
class BlockReader {
 public:
  BlockReader() = default;

  // `contents` is a data block without its CRC trailer. Check status() before
  // use: a malformed restart array is rejected here rather than mid-iteration.
  explicit BlockReader(std::string_view contents);

  bool Valid() const { return valid_; }
  Status status() const { return status_; }
  uint32_t num_restarts() const { return num_restarts_; }

  void SeekToFirst();
  // Positions at the first entry with key >= target.
  void Seek(std::string_view target);
  void Next();

  // Valid() must be true. The key points into an internal buffer that the next
  // Next()/Seek() overwrites; the value points into the block itself.
  std::string_view key() const { return key_; }
  std::string_view value() const { return value_; }

 private:
  uint32_t RestartOffset(uint32_t index) const;
  // Decodes the entry at `offset`, given the previous key for prefix reuse.
  // Returns false at the end of the entry region or on a malformed entry.
  bool ParseEntryAt(uint32_t offset);
  void SeekToRestart(uint32_t index);

  const char* data_ = nullptr;   // Start of the entry region.
  uint32_t entries_size_ = 0;    // Bytes of entries, excluding the trailer.
  const char* restarts_ = nullptr;
  uint32_t num_restarts_ = 0;

  uint32_t current_ = 0;  // Offset of the entry being pointed at.
  uint32_t next_ = 0;     // Offset just past it.
  std::string key_;       // Reconstructed key for the current entry.
  std::string_view value_;
  bool valid_ = false;
  Status status_;
};

// Builds the block format BlockReader consumes. Keys must arrive sorted.
class BlockBuilder {
 public:
  explicit BlockBuilder(int restart_interval)
      : restart_interval_(restart_interval < 1 ? 1 : restart_interval) {}

  void Add(std::string_view key, std::string_view value);

  // Appends the restart array and returns the finished block. The builder is
  // reset and ready for the next block.
  std::string Finish();

  void Reset();

  bool empty() const { return entries_ == 0; }
  // Size the block would occupy if finished now, used to decide when to cut.
  size_t CurrentSizeEstimate() const {
    return buffer_.size() + (restarts_.size() + 1) * sizeof(uint32_t);
  }

 private:
  const int restart_interval_;
  std::string buffer_;
  std::vector<uint32_t> restarts_{0};
  std::string last_key_;
  int since_restart_ = 0;
  int entries_ = 0;
};

}  // namespace lsm
