#include "lsm/bloom.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

namespace lsm {
namespace {

std::string NumKey(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "bloomkey%09d", i);
  return buf;
}

std::vector<std::string> BuildKeys(int n) {
  std::vector<std::string> keys;
  keys.reserve(n);
  for (int i = 0; i < n; ++i) keys.push_back(NumKey(i));
  return keys;
}

// Fraction of absent keys the filter wrongly reports as present.
double MeasureFalsePositiveRate(int num_keys, int bits_per_key, int num_probes) {
  const BloomFilter filter(BloomFilter::Build(BuildKeys(num_keys), bits_per_key));
  int positives = 0;
  for (int i = 0; i < num_probes; ++i) {
    // Disjoint from the inserted range.
    if (filter.MayContain(NumKey(1'000'000 + i))) ++positives;
  }
  return static_cast<double>(positives) / num_probes;
}

}  // namespace

TEST(BloomFilter, NoFalseNegatives) {
  // The correctness guarantee: a key that was inserted must always be reported
  // present. A false negative would make the engine lose data.
  for (const int n : {1, 10, 100, 1000, 10000}) {
    const auto keys = BuildKeys(n);
    const BloomFilter filter(BloomFilter::Build(keys, 10));
    for (const std::string& key : keys) {
      ASSERT_TRUE(filter.MayContain(key)) << "n=" << n << " key=" << key;
    }
  }
}

TEST(BloomFilter, EmptyFilterRejectsEverything) {
  const BloomFilter filter(BloomFilter::Build({}, 10));
  int positives = 0;
  for (int i = 0; i < 1000; ++i) {
    if (filter.MayContain(NumKey(i))) ++positives;
  }
  EXPECT_EQ(positives, 0);
}

TEST(BloomFilter, DefaultConstructedFilterIsConservative) {
  // With no filter at all we must answer "maybe" -- never "no".
  const BloomFilter filter;
  EXPECT_TRUE(filter.MayContain("anything"));
  EXPECT_TRUE(filter.empty());
}

TEST(BloomFilter, FalsePositiveRateNearTheoreticalOptimum) {
  // (1 - e^(-k*n/m))^k at k = (m/n)ln2 gives roughly 0.62^(bits per key):
  // ~6.2% at 6 bits, ~0.8% at 10, ~0.05% at 16.
  const double fp6 = MeasureFalsePositiveRate(10000, 6, 20000);
  const double fp10 = MeasureFalsePositiveRate(10000, 10, 20000);
  const double fp16 = MeasureFalsePositiveRate(10000, 16, 20000);

  std::printf("[bloom] measured FP rate: 6 bits/key=%.4f  10 bits/key=%.4f  "
              "16 bits/key=%.4f\n",
              fp6, fp10, fp16);

  EXPECT_LT(fp6, 0.10);
  EXPECT_LT(fp10, 0.02);
  EXPECT_LT(fp16, 0.005);

  // More bits must never make the filter worse.
  EXPECT_LE(fp10, fp6);
  EXPECT_LE(fp16, fp10);
}

TEST(BloomFilter, ProbeCountFollowsBitsPerKey) {
  // k = (m/n) ln 2, clamped to [1, 30].
  EXPECT_EQ(BloomFilter::BitsPerKeyToProbes(1), 1);
  EXPECT_EQ(BloomFilter::BitsPerKeyToProbes(10), 6);
  EXPECT_EQ(BloomFilter::BitsPerKeyToProbes(16), 11);
  EXPECT_EQ(BloomFilter::BitsPerKeyToProbes(1000), 30);
}

TEST(BloomFilter, SizeScalesWithBitsPerKey) {
  const size_t small = BloomFilter::Build(BuildKeys(1000), 4).size();
  const size_t large = BloomFilter::Build(BuildKeys(1000), 16).size();
  EXPECT_GT(large, small * 3);
}

TEST(BloomFilter, HandlesBinaryAndEmptyKeys) {
  std::vector<std::string> keys = {std::string("\0\0\0", 3), std::string(""),
                                   std::string("\xff\xfe", 2), "normal"};
  const BloomFilter filter(BloomFilter::Build(keys, 10));
  for (const std::string& key : keys) EXPECT_TRUE(filter.MayContain(key));
}

}  // namespace lsm
