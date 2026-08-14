#include "lsm/crc32c.h"

#include <array>

namespace lsm {
namespace crc32c {
namespace {

// Bit-reflected Castagnoli polynomial. The table is built once at static-init
// time rather than hand-written so the constant stays auditable.
constexpr uint32_t kPoly = 0x82f63b78u;

std::array<uint32_t, 256> MakeTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t crc = i;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) ? (crc >> 1) ^ kPoly : (crc >> 1);
    }
    table[i] = crc;
  }
  return table;
}

const std::array<uint32_t, 256>& Table() {
  static const std::array<uint32_t, 256> table = MakeTable();
  return table;
}

}  // namespace

uint32_t Extend(uint32_t crc, const char* data, size_t n) {
  const auto& table = Table();
  uint32_t c = ~crc;
  const auto* p = reinterpret_cast<const unsigned char*>(data);
  for (size_t i = 0; i < n; ++i) {
    c = table[(c ^ p[i]) & 0xffu] ^ (c >> 8);
  }
  return ~c;
}

}  // namespace crc32c
}  // namespace lsm
