#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lsm {

// Bloom filter over the user keys of one SSTable.
//
// The point is negative lookups. Without it, a Get for a key that does not
// exist has to binary-search the index and read a data block from every SSTable
// whose range covers the key -- pure wasted I/O. The filter answers "definitely
// absent" from memory, and only "possibly present" costs a disk read.
//
// False positives cost an extra block read; false negatives would be a
// correctness bug and cannot happen. Expected FP rate is approximately
// (1 - e^(-k*n/m))^k, minimised at k = (m/n) * ln 2, which is what
// BitsPerKeyToProbes computes.
class BloomFilter {
 public:
  static int BitsPerKeyToProbes(int bits_per_key);

  // Builds a filter over `keys` (user keys; duplicates are harmless).
  static std::string Build(const std::vector<std::string>& keys, int bits_per_key);

  BloomFilter() = default;
  // `encoded` is the output of Build(); the trailing byte holds the probe count.
  explicit BloomFilter(std::string encoded) : data_(std::move(encoded)) {}

  // False means the key is definitely not in the SSTable. True means maybe.
  bool MayContain(std::string_view key) const;

  bool empty() const { return data_.size() < 2; }
  size_t size_bytes() const { return data_.size(); }
  const std::string& encoded() const { return data_; }

 private:
  std::string data_;
};

// The hash the filter is built on: 32-bit MurmurHash-style mixing. Exposed so
// tests can reason about collisions.
uint32_t BloomHash(std::string_view key);

}  // namespace lsm
