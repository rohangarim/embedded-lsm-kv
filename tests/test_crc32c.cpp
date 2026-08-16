#include "lsm/crc32c.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <random>
#include <string>

namespace lsm {

// Known-answer vectors from RFC 3720 appendix B. If these drift, every WAL and
// SSTable written by an older build becomes unreadable, so pin them.
TEST(Crc32c, StandardVectors) {
  const std::string zeros(32, '\0');
  EXPECT_EQ(crc32c::Value(zeros), 0x8a9136aau);

  const std::string ones(32, '\xff');
  EXPECT_EQ(crc32c::Value(ones), 0x62a8ab43u);

  std::string ascending(32, '\0');
  for (int i = 0; i < 32; ++i) ascending[i] = static_cast<char>(i);
  EXPECT_EQ(crc32c::Value(ascending), 0x46dd794eu);
}

TEST(Crc32c, EmptyInputIsZero) { EXPECT_EQ(crc32c::Value("", 0), 0u); }

TEST(Crc32c, DetectsSingleBitFlip) {
  std::string data = "the quick brown fox jumps over the lazy dog";
  const uint32_t original = crc32c::Value(data);
  for (size_t i = 0; i < data.size(); ++i) {
    for (int bit = 0; bit < 8; ++bit) {
      std::string flipped = data;
      flipped[i] = static_cast<char>(flipped[i] ^ (1 << bit));
      EXPECT_NE(crc32c::Value(flipped), original) << "byte " << i << " bit " << bit;
    }
  }
}

// The hardware instruction and the portable tables must agree exactly. If they
// ever diverge, a file written on one machine stops verifying on another.
TEST(Crc32c, HardwarePathMatchesPortablePath) {
  std::printf("[crc32c] hardware acceleration: %s\n",
              crc32c::UsingHardwareAcceleration() ? "yes" : "no");

  std::mt19937 rng(20260816);
  for (size_t length : {0u, 1u, 2u, 3u, 7u, 8u, 9u, 15u, 16u, 31u, 64u, 100u,
                        1000u, 4096u, 4097u, 65536u}) {
    std::string data(length, '\0');
    for (size_t i = 0; i < length; ++i) data[i] = static_cast<char>(rng());
    EXPECT_EQ(crc32c::Value(data.data(), data.size()),
              crc32c::ExtendPortable(0, data.data(), data.size()))
        << "length " << length;
  }
}

TEST(Crc32c, ExtendMatchesSinglePass) {
  const std::string a = "first half of the record";
  const std::string b = "second half of the record";
  const uint32_t incremental =
      crc32c::Extend(crc32c::Value(a.data(), a.size()), b.data(), b.size());
  EXPECT_EQ(incremental, crc32c::Value(a + b));
}

}  // namespace lsm
