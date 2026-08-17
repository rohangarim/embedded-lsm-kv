#include "lsm/write_batch.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "lsm/db.h"
#include "test_util.h"

namespace lsm {
namespace {

using testing_support::Key;
using testing_support::ScopedTempDir;
using testing_support::Value;

using Entry = std::tuple<ValueType, std::string, std::string>;

std::vector<Entry> Drain(const WriteBatch& batch) {
  std::vector<Entry> out;
  const Status s = batch.ForEach(
      [&](ValueType type, std::string_view key, std::string_view value) {
        out.emplace_back(type, std::string(key), std::string(value));
      });
  EXPECT_TRUE(s.ok()) << s.ToString();
  return out;
}

Options BatchOptions() {
  Options options;
  options.memtable_size_bytes = 64 * 1024;
  options.block_size_bytes = 1024;
  options.max_bytes_for_level_base = 256 * 1024;
  options.target_file_size = 128 * 1024;
  options.l0_compaction_trigger = 3;
  options.sync_policy = SyncPolicy::kNever;
  options.background_compaction = false;
  return options;
}

std::unique_ptr<DB> OpenDB(const std::string& path, const Options& options) {
  std::unique_ptr<DB> db;
  const Status s = DB::Open(options, path, &db);
  EXPECT_TRUE(s.ok()) << s.ToString();
  return db;
}

}  // namespace

TEST(WriteBatch, EmptyBatchHasNothingInIt) {
  WriteBatch batch;
  EXPECT_TRUE(batch.empty());
  EXPECT_EQ(batch.Count(), 0u);
  EXPECT_TRUE(Drain(batch).empty());
}

TEST(WriteBatch, RecordsPutsAndDeletesInOrder) {
  WriteBatch batch;
  batch.Put("a", "1");
  batch.Delete("b");
  batch.Put("c", "3");

  EXPECT_EQ(batch.Count(), 3u);
  const auto entries = Drain(batch);
  ASSERT_EQ(entries.size(), 3u);
  EXPECT_EQ(entries[0], Entry(ValueType::kValue, "a", "1"));
  EXPECT_EQ(entries[1], Entry(ValueType::kDeletion, "b", ""));
  EXPECT_EQ(entries[2], Entry(ValueType::kValue, "c", "3"));
}

TEST(WriteBatch, HandlesEmptyAndBinaryKeysAndValues) {
  WriteBatch batch;
  batch.Put("", "");
  batch.Put(std::string("\0\xff", 2), std::string("\0v\0", 3));
  batch.Delete(std::string("\0", 1));

  const auto entries = Drain(batch);
  ASSERT_EQ(entries.size(), 3u);
  EXPECT_EQ(std::get<1>(entries[0]), "");
  EXPECT_EQ(std::get<2>(entries[0]), "");
  EXPECT_EQ(std::get<1>(entries[1]), std::string("\0\xff", 2));
  EXPECT_EQ(std::get<2>(entries[1]), std::string("\0v\0", 3));
  EXPECT_EQ(std::get<1>(entries[2]), std::string("\0", 1));
}

TEST(WriteBatch, ClearResetsEverything) {
  WriteBatch batch;
  batch.Put("a", "1");
  batch.Clear();
  EXPECT_TRUE(batch.empty());
  EXPECT_EQ(batch.ApproximateSize(), 0u);

  batch.Put("b", "2");
  EXPECT_EQ(batch.Count(), 1u);
  EXPECT_EQ(std::get<1>(Drain(batch)[0]), "b");
}

TEST(WriteBatch, LargeValuesRoundTrip) {
  WriteBatch batch;
  const std::string big(1 << 20, 'x');  // Forces multi-byte varint lengths.
  batch.Put("big", big);
  const auto entries = Drain(batch);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(std::get<2>(entries[0]), big);
}

TEST(WriteBatch, DecodeRejectsATruncatedBuffer) {
  WriteBatch batch;
  batch.Put("key", "value");
  const std::string_view entries = batch.entries();

  for (size_t cut = 1; cut < entries.size(); ++cut) {
    const Status s = WriteBatch::Decode(entries.substr(0, cut), batch.Count(),
                                        [](ValueType, std::string_view,
                                           std::string_view) {});
    EXPECT_TRUE(s.IsCorruption()) << "cut=" << cut;
  }
}

TEST(WriteBatch, DecodeRejectsACountThatDisagreesWithTheBytes) {
  WriteBatch batch;
  batch.Put("a", "1");
  batch.Put("b", "2");

  auto ignore = [](ValueType, std::string_view, std::string_view) {};
  EXPECT_TRUE(WriteBatch::Decode(batch.entries(), 1, ignore).IsCorruption())
      << "too few: trailing bytes should be caught";
  EXPECT_TRUE(WriteBatch::Decode(batch.entries(), 3, ignore).IsCorruption())
      << "too many: should run out of buffer";
}

// -------------------------------------------------------------- DB integration

TEST(WriteBatch, AppliedThroughTheDatabase) {
  ScopedTempDir dir("batch_db");
  auto db = OpenDB(dir.File("db"), BatchOptions());

  WriteBatch batch;
  for (int i = 0; i < 100; ++i) batch.Put(Key(i), Value(i));
  ASSERT_TRUE(db->Write(WriteOptions(), batch).ok());

  std::string value;
  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(db->Get(ReadOptions(), Key(i), &value).ok()) << i;
    EXPECT_EQ(value, Value(i)) << i;
  }
}

