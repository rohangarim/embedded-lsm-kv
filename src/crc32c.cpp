#include "lsm/crc32c.h"

#include <array>
#include <cstring>

#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#endif
#if defined(__x86_64__) && defined(__SSE4_2__)
#include <nmmintrin.h>
#endif

namespace lsm {
namespace crc32c {
namespace {

// Bit-reflected Castagnoli polynomial. Tables are built at static-init time
// rather than hand-written so the constant stays auditable; correctness is
// pinned by the RFC 3720 known-answer tests.
constexpr uint32_t kPoly = 0x82f63b78u;

// Slicing-by-8: table[k] folds in a byte that is k positions further ahead, so
// eight input bytes can be consumed per iteration instead of one. A byte-at-a-
// time CRC costs roughly a nanosecond per byte, which on 4 KiB blocks is enough
// to dominate a cached read -- this was measured as ~9 microseconds of the p95
// on read-heavy workloads before it was fixed.
using Table = std::array<std::array<uint32_t, 256>, 8>;

Table MakeTables() {
  Table table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t crc = i;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) ? (crc >> 1) ^ kPoly : (crc >> 1);
    }
    table[0][i] = crc;
  }
  for (uint32_t i = 0; i < 256; ++i) {
    for (int k = 1; k < 8; ++k) {
      const uint32_t previous = table[k - 1][i];
      table[k][i] = (previous >> 8) ^ table[0][previous & 0xffu];
    }
  }
  return table;
}

const Table& Tables() {
  static const Table table = MakeTables();
  return table;
}

inline uint32_t LoadLE32(const unsigned char* p) {
  uint32_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

inline uint64_t LoadLE64(const unsigned char* p) {
  uint64_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

uint32_t ExtendSoftware(uint32_t crc, const char* data, size_t n) {
  const Table& t = Tables();
  uint32_t c = ~crc;
  const auto* p = reinterpret_cast<const unsigned char*>(data);

  while (n >= 8) {
    c ^= LoadLE32(p);
    const uint32_t high = LoadLE32(p + 4);
    c = t[7][c & 0xffu] ^ t[6][(c >> 8) & 0xffu] ^ t[5][(c >> 16) & 0xffu] ^
        t[4][(c >> 24) & 0xffu] ^ t[3][high & 0xffu] ^ t[2][(high >> 8) & 0xffu] ^
        t[1][(high >> 16) & 0xffu] ^ t[0][(high >> 24) & 0xffu];
    p += 8;
    n -= 8;
  }
  while (n-- > 0) {
    c = t[0][(c ^ *p++) & 0xffu] ^ (c >> 8);
  }
  return ~c;
}

#if defined(__ARM_FEATURE_CRC32) || (defined(__x86_64__) && defined(__SSE4_2__))
#define LSM_HAVE_HARDWARE_CRC32C 1

// Both ARMv8 CRC and SSE4.2 implement exactly CRC-32C, so the hardware path is
// bit-identical to the table path -- the tests run against whichever one this
// build selected.
uint32_t ExtendHardware(uint32_t crc, const char* data, size_t n) {
  uint32_t c = ~crc;
  const auto* p = reinterpret_cast<const unsigned char*>(data);

  while (n >= 8) {
#if defined(__ARM_FEATURE_CRC32)
    c = __crc32cd(c, LoadLE64(p));
#else
    c = static_cast<uint32_t>(_mm_crc32_u64(c, LoadLE64(p)));
#endif
    p += 8;
    n -= 8;
  }
  while (n-- > 0) {
#if defined(__ARM_FEATURE_CRC32)
    c = __crc32cb(c, *p++);
#else
    c = _mm_crc32_u8(c, *p++);
#endif
  }
  return ~c;
}
#endif

}  // namespace

uint32_t Extend(uint32_t crc, const char* data, size_t n) {
#if defined(LSM_HAVE_HARDWARE_CRC32C)
  return ExtendHardware(crc, data, n);
#else
  return ExtendSoftware(crc, data, n);
#endif
}

uint32_t ExtendPortable(uint32_t crc, const char* data, size_t n) {
  return ExtendSoftware(crc, data, n);
}

bool UsingHardwareAcceleration() {
#if defined(LSM_HAVE_HARDWARE_CRC32C)
  return true;
#else
  return false;
#endif
}

}  // namespace crc32c
}  // namespace lsm
