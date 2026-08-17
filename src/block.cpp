#include "lsm/block.h"

#include <algorithm>
#include <cstring>

#include "lsm/coding.h"
#include "lsm/internal_key.h"

namespace lsm {

// ------------------------------------------------------------- BlockBuilder

void BlockBuilder::Add(std::string_view key, std::string_view value) {
  size_t shared = 0;
  if (since_restart_ < restart_interval_) {
    // How much of the previous key this one reuses. Internal keys here run
    // like "user0000000000123456" plus an 8-byte tag, so neighbours typically
    // share everything but the last few digits.
    const size_t limit = std::min(last_key_.size(), key.size());
    while (shared < limit && last_key_[shared] == key[shared]) ++shared;
  } else {
    // Start a new restart run: this key is stored whole so a binary search can
    // land here without decoding anything before it.
    restarts_.push_back(static_cast<uint32_t>(buffer_.size()));
    since_restart_ = 0;
  }

  const size_t non_shared = key.size() - shared;
  PutVarint32(&buffer_, static_cast<uint32_t>(shared));
  PutVarint32(&buffer_, static_cast<uint32_t>(non_shared));
  PutVarint32(&buffer_, static_cast<uint32_t>(value.size()));
  buffer_.append(key.data() + shared, non_shared);
  if (!value.empty()) buffer_.append(value.data(), value.size());

  last_key_.assign(key);
  ++since_restart_;
  ++entries_;
}

std::string BlockBuilder::Finish() {
  for (const uint32_t offset : restarts_) PutFixed32(&buffer_, offset);
  PutFixed32(&buffer_, static_cast<uint32_t>(restarts_.size()));

  std::string finished;
  finished.swap(buffer_);
  Reset();
  return finished;
}

void BlockBuilder::Reset() {
  buffer_.clear();
  restarts_.assign(1, 0);
  last_key_.clear();
  since_restart_ = 0;
  entries_ = 0;
}

// -------------------------------------------------------------- BlockReader

BlockReader::BlockReader(std::string_view contents) {
  if (contents.size() < sizeof(uint32_t)) {
    status_ = Status::Corruption("data block shorter than its restart count");
    return;
  }
  const uint32_t num_restarts =
      DecodeFixed32(contents.data() + contents.size() - sizeof(uint32_t));
  // The restart array plus its count must fit in what is left of the block.
  const size_t trailer = (static_cast<size_t>(num_restarts) + 1) * sizeof(uint32_t);
  if (num_restarts == 0 || trailer > contents.size()) {
    status_ = Status::Corruption("data block has a malformed restart array");
    return;
  }

  data_ = contents.data();
  entries_size_ = static_cast<uint32_t>(contents.size() - trailer);
  restarts_ = contents.data() + entries_size_;
  num_restarts_ = num_restarts;

  // Every restart offset must point inside the entry region.
  for (uint32_t i = 0; i < num_restarts_; ++i) {
    if (RestartOffset(i) > entries_size_) {
      status_ = Status::Corruption("data block restart offset out of range");
      data_ = nullptr;
      return;
    }
  }
}

uint32_t BlockReader::RestartOffset(uint32_t index) const {
  return DecodeFixed32(restarts_ + index * sizeof(uint32_t));
}

bool BlockReader::ParseEntryAt(uint32_t offset) {
  if (offset >= entries_size_) {
    valid_ = false;
    return false;
  }
  const char* p = data_ + offset;
  const char* limit = data_ + entries_size_;

  uint32_t shared = 0, non_shared = 0, value_len = 0;
  p = GetVarint32Ptr(p, limit, &shared);
  if (p != nullptr) p = GetVarint32Ptr(p, limit, &non_shared);
  if (p != nullptr) p = GetVarint32Ptr(p, limit, &value_len);
  if (p == nullptr || static_cast<size_t>(limit - p) < non_shared + value_len) {
    status_ = Status::Corruption("truncated data block entry");
    valid_ = false;
    return false;
  }
  // A key cannot reuse more bytes than the previous one had.
  if (shared > key_.size()) {
    status_ = Status::Corruption("data block entry shares more than it can");
    valid_ = false;
    return false;
  }

  key_.resize(shared);
  key_.append(p, non_shared);
  value_ = std::string_view(p + non_shared, value_len);

  current_ = offset;
  next_ = static_cast<uint32_t>((p - data_) + non_shared + value_len);
  valid_ = true;
  return true;
}

void BlockReader::SeekToRestart(uint32_t index) {
  key_.clear();  // A restart entry stores its key whole, so nothing to reuse.
  ParseEntryAt(RestartOffset(index));
}

void BlockReader::SeekToFirst() {
  if (data_ == nullptr) return;
  SeekToRestart(0);
}

void BlockReader::Next() {
  if (!valid_) return;
  ParseEntryAt(next_);
}

void BlockReader::Seek(std::string_view target) {
  if (data_ == nullptr) return;

  // Binary search the restart points for the last one whose key is < target,
  // then walk forward. Only restart keys can be compared without decoding
  // everything before them, which is the whole reason they exist.
  uint32_t left = 0;
  uint32_t right = num_restarts_ - 1;
  while (left < right) {
    const uint32_t mid = left + (right - left + 1) / 2;
    key_.clear();
    if (!ParseEntryAt(RestartOffset(mid))) {
      valid_ = false;
      return;
    }
    if (CompareInternalKey(key_, target) < 0) {
      left = mid;
    } else {
      right = mid - 1;
    }
  }

  SeekToRestart(left);
  while (valid_ && CompareInternalKey(key_, target) < 0) Next();
}

}  // namespace lsm
