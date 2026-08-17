#include "lsm/block.h"

#include <gtest/gtest.h>

#include <random>
#include <string>
#include <utility>
#include <vector>

#include "lsm/coding.h"
#include "lsm/internal_key.h"

namespace lsm {
namespace {

using Entries = std::vector<std::pair<std::string, std::string>>;

std::string IKey(const std::string& user_key, SequenceNumber seq = 1) {
  return EncodeInternalKey(user_key, seq, ValueType::kValue);
}

std::string Build(const Entries& entries, int restart_interval = 16) {
  BlockBuilder builder(restart_interval);
  for (const auto& e : entries) builder.Add(e.first, e.second);
  return builder.Finish();
}

Entries Drain(const std::string& block) {
  BlockReader reader(block);
  EXPECT_TRUE(reader.status().ok()) << reader.status().ToString();
  Entries out;
  for (reader.SeekToFirst(); reader.Valid(); reader.Next()) {
    out.emplace_back(std::string(reader.key()), std::string(reader.value()));
  }
  EXPECT_TRUE(reader.status().ok()) << reader.status().ToString();
  return out;
}

}  // namespace

TEST(Block, SingleEntryRoundTrips) {
  const Entries entries = {{IKey("only"), "value"}};
  EXPECT_EQ(Drain(Build(entries)), entries);
}

TEST(Block, EmptyBlockHasOneRestartAndNoEntries) {
  BlockBuilder builder(16);
  EXPECT_TRUE(builder.empty());
  const std::string block = builder.Finish();

  BlockReader reader(block);
  ASSERT_TRUE(reader.status().ok());
  EXPECT_EQ(reader.num_restarts(), 1u);
  reader.SeekToFirst();
  EXPECT_FALSE(reader.Valid());
}

TEST(Block, ManyEntriesRoundTripInOrder) {
  Entries entries;
  for (int i = 0; i < 500; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "user%010d", i);
    entries.emplace_back(IKey(buf, static_cast<SequenceNumber>(i + 1)),
                         "value" + std::to_string(i));
  }
  EXPECT_EQ(Drain(Build(entries)), entries);
}

TEST(Block, EmptyValuesSurvive) {
  const Entries entries = {
      {IKey("a"), ""}, {IKey("b"), "v"}, {IKey("c"), ""}, {IKey("d"), ""}};
  EXPECT_EQ(Drain(Build(entries)), entries);
}

TEST(Block, BinaryKeysAndSharedPrefixes) {
  const Entries entries = {
      {IKey(std::string("\0\0", 2)), "a"},
      {IKey(std::string("\0\1", 2)), "b"},
      {IKey(std::string("\0\1\xff", 3)), "c"},
      {IKey(std::string("\xff", 1)), "d"},
  };
  EXPECT_EQ(Drain(Build(entries)), entries);
}

// Keys differing only in the trailing tag share their entire user key, which is
// the case prefix compression is most aggressive on.
TEST(Block, VersionsOfOneKeyShareEverythingButTheTag) {
  Entries entries;
  for (int seq = 40; seq > 0; --seq) {
    entries.emplace_back(IKey("hot", static_cast<SequenceNumber>(seq)),
                         "v" + std::to_string(seq));
  }
  EXPECT_EQ(Drain(Build(entries)), entries);
}

TEST(Block, RestartIntervalIsHonoured) {
  Entries entries;
  for (int i = 0; i < 64; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key%06d", i);
    entries.emplace_back(IKey(buf), "v");
  }
  for (const int interval : {1, 2, 8, 16, 64, 1000}) {
    const std::string block = Build(entries, interval);
    BlockReader reader(block);
    ASSERT_TRUE(reader.status().ok());
    // 64 entries at interval N means ceil(64/N) restart points.
    const uint32_t expected = static_cast<uint32_t>((64 + interval - 1) / interval);
    EXPECT_EQ(reader.num_restarts(), expected) << "interval " << interval;
    EXPECT_EQ(Drain(block), entries) << "interval " << interval;
  }
}

TEST(Block, PrefixCompressionActuallyShrinksTheBlock) {
  Entries entries;
  for (int i = 0; i < 200; ++i) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "user%016d", i);
    entries.emplace_back(IKey(buf), std::string(100, 'x'));
  }
  // interval 1 stores every key whole; interval 16 shares within runs.
  const size_t uncompressed = Build(entries, 1).size();
  const size_t compressed = Build(entries, 16).size();
  std::printf("[block] 200 entries: %zu bytes at interval 1, %zu at interval 16 "
              "(%.1f%% smaller)\n",
              uncompressed, compressed,
              100.0 * (1.0 - static_cast<double>(compressed) /
                                 static_cast<double>(uncompressed)));
  EXPECT_LT(compressed, uncompressed);
}

TEST(Block, SeekFindsEveryKey) {
  Entries entries;
  for (int i = 0; i < 300; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key%06d", i * 2);  // Even keys only.
    entries.emplace_back(IKey(buf), "v" + std::to_string(i));
  }
  const std::string block = Build(entries);

  BlockReader reader(block);
  for (size_t i = 0; i < entries.size(); ++i) {
    reader.Seek(entries[i].first);
    ASSERT_TRUE(reader.Valid()) << i;
    EXPECT_EQ(reader.key(), entries[i].first) << i;
    EXPECT_EQ(reader.value(), entries[i].second) << i;
  }
}

