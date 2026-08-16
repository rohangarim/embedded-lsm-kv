#pragma once

#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "lsm/coding.h"

namespace lsm {

using SequenceNumber = uint64_t;

// Sequence numbers occupy the high 56 bits of the trailing tag, so they cap out
// well beyond any realistic write volume.
constexpr SequenceNumber kMaxSequenceNumber = (1ull << 56) - 1;

enum class ValueType : uint8_t {
  kDeletion = 0x0,
  kValue = 0x1,
};

// A key as stored in the memtable and in SSTables: the user key followed by an
// 8-byte tag of (sequence << 8 | type). Carrying the sequence number all the
// way down is what lets a newer Put shadow an older one across levels without
// either physically overwriting the other -- compaction resolves it later.
constexpr size_t kTagSize = 8;

inline uint64_t PackTag(SequenceNumber seq, ValueType type) {
  assert(seq <= kMaxSequenceNumber);
  return (seq << 8) | static_cast<uint8_t>(type);
}

inline std::string EncodeInternalKey(std::string_view user_key, SequenceNumber seq,
                                     ValueType type) {
  std::string out;
  out.reserve(user_key.size() + kTagSize);
  // Empty user keys are legal, and an empty string_view's data() may be null,
  // which append() is not required to tolerate.
  if (!user_key.empty()) out.append(user_key.data(), user_key.size());
  PutFixed64(&out, PackTag(seq, type));
  return out;
}

inline std::string_view ExtractUserKey(std::string_view internal_key) {
  assert(internal_key.size() >= kTagSize);
  return internal_key.substr(0, internal_key.size() - kTagSize);
}

inline uint64_t ExtractTag(std::string_view internal_key) {
  assert(internal_key.size() >= kTagSize);
  return DecodeFixed64(internal_key.data() + internal_key.size() - kTagSize);
}

inline SequenceNumber ExtractSequence(std::string_view internal_key) {
  return ExtractTag(internal_key) >> 8;
}

inline ValueType ExtractValueType(std::string_view internal_key) {
  return static_cast<ValueType>(ExtractTag(internal_key) & 0xffu);
}

// Orders by user key ascending, then by sequence number *descending* so that
// the newest version of a key sorts first. A forward scan therefore sees the
// live value before any of the versions it shadows.
inline int CompareInternalKey(std::string_view a, std::string_view b) {
  const int r = ExtractUserKey(a).compare(ExtractUserKey(b));
  if (r != 0) return r;
  const uint64_t ta = ExtractTag(a);
  const uint64_t tb = ExtractTag(b);
  if (ta > tb) return -1;
  if (ta < tb) return 1;
  return 0;
}

// Three-way comparator, as the skip list wants.
struct InternalKeyComparator {
  int operator()(std::string_view a, std::string_view b) const {
    return CompareInternalKey(a, b);
  }
};

// Strict-weak-ordering form, for std::map / std::sort.
struct InternalKeyLess {
  bool operator()(std::string_view a, std::string_view b) const {
    return CompareInternalKey(a, b) < 0;
  }
};

// Smallest internal key that sorts at or after every version of `user_key`
// visible at `seq`. Seeking to this lands on the newest visible version.
inline std::string LookupKey(std::string_view user_key, SequenceNumber seq) {
  return EncodeInternalKey(user_key, seq, ValueType::kValue);
}

// Stack-backed builder for the same thing.
//
// Every point lookup needs a throwaway internal key, and going through
// std::string meant a malloc and free on the hottest path in the engine. Keys
// up to kInlineCapacity are assembled in place; anything longer falls back to
// the heap, so correctness never depends on the bound being generous enough.
class LookupKeyBuffer {
 public:
  static constexpr size_t kInlineCapacity = 200;

  LookupKeyBuffer(std::string_view user_key, SequenceNumber seq,
                  ValueType type = ValueType::kValue) {
    const size_t total = user_key.size() + kTagSize;
    char* out;
    if (total <= kInlineCapacity) {
      out = inline_buf_;
    } else {
      heap_.resize(total);
      out = heap_.data();
    }
    if (!user_key.empty()) {
      std::memcpy(out, user_key.data(), user_key.size());
    }
    const uint64_t tag = PackTag(seq, type);
    std::memcpy(out + user_key.size(), &tag, sizeof(tag));
    key_ = std::string_view(out, total);
  }

  LookupKeyBuffer(const LookupKeyBuffer&) = delete;
  LookupKeyBuffer& operator=(const LookupKeyBuffer&) = delete;

  std::string_view key() const { return key_; }
  operator std::string_view() const { return key_; }

 private:
  char inline_buf_[kInlineCapacity];
  std::string heap_;
  std::string_view key_;
};

}  // namespace lsm
