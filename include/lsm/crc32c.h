#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace lsm {
namespace crc32c {

// CRC-32C (Castagnoli, polynomial 0x1EDC6F41). Used to detect torn and
// bit-rotted records: every WAL record and every SSTable block carries one.
//
// Uses the CPU's CRC32C instruction where the build targets one (ARMv8 CRC or
// SSE4.2) and slicing-by-8 tables otherwise. Both produce identical values --
// this is a checksum on disk, so a mismatch between builds would make files
// unreadable.
uint32_t Extend(uint32_t crc, const char* data, size_t n);

// The table-driven implementation, always available. Exposed so the tests can
// check the hardware path against it rather than trusting it blindly.
uint32_t ExtendPortable(uint32_t crc, const char* data, size_t n);

bool UsingHardwareAcceleration();

inline uint32_t Value(const char* data, size_t n) { return Extend(0, data, n); }
inline uint32_t Value(std::string_view s) { return Extend(0, s.data(), s.size()); }

}  // namespace crc32c
}  // namespace lsm