TEST(Block, SeekLandsOnTheFirstKeyAtOrAfterTarget) {
  Entries entries;
  for (int i = 0; i < 100; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key%06d", i * 10);
    entries.emplace_back(IKey(buf), "v");
  }
  const std::string block = Build(entries);
  BlockReader reader(block);

  // Between two stored keys.
  reader.Seek(IKey("key000055"));
  ASSERT_TRUE(reader.Valid());
  EXPECT_EQ(ExtractUserKey(reader.key()), "key000060");

  // Before everything.
  reader.Seek(IKey("aaaaaa"));
  ASSERT_TRUE(reader.Valid());
  EXPECT_EQ(ExtractUserKey(reader.key()), "key000000");

  // Past the end.
  reader.Seek(IKey("zzzzzz"));
  EXPECT_FALSE(reader.Valid());
}

TEST(Block, SeekThenIterateContinuesCorrectly) {
  Entries entries;
  for (int i = 0; i < 200; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key%06d", i);
    entries.emplace_back(IKey(buf), "v" + std::to_string(i));
  }
  const std::string block = Build(entries, 8);

  BlockReader reader(block);
  reader.Seek(entries[137].first);
  size_t i = 137;
  for (; reader.Valid(); reader.Next(), ++i) {
    ASSERT_LT(i, entries.size());
    EXPECT_EQ(reader.key(), entries[i].first) << i;
    EXPECT_EQ(reader.value(), entries[i].second) << i;
  }
  EXPECT_EQ(i, entries.size());
}

// Seeking mid-run must reconstruct the key from its restart point, not from
// whatever the cursor happened to be pointing at before.
TEST(Block, RepeatedSeeksDoNotLeakPreviousKeyState) {
  Entries entries;
  for (int i = 0; i < 128; ++i) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "prefix%08d_suffix", i);
    entries.emplace_back(IKey(buf), "v");
  }
  const std::string block = Build(entries, 16);
  BlockReader reader(block);

  const int order[] = {127, 0, 64, 3, 100, 17, 63, 65, 1, 126};
  for (const int i : order) {
    reader.Seek(entries[i].first);
    ASSERT_TRUE(reader.Valid()) << i;
    EXPECT_EQ(reader.key(), entries[i].first) << i;
  }
}

TEST(Block, RejectsTruncatedBlock) {
  const std::string block = Build({{IKey("a"), "v"}, {IKey("b"), "v"}});
  BlockReader empty(std::string_view("", 0));
  EXPECT_TRUE(empty.status().IsCorruption());

  // Chop the restart array off.
  BlockReader chopped(std::string_view(block.data(), block.size() - 4));
  chopped.SeekToFirst();
  // Either rejected up front or it runs out cleanly -- never a bogus key.
  if (chopped.status().ok() && chopped.Valid()) {
    EXPECT_GE(chopped.key().size(), kTagSize);
  }
}

TEST(Block, RejectsRestartCountLargerThanTheBlock) {
  std::string block = Build({{IKey("a"), "v"}});
  // Claim a restart array that cannot fit.
  std::string forged(block.data(), block.size() - 4);
  PutFixed32(&forged, 1000000);
  BlockReader reader(forged);
  EXPECT_TRUE(reader.status().IsCorruption()) << reader.status().ToString();
}

TEST(Block, RejectsRestartOffsetPastTheEntries) {
  Entries entries;
  for (int i = 0; i < 40; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key%04d", i);
    entries.emplace_back(IKey(buf), "v");
  }
  std::string block = Build(entries, 8);

  // Point the second restart offset far past the entry region.
  BlockReader original(block);
  ASSERT_TRUE(original.status().ok());
  ASSERT_GT(original.num_restarts(), 1u);
  const size_t trailer_start =
      block.size() - (original.num_restarts() + 1) * sizeof(uint32_t);
  std::string forged = block;
  const uint32_t bogus = 0xffffff;
  std::memcpy(&forged[trailer_start + sizeof(uint32_t)], &bogus, sizeof(bogus));

  BlockReader reader(forged);
  EXPECT_TRUE(reader.status().IsCorruption()) << reader.status().ToString();
}

// Whatever the interval and whatever the keys, building then draining must be
// the identity.
TEST(Block, RandomisedRoundTrip) {
  std::mt19937 rng(20260817);
  for (int trial = 0; trial < 100; ++trial) {
    const int interval = 1 + static_cast<int>(rng() % 32);
    const int count = static_cast<int>(rng() % 200);

    std::vector<std::string> user_keys;
    for (int i = 0; i < count; ++i) {
      const size_t len = 1 + rng() % 40;
      std::string k;
      for (size_t j = 0; j < len; ++j) {
        k.push_back(static_cast<char>('a' + rng() % 4));  // Force shared prefixes.
      }
      user_keys.push_back(std::move(k));
    }
    std::sort(user_keys.begin(), user_keys.end());
    user_keys.erase(std::unique(user_keys.begin(), user_keys.end()),
                    user_keys.end());

    Entries entries;
    for (const auto& k : user_keys) {
      entries.emplace_back(IKey(k), std::string(rng() % 50, 'v'));
    }

    const std::string block = Build(entries, interval);
    ASSERT_EQ(Drain(block), entries) << "trial " << trial << " interval " << interval;

    // And every key is findable by Seek.
    BlockReader reader(block);
    for (const auto& e : entries) {
      reader.Seek(e.first);
      ASSERT_TRUE(reader.Valid());
      ASSERT_EQ(reader.key(), e.first);
    }
  }
}

}  // namespace lsm
