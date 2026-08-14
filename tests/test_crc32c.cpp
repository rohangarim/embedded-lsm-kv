#include "lsm/crc32c.h"

#include <gtest/gtest.h>

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

TEST(Crc32c, ExtendMatchesSinglePass) {
  const std::string a = "first half of the record";
  const std::string b = "second half of the record";
  const uint32_t incremental =
      crc32c::Extend(crc32c::Value(a.data(), a.size()), b.data(), b.size());
  EXPECT_EQ(incremental, crc32c::Value(a + b));
}

}  // namespace lsm
