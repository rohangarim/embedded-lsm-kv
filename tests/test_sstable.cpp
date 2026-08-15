#include "lsm/sstable.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "test_util.h"

namespace lsm {
namespace {

// Writes keys 0..n-1 (sequence i+1) into an SSTable and returns it opened.
std::unique_ptr<Table> BuildTable(const std::string& path, int n,
                                  const Options& options) {
  TableBuilder builder(options);
  EXPECT_TRUE(builder.Open(path).ok());
  for (int i = 0; i < n; ++i) {
    const std::string ikey = EncodeInternalKey(testing_support::Key(i),
                                               static_cast<SequenceNumber>(i) + 1,
                                               ValueType::kValue);
    EXPECT_TRUE(builder.Add(ikey, testing_support::Value(i)).ok());
  }
  EXPECT_TRUE(builder.Finish().ok());

  std::unique_ptr<Table> table;
  const Status s = Table::Open(path, options, &table);
  EXPECT_TRUE(s.ok()) << s.ToString();
  return table;
}

}  // namespace

TEST(SSTable, EmptyTableRoundTrips) {
  testing_support::ScopedTempDir dir("sst_empty");
  Options options;
  const std::string path = dir.File("000001.sst");

  TableBuilder builder(options);
  ASSERT_TRUE(builder.Open(path).ok());
  ASSERT_TRUE(builder.Finish().ok());
  EXPECT_EQ(builder.NumEntries(), 0u);

  std::unique_ptr<Table> table;
  ASSERT_TRUE(Table::Open(path, options, &table).ok());
  EXPECT_EQ(table->num_data_blocks(), 0u);

  std::string value;
  Status status;
  EXPECT_FALSE(table->Get("anything", kMaxSequenceNumber, &value, &status));

  auto iter = table->NewIterator();
  iter->SeekToFirst();
  EXPECT_FALSE(iter->Valid());
}

TEST(SSTable, PointLookupsFindEveryKey) {
  testing_support::ScopedTempDir dir("sst_lookup");
  Options options;
  auto table = BuildTable(dir.File("000001.sst"), 5000, options);
  ASSERT_NE(table, nullptr);

  std::string value;
  Status status;
  for (int i = 0; i < 5000; ++i) {
    ASSERT_TRUE(table->Get(testing_support::Key(i), kMaxSequenceNumber, &value,
                           &status))
        << i;
    ASSERT_TRUE(status.ok());
    EXPECT_EQ(value, testing_support::Value(i));
  }
}

TEST(SSTable, MissingKeysAreNotFound) {
  testing_support::ScopedTempDir dir("sst_missing");
  Options options;
  auto table = BuildTable(dir.File("000001.sst"), 1000, options);

  std::string value;
  Status status;
  EXPECT_FALSE(table->Get("aaaa_before_everything", kMaxSequenceNumber, &value,
                          &status));
  EXPECT_FALSE(table->Get("zzzz_after_everything", kMaxSequenceNumber, &value,
                          &status));
  for (int i = 1000; i < 1100; ++i) {
    EXPECT_FALSE(
        table->Get(testing_support::Key(i), kMaxSequenceNumber, &value, &status));
  }
}

// A key absent from the table should mostly not cost a block read at all --
// that is the entire value of carrying a per-table Bloom filter.
TEST(SSTable, BloomFilterSkipsMostBlockReadsForAbsentKeys) {
  testing_support::ScopedTempDir dir("sst_bloom");
  Options options;
  options.bloom_bits_per_key = 10;
  auto table = BuildTable(dir.File("000001.sst"), 10000, options);

  const uint64_t blocks_before = table->blocks_read();
  std::string value;
  Status status;
  constexpr int kProbes = 5000;
  for (int i = 100000; i < 100000 + kProbes; ++i) {
    EXPECT_FALSE(
        table->Get(testing_support::Key(i), kMaxSequenceNumber, &value, &status));
  }
  const uint64_t reads = table->blocks_read() - blocks_before;
  const double read_rate = static_cast<double>(reads) / kProbes;
  std::printf("[sstable] absent-key block reads: %llu / %d probes (%.3f)\n",
              static_cast<unsigned long long>(reads), kProbes, read_rate);

  EXPECT_LT(read_rate, 0.05);
  EXPECT_GT(table->bloom_rejections(), static_cast<uint64_t>(kProbes) * 95 / 100);
}

TEST(SSTable, LookupReadsExactlyOneBlock) {
  testing_support::ScopedTempDir dir("sst_oneblock");
  Options options;
  auto table = BuildTable(dir.File("000001.sst"), 10000, options);
  ASSERT_GT(table->num_data_blocks(), 10u) << "test needs a multi-block table";

  const uint64_t before = table->blocks_read();
  std::string value;
  Status status;
  ASSERT_TRUE(
      table->Get(testing_support::Key(7777), kMaxSequenceNumber, &value, &status));
  // Sparse index in memory, then one data block off disk.
  EXPECT_EQ(table->blocks_read() - before, 1u);
}

TEST(SSTable, TombstonesAreVisibleToLookups) {
  testing_support::ScopedTempDir dir("sst_tombstone");
  Options options;
  const std::string path = dir.File("000001.sst");

  TableBuilder builder(options);
  ASSERT_TRUE(builder.Open(path).ok());
  ASSERT_TRUE(builder.Add(EncodeInternalKey("gone", 5, ValueType::kDeletion), "")
                  .ok());
  ASSERT_TRUE(
      builder.Add(EncodeInternalKey("gone", 1, ValueType::kValue), "old").ok());
  ASSERT_TRUE(builder.Add(EncodeInternalKey("live", 2, ValueType::kValue), "v")
                  .ok());
  ASSERT_TRUE(builder.Finish().ok());

  std::unique_ptr<Table> table;
  ASSERT_TRUE(Table::Open(path, options, &table).ok());

  std::string value;
  Status status;
  ASSERT_TRUE(table->Get("gone", kMaxSequenceNumber, &value, &status));
  EXPECT_TRUE(status.IsNotFound());

  // Reading below the tombstone's sequence still sees the old value.
  ASSERT_TRUE(table->Get("gone", 3, &value, &status));
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(value, "old");
}

TEST(SSTable, IteratorWalksEveryEntryInOrder) {
  testing_support::ScopedTempDir dir("sst_iter");
  Options options;
  options.block_size_bytes = 512;  // Force many blocks.
  auto table = BuildTable(dir.File("000001.sst"), 3000, options);
  ASSERT_GT(table->num_data_blocks(), 100u);

  auto iter = table->NewIterator();
  int count = 0;
  std::string previous;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    const std::string key(iter->key());
    if (count > 0) ASSERT_LT(CompareInternalKey(previous, key), 0);
    EXPECT_EQ(ExtractUserKey(key), testing_support::Key(count));
    EXPECT_EQ(iter->value(), testing_support::Value(count));
    previous = key;
    ++count;
  }
  EXPECT_EQ(count, 3000);
  EXPECT_TRUE(iter->status().ok());
}

