#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace lsm {
namespace crc32c {

// CRC-32C (Castagnoli, polynomial 0x1EDC6F41). Used to detect torn and
// bit-rotted records: every WAL record and every SSTable block carries one.
uint32_t Extend(uint32_t crc, const char* data, size_t n);

inline uint32_t Value(const char* data, size_t n) { return Extend(0, data, n); }
inline uint32_t Value(std::string_view s) { return Extend(0, s.data(), s.size()); }

}  // namespace crc32c
}  // namespace lsm
