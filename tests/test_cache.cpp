#include "lsm/cache.h"

#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace lsm {
namespace {

std::string Block(size_t size, char fill) { return std::string(size, fill); }

}  // namespace

TEST(BlockCache, LookupMissesOnEmptyCache) {
  BlockCache cache(1 << 20);
  EXPECT_EQ(cache.Lookup(1, 0), nullptr);
  EXPECT_EQ(cache.hits(), 0u);
  EXPECT_EQ(cache.misses(), 1u);
}

TEST(BlockCache, InsertThenLookup) {
  BlockCache cache(1 << 20);
  cache.Insert(7, 4096, Block(100, 'a'));

  const auto handle = cache.Lookup(7, 4096);
  ASSERT_NE(handle, nullptr);
  EXPECT_EQ(*handle, Block(100, 'a'));
  EXPECT_EQ(cache.hits(), 1u);
}

TEST(BlockCache, FileNumberAndOffsetBothDiscriminate) {
  BlockCache cache(1 << 20);
  cache.Insert(1, 2, Block(10, 'x'));
  cache.Insert(2, 1, Block(10, 'y'));

  // A hash that merely xor'd the two fields would collide these.
  ASSERT_NE(cache.Lookup(1, 2), nullptr);
  ASSERT_NE(cache.Lookup(2, 1), nullptr);
  EXPECT_EQ(*cache.Lookup(1, 2), Block(10, 'x'));
  EXPECT_EQ(*cache.Lookup(2, 1), Block(10, 'y'));
  EXPECT_EQ(cache.Lookup(1, 1), nullptr);
}

TEST(BlockCache, EvictsUnderCapacityPressure) {
  // One shard so the eviction order is deterministic.
  BlockCache cache(/*capacity_bytes=*/1000, /*num_shards=*/1);
  for (int i = 0; i < 20; ++i) {
    cache.Insert(1, static_cast<uint64_t>(i), Block(100, static_cast<char>('a' + i)));
  }
  EXPECT_GT(cache.evictions(), 0u);
  EXPECT_LE(cache.charge_bytes(), 1000u + 100u);

  // The most recent inserts should still be resident, the oldest gone.
  EXPECT_NE(cache.Lookup(1, 19), nullptr);
  EXPECT_EQ(cache.Lookup(1, 0), nullptr);
}

TEST(BlockCache, RecentlyUsedEntriesSurviveEviction) {
  BlockCache cache(/*capacity_bytes=*/500, /*num_shards=*/1);
  for (int i = 0; i < 5; ++i) {
    cache.Insert(1, static_cast<uint64_t>(i), Block(100, 'x'));
  }
  // Touch the oldest so it is no longer the eviction candidate.
  ASSERT_NE(cache.Lookup(1, 0), nullptr);

  for (int i = 5; i < 8; ++i) {
    cache.Insert(1, static_cast<uint64_t>(i), Block(100, 'x'));
  }
  EXPECT_NE(cache.Lookup(1, 0), nullptr) << "touched entry should have survived";
}

// The property that makes eviction safe: a handle already handed out keeps its
// bytes alive even after the cache drops the entry.
TEST(BlockCache, HandleOutlivesEviction) {
  BlockCache cache(/*capacity_bytes=*/300, /*num_shards=*/1);
  cache.Insert(1, 0, Block(100, 'z'));
  const auto pinned = cache.Lookup(1, 0);
  ASSERT_NE(pinned, nullptr);

  for (int i = 1; i < 30; ++i) {
    cache.Insert(1, static_cast<uint64_t>(i), Block(100, 'q'));
  }
  ASSERT_EQ(cache.Lookup(1, 0), nullptr) << "entry should have been evicted";
  EXPECT_EQ(*pinned, Block(100, 'z')) << "pinned bytes must still be readable";
}

TEST(BlockCache, EraseFileDropsOnlyThatFile) {
  BlockCache cache(1 << 20);
  for (int i = 0; i < 10; ++i) {
    cache.Insert(1, static_cast<uint64_t>(i), Block(50, 'a'));
    cache.Insert(2, static_cast<uint64_t>(i), Block(50, 'b'));
  }
  cache.EraseFile(1);

  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(cache.Lookup(1, static_cast<uint64_t>(i)), nullptr) << i;
    EXPECT_NE(cache.Lookup(2, static_cast<uint64_t>(i)), nullptr) << i;
  }
  EXPECT_EQ(cache.charge_bytes(), 10u * 50u);
}

TEST(BlockCache, DuplicateInsertKeepsOneEntry) {
  BlockCache cache(1 << 20);
  const auto first = cache.Insert(1, 0, Block(64, 'a'));
  const auto second = cache.Insert(1, 0, Block(64, 'a'));
  // Racing readers must converge on one canonical copy, not two.
  EXPECT_EQ(first.get(), second.get());
  EXPECT_EQ(cache.charge_bytes(), 64u);
}

TEST(BlockCache, HitRateReflectsLookups) {
  BlockCache cache(1 << 20);
  cache.Insert(1, 0, Block(10, 'a'));
  for (int i = 0; i < 3; ++i) cache.Lookup(1, 0);
  cache.Lookup(1, 999);

  EXPECT_EQ(cache.hits(), 3u);
  EXPECT_EQ(cache.misses(), 1u);
  EXPECT_DOUBLE_EQ(cache.HitRate(), 0.75);
}

TEST(BlockCache, ConcurrentAccessIsSafe) {
  BlockCache cache(1 << 16);  // Small enough to force constant eviction.
  constexpr int kThreads = 8;
  constexpr int kOpsPerThread = 20000;

  std::vector<std::thread> threads;
  std::atomic<int> mismatches{0};
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kOpsPerThread; ++i) {
        const uint64_t offset = static_cast<uint64_t>((i * 7 + t) % 500);
        if (auto handle = cache.Lookup(1, offset)) {
          // Whatever we get back must be the block written for that offset.
          if (handle->size() != 64 ||
              (*handle)[0] != static_cast<char>('a' + offset % 26)) {
            mismatches.fetch_add(1);
          }
        } else {
          cache.Insert(1, offset,
                       std::string(64, static_cast<char>('a' + offset % 26)));
        }
      }
    });
  }
  for (auto& thread : threads) thread.join();

  EXPECT_EQ(mismatches.load(), 0);
  EXPECT_LE(cache.charge_bytes(), static_cast<size_t>(1 << 16) + 64 * 16);
}

TEST(BlockCache, ZeroCapacityStillReturnsTheInsertedBlock) {
  // Degenerate config: nothing can be retained, but Insert must still hand back
  // a usable handle rather than null.
  BlockCache cache(0, 1);
  const auto handle = cache.Insert(1, 0, Block(32, 'a'));
  ASSERT_NE(handle, nullptr);
  EXPECT_EQ(*handle, Block(32, 'a'));
}

}  // namespace lsm
