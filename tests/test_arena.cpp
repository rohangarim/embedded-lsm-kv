#include "lsm/arena.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

namespace lsm {

TEST(Arena, AllocationsDoNotOverlap) {
  Arena arena;
  std::mt19937 rng(42);
  std::vector<std::pair<char*, size_t>> chunks;

  for (int i = 0; i < 2000; ++i) {
    const size_t bytes = 1 + rng() % 300;
    char* p = arena.Allocate(bytes);
    ASSERT_NE(p, nullptr);
    std::memset(p, static_cast<int>(i & 0xff), bytes);
    chunks.emplace_back(p, bytes);
  }

  // If two allocations aliased, an earlier fill would have been clobbered.
  for (size_t i = 0; i < chunks.size(); ++i) {
    const char expected = static_cast<char>(i & 0xff);
    for (size_t j = 0; j < chunks[i].second; ++j) {
      ASSERT_EQ(chunks[i].first[j], expected) << "chunk " << i << " byte " << j;
    }
  }
}

TEST(Arena, AlignedAllocationsAreAligned) {
  Arena arena;
  std::mt19937 rng(7);
  for (int i = 0; i < 500; ++i) {
    arena.Allocate(1 + rng() % 7);  // Deliberately misalign the bump pointer.
    char* p = arena.AllocateAligned(1 + rng() % 64);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % alignof(std::max_align_t), 0u);
  }
}

TEST(Arena, MemoryUsageGrowsWithAllocation) {
  Arena arena;
  const size_t before = arena.MemoryUsage();
  for (int i = 0; i < 100; ++i) arena.Allocate(1024);
  EXPECT_GT(arena.MemoryUsage(), before + 100 * 1024 - 8192);
}

TEST(Arena, LargeAllocationGetsItsOwnBlock) {
  Arena arena;
  arena.Allocate(16);
  char* big = arena.Allocate(64 * 1024);
  ASSERT_NE(big, nullptr);
  std::memset(big, 0xab, 64 * 1024);
  EXPECT_GE(arena.MemoryUsage(), 64u * 1024);
}

}  // namespace lsm
