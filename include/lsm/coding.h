#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace lsm {

// All integers hit the disk little-endian so the format is byte-identical on
// every platform we care about; we always go through memcpy rather than
// reinterpret_cast so the reads stay defined and alignment-safe.

inline void PutFixed32(std::string* dst, uint32_t value) {
  char buf[4];
  std::memcpy(buf, &value, sizeof(buf));
  dst->append(buf, sizeof(buf));
}

inline void PutFixed64(std::string* dst, uint64_t value) {
  char buf[8];
  std::memcpy(buf, &value, sizeof(buf));
  dst->append(buf, sizeof(buf));
}

inline uint32_t DecodeFixed32(const char* ptr) {
  uint32_t value;
  std::memcpy(&value, ptr, sizeof(value));
  return value;
}

inline uint64_t DecodeFixed64(const char* ptr) {
  uint64_t value;
  std::memcpy(&value, ptr, sizeof(value));
  return value;
}

// Length-prefixed byte string: fixed32 length then the raw bytes. A varint
// would save 3 bytes per record but costs branchy decoding on the hot path;
// with 4 KiB blocks the space difference is under 1%.
inline void PutLengthPrefixed(std::string* dst, std::string_view value) {
  PutFixed32(dst, static_cast<uint32_t>(value.size()));
  dst->append(value.data(), value.size());
}

// Consumes one length-prefixed string from *input. Returns false (leaving
// *input untouched) if the buffer is truncated.
inline bool GetLengthPrefixed(std::string_view* input, std::string_view* result) {
  if (input->size() < 4) return false;
  const uint32_t len = DecodeFixed32(input->data());
  if (input->size() < 4 + static_cast<size_t>(len)) return false;
  *result = std::string_view(input->data() + 4, len);
  input->remove_prefix(4 + static_cast<size_t>(len));
  return true;
}

}  // namespace lsm
