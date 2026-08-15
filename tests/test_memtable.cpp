#include "lsm/memtable.h"

#include <gtest/gtest.h>

#include <string>

#include "test_util.h"

namespace lsm {

TEST(MemTable, GetMissingKeyReturnsFalse) {
  MemTable mem;
  std::string value;
  Status status;
  EXPECT_FALSE(mem.Get("absent", 100, &value, &status));
}

TEST(MemTable, PutThenGet) {
  MemTable mem;
  mem.Add(1, ValueType::kValue, "alpha", "one");
  mem.Add(2, ValueType::kValue, "beta", "two");

  std::string value;
  Status status;
  ASSERT_TRUE(mem.Get("alpha", 100, &value, &status));
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(value, "one");

  ASSERT_TRUE(mem.Get("beta", 100, &value, &status));
  EXPECT_EQ(value, "two");
}

TEST(MemTable, NewerSequenceShadowsOlder) {
  MemTable mem;
  mem.Add(1, ValueType::kValue, "k", "v1");
  mem.Add(2, ValueType::kValue, "k", "v2");
  mem.Add(3, ValueType::kValue, "k", "v3");

  std::string value;
  Status status;
  ASSERT_TRUE(mem.Get("k", 100, &value, &status));
  EXPECT_EQ(value, "v3");

  // Reading at an older sequence must see the older value -- this is what makes
  // a consistent snapshot possible.
  ASSERT_TRUE(mem.Get("k", 2, &value, &status));
  EXPECT_EQ(value, "v2");
  ASSERT_TRUE(mem.Get("k", 1, &value, &status));
  EXPECT_EQ(value, "v1");

  // Below every version of the key, the memtable knows nothing about it.
  EXPECT_FALSE(mem.Get("k", 0, &value, &status));
}

TEST(MemTable, DeleteWritesTombstoneNotRemoval) {
  MemTable mem;
  mem.Add(1, ValueType::kValue, "k", "v");
  mem.Add(2, ValueType::kDeletion, "k", "");

  std::string value;
  Status status;
  // Found, but as a deletion: the caller must stop here rather than fall
  // through to an older SSTable that still holds "v".
  ASSERT_TRUE(mem.Get("k", 100, &value, &status));
  EXPECT_TRUE(status.IsNotFound());

  ASSERT_TRUE(mem.Get("k", 1, &value, &status));
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(value, "v");

  EXPECT_EQ(mem.NumEntries(), 2u);  // Both records still present.
}

TEST(MemTable, IterationOrdersByKeyThenNewestFirst) {
  MemTable mem;
  mem.Add(1, ValueType::kValue, "b", "b1");
  mem.Add(5, ValueType::kValue, "a", "a5");
  mem.Add(3, ValueType::kValue, "b", "b3");
  mem.Add(2, ValueType::kValue, "a", "a2");

  MemTable::Iterator iter(&mem);
  std::vector<std::pair<std::string, std::string>> seen;
  for (iter.SeekToFirst(); iter.Valid(); iter.Next()) {
    seen.emplace_back(std::string(ExtractUserKey(iter.internal_key())),
                      std::string(iter.value()));
  }

  ASSERT_EQ(seen.size(), 4u);
  EXPECT_EQ(seen[0], std::make_pair(std::string("a"), std::string("a5")));
  EXPECT_EQ(seen[1], std::make_pair(std::string("a"), std::string("a2")));
  EXPECT_EQ(seen[2], std::make_pair(std::string("b"), std::string("b3")));
  EXPECT_EQ(seen[3], std::make_pair(std::string("b"), std::string("b1")));
}

TEST(MemTable, HandlesEmptyValuesAndBinaryKeys) {
  MemTable mem;
  mem.Add(1, ValueType::kValue, std::string("\0\1\2", 3), "");
  std::string value = "stale";
  Status status;
  ASSERT_TRUE(mem.Get(std::string("\0\1\2", 3), 10, &value, &status));
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(value, "");
}

TEST(MemTable, MemoryUsageGrowsWithEntries) {
  MemTable mem;
  const size_t before = mem.ApproximateMemoryUsage();
  for (int i = 0; i < 5000; ++i) {
    mem.Add(i + 1, ValueType::kValue, testing_support::Key(i),
            testing_support::Value(i));
  }
  EXPECT_GT(mem.ApproximateMemoryUsage(), before);
  EXPECT_GT(mem.ApproximateMemoryUsage(), 5000u * 64);
  EXPECT_EQ(mem.NumEntries(), 5000u);
}

TEST(MemTable, LookupAcrossManyKeys) {
  MemTable mem;
  for (const int i : testing_support::ShuffledRange(3000, 7)) {
    mem.Add(static_cast<SequenceNumber>(i) + 1, ValueType::kValue,
            testing_support::Key(i), testing_support::Value(i));
  }
  std::string value;
  Status status;
  for (int i = 0; i < 3000; ++i) {
    ASSERT_TRUE(mem.Get(testing_support::Key(i), kMaxSequenceNumber, &value,
                        &status))
        << i;
    EXPECT_EQ(value, testing_support::Value(i));
  }
}

}  // namespace lsm
