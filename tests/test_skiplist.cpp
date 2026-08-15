#include "lsm/skiplist.h"

#include <gtest/gtest.h>

#include <atomic>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "lsm/arena.h"
#include "test_util.h"

namespace lsm {
namespace {

struct StringComparator {
  int operator()(std::string_view a, std::string_view b) const {
    return a.compare(b);
  }
};

using TestList = SkipList<StringComparator>;

}  // namespace

TEST(SkipList, EmptyListFindsNothing) {
  Arena arena;
  TestList list((StringComparator()), &arena);
  EXPECT_FALSE(list.Contains("anything"));

  TestList::Iterator iter(&list);
  iter.SeekToFirst();
  EXPECT_FALSE(iter.Valid());
  iter.Seek("k");
  EXPECT_FALSE(iter.Valid());
}

TEST(SkipList, InsertAndLookup) {
  Arena arena;
  TestList list((StringComparator()), &arena);

  const auto order = testing_support::ShuffledRange(2000, 1234);
  for (const int i : order) list.Insert(testing_support::Key(i));

  EXPECT_EQ(list.size(), 2000u);
  for (int i = 0; i < 2000; ++i) {
    EXPECT_TRUE(list.Contains(testing_support::Key(i))) << i;
  }
  EXPECT_FALSE(list.Contains(testing_support::Key(2000)));
  EXPECT_FALSE(list.Contains("zzz"));
}

TEST(SkipList, IterationIsSorted) {
  Arena arena;
  TestList list((StringComparator()), &arena);
  for (const int i : testing_support::ShuffledRange(1000, 99)) {
    list.Insert(testing_support::Key(i));
  }

  TestList::Iterator iter(&list);
  int expected = 0;
  for (iter.SeekToFirst(); iter.Valid(); iter.Next()) {
    EXPECT_EQ(iter.key(), testing_support::Key(expected));
    ++expected;
  }
  EXPECT_EQ(expected, 1000);
}

TEST(SkipList, SeekLandsOnFirstKeyAtOrAfterTarget) {
  Arena arena;
  TestList list((StringComparator()), &arena);
  for (int i = 0; i < 100; ++i) list.Insert(testing_support::Key(i * 10));

  TestList::Iterator iter(&list);
  iter.Seek(testing_support::Key(0));
  ASSERT_TRUE(iter.Valid());
  EXPECT_EQ(iter.key(), testing_support::Key(0));

  // A target between two entries must land on the later one.
  iter.Seek(testing_support::Key(55));
  ASSERT_TRUE(iter.Valid());
  EXPECT_EQ(iter.key(), testing_support::Key(60));

  iter.Seek(testing_support::Key(10000));
  EXPECT_FALSE(iter.Valid());
}

TEST(SkipList, HandlesEmptyAndBinaryKeys) {
  Arena arena;
  TestList list((StringComparator()), &arena);
  list.Insert("");
  list.Insert(std::string("\0abc", 4));
  list.Insert(std::string("\xff\xff", 2));

  EXPECT_TRUE(list.Contains(""));
  EXPECT_TRUE(list.Contains(std::string("\0abc", 4)));
  EXPECT_TRUE(list.Contains(std::string("\xff\xff", 2)));

  TestList::Iterator iter(&list);
  iter.SeekToFirst();
  ASSERT_TRUE(iter.Valid());
  EXPECT_EQ(iter.key(), "");  // Empty key sorts first.
}

// The property the whole design rests on: one writer inserting while readers
// traverse must never expose a partially-linked node. Run this under TSan.
TEST(SkipList, ConcurrentReadsDuringWrites) {
  Arena arena;
  TestList list((StringComparator()), &arena);

  constexpr int kNumKeys = 20000;
  std::atomic<int> written{0};
  std::atomic<bool> done{false};

  std::thread writer([&] {
    for (int i = 0; i < kNumKeys; ++i) {
      list.Insert(testing_support::Key(i));
      written.store(i + 1, std::memory_order_release);
    }
    done.store(true, std::memory_order_release);
  });

  std::vector<std::thread> readers;
  std::atomic<long> total_seen{0};
  for (int r = 0; r < 4; ++r) {
    readers.emplace_back([&] {
      long seen = 0;
      while (!done.load(std::memory_order_acquire)) {
        // Everything below the writer's published watermark must be visible.
        const int watermark = written.load(std::memory_order_acquire);
        for (int i = 0; i < watermark; i += 97) {
          ASSERT_TRUE(list.Contains(testing_support::Key(i))) << i;
          ++seen;
        }
        // A full scan must stay sorted no matter where the writer is.
        TestList::Iterator iter(&list);
        std::string previous;
        bool first = true;
        for (iter.SeekToFirst(); iter.Valid(); iter.Next()) {
          const std::string key(iter.key());
          if (!first) ASSERT_LT(previous, key);
          previous = key;
          first = false;
        }
      }
      total_seen.fetch_add(seen, std::memory_order_relaxed);
    });
  }

  writer.join();
  for (auto& t : readers) t.join();

  EXPECT_EQ(list.size(), static_cast<size_t>(kNumKeys));
  for (int i = 0; i < kNumKeys; i += 13) {
    EXPECT_TRUE(list.Contains(testing_support::Key(i)));
  }
}

}  // namespace lsm