TEST(WriteBatch, MixedPutsAndDeletesInOneBatch) {
  ScopedTempDir dir("batch_mixed");
  auto db = OpenDB(dir.File("db"), BatchOptions());

  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Value(i)).ok());
  }

  WriteBatch batch;
  for (int i = 0; i < 100; i += 2) batch.Delete(Key(i));
  for (int i = 1; i < 100; i += 2) batch.Put(Key(i), Value(i + 7000));
  ASSERT_TRUE(db->Write(WriteOptions(), batch).ok());

  std::string value;
  for (int i = 0; i < 100; ++i) {
    const Status s = db->Get(ReadOptions(), Key(i), &value);
    if (i % 2 == 0) {
      EXPECT_TRUE(s.IsNotFound()) << i;
    } else {
      ASSERT_TRUE(s.ok()) << i;
      EXPECT_EQ(value, Value(i + 7000)) << i;
    }
  }
}

// Entries get consecutive sequence numbers, so a later write to the same key
// inside one batch wins -- the same as two separate writes.
TEST(WriteBatch, LastWriteToAKeyWinsWithinABatch) {
  ScopedTempDir dir("batch_shadow");
  auto db = OpenDB(dir.File("db"), BatchOptions());

  WriteBatch batch;
  batch.Put("k", "first");
  batch.Put("k", "second");
  batch.Put("k", "third");
  ASSERT_TRUE(db->Write(WriteOptions(), batch).ok());

  std::string value;
  ASSERT_TRUE(db->Get(ReadOptions(), "k", &value).ok());
  EXPECT_EQ(value, "third");

  WriteBatch overwrite_then_delete;
  overwrite_then_delete.Put("k", "resurrected");
  overwrite_then_delete.Delete("k");
  ASSERT_TRUE(db->Write(WriteOptions(), overwrite_then_delete).ok());
  EXPECT_TRUE(db->Get(ReadOptions(), "k", &value).IsNotFound());
}

TEST(WriteBatch, EmptyBatchIsANoOp) {
  ScopedTempDir dir("batch_empty");
  auto db = OpenDB(dir.File("db"), BatchOptions());
  ASSERT_TRUE(db->Put(WriteOptions(), "k", "v").ok());
  const uint64_t writes_before = db->GetStats().writes;

  WriteBatch batch;
  ASSERT_TRUE(db->Write(WriteOptions(), batch).ok());
  EXPECT_EQ(db->GetStats().writes, writes_before);

  std::string value;
  EXPECT_TRUE(db->Get(ReadOptions(), "k", &value).ok());
}

// A batch is one snapshot boundary as well as one log record: a reader either
// sees all of it or none of it.
TEST(WriteBatch, IsAtomicWithRespectToSnapshots) {
  ScopedTempDir dir("batch_snapshot");
  auto db = OpenDB(dir.File("db"), BatchOptions());

  const Snapshot* before = db->GetSnapshot();

  WriteBatch batch;
  for (int i = 0; i < 50; ++i) batch.Put(Key(i), Value(i));
  ASSERT_TRUE(db->Write(WriteOptions(), batch).ok());

  const Snapshot* after = db->GetSnapshot();

  ReadOptions old_read;
  old_read.snapshot = before;
  ReadOptions new_read;
  new_read.snapshot = after;

  std::string value;
  for (int i = 0; i < 50; ++i) {
    EXPECT_TRUE(db->Get(old_read, Key(i), &value).IsNotFound()) << i;
    EXPECT_TRUE(db->Get(new_read, Key(i), &value).ok()) << i;
  }
  db->ReleaseSnapshot(before);
  db->ReleaseSnapshot(after);
}

TEST(WriteBatch, SurvivesRestart) {
  ScopedTempDir dir("batch_recover");
  const std::string path = dir.File("db");
  constexpr int kBatches = 40;
  constexpr int kPerBatch = 25;

  {
    auto db = OpenDB(path, BatchOptions());
    for (int b = 0; b < kBatches; ++b) {
      WriteBatch batch;
      for (int i = 0; i < kPerBatch; ++i) {
        batch.Put(Key(b * kPerBatch + i), Value(b * kPerBatch + i));
      }
      ASSERT_TRUE(db->Write(WriteOptions(), batch).ok());
    }
  }

  auto db = OpenDB(path, BatchOptions());
  std::string value;
  for (int i = 0; i < kBatches * kPerBatch; ++i) {
    ASSERT_TRUE(db->Get(ReadOptions(), Key(i), &value).ok()) << i;
    EXPECT_EQ(value, Value(i)) << i;
  }
}

TEST(WriteBatch, ABatchLargerThanTheMemtableStillApplies) {
  ScopedTempDir dir("batch_huge");
  Options options = BatchOptions();
  options.memtable_size_bytes = 16 * 1024;  // Smaller than the batch.
  auto db = OpenDB(dir.File("db"), options);

  WriteBatch batch;
  constexpr int kKeys = 2000;
  for (int i = 0; i < kKeys; ++i) batch.Put(Key(i), Value(i));
  ASSERT_TRUE(db->Write(WriteOptions(), batch).ok());

  std::string value;
  for (int i = 0; i < kKeys; ++i) {
    ASSERT_TRUE(db->Get(ReadOptions(), Key(i), &value).ok()) << i;
    EXPECT_EQ(value, Value(i)) << i;
  }
}

}  // namespace lsm
