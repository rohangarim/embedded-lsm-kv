#include "lsm/bloom.h"

#include <algorithm>
#include <cstring>

#include "lsm/coding.h"

namespace lsm {

uint32_t BloomHash(std::string_view key) {
  // MurmurHash-1 style: mix 4 bytes at a time, then the tail.
  constexpr uint32_t m = 0xc6a4a793;
  constexpr uint32_t seed = 0xbc9f1d34;
  uint32_t h = seed ^ (static_cast<uint32_t>(key.size()) * m);

  const char* data = key.data();
  const char* limit = data + key.size();
  while (limit - data >= 4) {
    h += DecodeFixed32(data);
    h *= m;
    h ^= (h >> 16);
    data += 4;
  }
  switch (limit - data) {
    case 3:
      h += static_cast<uint8_t>(data[2]) << 16;
      [[fallthrough]];
    case 2:
      h += static_cast<uint8_t>(data[1]) << 8;
      [[fallthrough]];
    case 1:
      h += static_cast<uint8_t>(data[0]);
      h *= m;
      h ^= (h >> 24);
      break;
    default:
      break;
  }
  return h;
}

int BloomFilter::BitsPerKeyToProbes(int bits_per_key) {
  // k = (m/n) * ln 2, clamped: below 1 the filter does nothing, above 30 the
  // extra probes cost more than the FP rate they save.
  int k = static_cast<int>(static_cast<double>(bits_per_key) * 0.69314718056);
  return std::max(1, std::min(30, k));
}

std::string BloomFilter::Build(const std::vector<std::string>& keys,
                               int bits_per_key) {
  const int k = BitsPerKeyToProbes(bits_per_key);

  size_t bits = keys.size() * static_cast<size_t>(bits_per_key);
  // Tiny filters have a disproportionately bad FP rate; floor at 64 bits.
  bits = std::max<size_t>(bits, 64);
  const size_t bytes = (bits + 7) / 8;
  bits = bytes * 8;

  std::string filter(bytes, '\0');
  for (const std::string& key : keys) {
    // Double hashing: derive all k probe positions from one hash by rotating
    // it. Cheaper than k independent hashes and, for these parameters,
    // indistinguishable in FP rate.
    uint32_t h = BloomHash(key);
    const uint32_t delta = (h >> 17) | (h << 15);
    for (int i = 0; i < k; ++i) {
      const size_t bitpos = h % bits;
      filter[bitpos / 8] |= static_cast<char>(1u << (bitpos % 8));
      h += delta;
    }
  }
  // Probe count is stored with the filter so a reader never has to be told the
  // bits-per-key the file was written with.
  filter.push_back(static_cast<char>(k));
  return filter;
}

bool BloomFilter::MayContain(std::string_view key) const {
  if (data_.size() < 2) {
    // No filter (or a degenerate one): fall back to "maybe", never a false
    // negative.
    return true;
  }
  const size_t bytes = data_.size() - 1;
  const size_t bits = bytes * 8;
  const int k = static_cast<uint8_t>(data_[bytes]);
  if (k > 30) return true;  // Reserved encoding; be conservative.

  uint32_t h = BloomHash(key);
  const uint32_t delta = (h >> 17) | (h << 15);
  for (int i = 0; i < k; ++i) {
    const size_t bitpos = h % bits;
    if ((static_cast<uint8_t>(data_[bitpos / 8]) & (1u << (bitpos % 8))) == 0) {
      return false;
    }
    h += delta;
  }
  return true;
}

}  // namespace lsm