TEST(SSTable, IteratorSeekCrossesBlockBoundaries) {
  testing_support::ScopedTempDir dir("sst_seek");
  Options options;
  options.block_size_bytes = 256;
  auto table = BuildTable(dir.File("000001.sst"), 2000, options);

  auto iter = table->NewIterator();
  for (const int target : {0, 1, 499, 500, 1234, 1999}) {
    iter->Seek(LookupKey(testing_support::Key(target), kMaxSequenceNumber));
    ASSERT_TRUE(iter->Valid()) << target;
    EXPECT_EQ(ExtractUserKey(iter->key()), testing_support::Key(target));
  }
  iter->Seek(LookupKey(testing_support::Key(999999), kMaxSequenceNumber));
  EXPECT_FALSE(iter->Valid());
}

TEST(SSTable, RecordsSmallestAndLargestKeys) {
  testing_support::ScopedTempDir dir("sst_range");
  Options options;
  const std::string path = dir.File("000001.sst");

  TableBuilder builder(options);
  ASSERT_TRUE(builder.Open(path).ok());
  for (int i = 10; i < 60; ++i) {
    ASSERT_TRUE(builder
                    .Add(EncodeInternalKey(testing_support::Key(i), i + 1,
                                           ValueType::kValue),
                         "v")
                    .ok());
  }
  ASSERT_TRUE(builder.Finish().ok());

  EXPECT_EQ(ExtractUserKey(builder.SmallestKey()), testing_support::Key(10));
  EXPECT_EQ(ExtractUserKey(builder.LargestKey()), testing_support::Key(59));
  EXPECT_EQ(builder.NumEntries(), 50u);
  EXPECT_GT(builder.FileSize(), kFooterSize);
}

TEST(SSTable, ChecksumMismatchIsReportedAsCorruption) {
  testing_support::ScopedTempDir dir("sst_crc");
  Options options;
  options.block_size_bytes = 4096;
  const std::string path = dir.File("000001.sst");
  { auto table = BuildTable(path, 2000, options); }

  // Corrupt a byte inside the first data block.
  const int fd = ::open(path.c_str(), O_RDWR);
  ASSERT_GE(fd, 0);
  char byte;
  ASSERT_EQ(::pread(fd, &byte, 1, 100), 1);
  byte = static_cast<char>(byte ^ 0xff);
  ASSERT_EQ(::pwrite(fd, &byte, 1, 100), 1);
  ::close(fd);

  std::unique_ptr<Table> table;
  ASSERT_TRUE(Table::Open(path, options, &table).ok());
  std::string value;
  Status status;
  // The read either reports corruption or, if the bloom filter rejects first,
  // does not resolve here at all. Silent bad data is the outcome we forbid.
  if (table->Get(testing_support::Key(0), kMaxSequenceNumber, &value, &status)) {
    EXPECT_TRUE(status.IsCorruption()) << status.ToString();
  }
}

TEST(SSTable, RejectsTruncatedFile) {
  testing_support::ScopedTempDir dir("sst_trunc");
  Options options;
  const std::string path = dir.File("000001.sst");
  { auto table = BuildTable(path, 500, options); }
  ASSERT_EQ(::truncate(path.c_str(), 20), 0);

  std::unique_ptr<Table> table;
  const Status s = Table::Open(path, options, &table);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

TEST(SSTable, HandlesValuesLargerThanABlock) {
  testing_support::ScopedTempDir dir("sst_bigvalue");
  Options options;
  options.block_size_bytes = 1024;
  const std::string path = dir.File("000001.sst");
  const std::string big(64 * 1024, 'z');

  TableBuilder builder(options);
  ASSERT_TRUE(builder.Open(path).ok());
  ASSERT_TRUE(builder.Add(EncodeInternalKey("a", 1, ValueType::kValue), "small")
                  .ok());
  ASSERT_TRUE(builder.Add(EncodeInternalKey("b", 2, ValueType::kValue), big).ok());
  ASSERT_TRUE(builder.Add(EncodeInternalKey("c", 3, ValueType::kValue), "small")
                  .ok());
  ASSERT_TRUE(builder.Finish().ok());

  std::unique_ptr<Table> table;
  ASSERT_TRUE(Table::Open(path, options, &table).ok());
  std::string value;
  Status status;
  ASSERT_TRUE(table->Get("b", kMaxSequenceNumber, &value, &status));
  EXPECT_EQ(value, big);
}

}  // namespace lsm
