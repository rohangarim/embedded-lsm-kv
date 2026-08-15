#include "lsm/db.h"

#include <gtest/gtest.h>

#include <memory>
#include <set>
#include <string>

#include "test_util.h"

namespace lsm {
namespace {

using testing_support::Key;
using testing_support::ScopedTempDir;
using testing_support::Value;

// Small thresholds so a few thousand keys actually exercise flush and
// compaction rather than sitting in one memtable.
Options SmallOptions() {
  Options options;
  options.memtable_size_bytes = 64 * 1024;
  options.block_size_bytes = 1024;
  options.max_bytes_for_level_base = 256 * 1024;
  options.target_file_size = 128 * 1024;
  options.l0_compaction_trigger = 3;
  options.sync_policy = SyncPolicy::kNever;  // Durability is tested elsewhere.
  options.background_compaction = false;     // Deterministic for assertions.
  return options;
}

std::unique_ptr<DB> OpenDB(const std::string& path, const Options& options) {
  std::unique_ptr<DB> db;
  const Status s = DB::Open(options, path, &db);
  EXPECT_TRUE(s.ok()) << s.ToString();
  return db;
}

std::string MustGet(DB* db, const std::string& key) {
  std::string value;
  const Status s = db->Get(ReadOptions(), key, &value);
  EXPECT_TRUE(s.ok()) << key << ": " << s.ToString();
  return value;
}

void ExpectMissing(DB* db, const std::string& key) {
  std::string value;
  const Status s = db->Get(ReadOptions(), key, &value);
  EXPECT_TRUE(s.IsNotFound()) << key << ": " << s.ToString();
}

}  // namespace

TEST(DB, OpenCreatesDirectory) {
  ScopedTempDir dir("db_open");
  const std::string path = dir.File("newdb");
  auto db = OpenDB(path, SmallOptions());
  ASSERT_NE(db, nullptr);

  struct stat st;
  EXPECT_EQ(::stat(path.c_str(), &st), 0);
  EXPECT_TRUE(S_ISDIR(st.st_mode));
}

TEST(DB, RefusesToCreateWhenCreateIfMissingIsFalse) {
  ScopedTempDir dir("db_nocreate");
  Options options = SmallOptions();
  options.create_if_missing = false;
  std::unique_ptr<DB> db;
  EXPECT_TRUE(DB::Open(options, dir.File("absent"), &db).IsNotFound());
}

TEST(DB, PutGetDelete) {
  ScopedTempDir dir("db_basic");
  auto db = OpenDB(dir.File("db"), SmallOptions());

  ASSERT_TRUE(db->Put(WriteOptions(), "alpha", "one").ok());
  ASSERT_TRUE(db->Put(WriteOptions(), "beta", "two").ok());
  EXPECT_EQ(MustGet(db.get(), "alpha"), "one");
  EXPECT_EQ(MustGet(db.get(), "beta"), "two");
  ExpectMissing(db.get(), "gamma");

  ASSERT_TRUE(db->Delete(WriteOptions(), "alpha").ok());
  ExpectMissing(db.get(), "alpha");
  EXPECT_EQ(MustGet(db.get(), "beta"), "two");

  // Deleting a key that was never there is not an error.
  ASSERT_TRUE(db->Delete(WriteOptions(), "never_existed").ok());
}

TEST(DB, OverwriteReturnsNewestValue) {
  ScopedTempDir dir("db_overwrite");
  auto db = OpenDB(dir.File("db"), SmallOptions());
  for (int i = 0; i < 20; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions(), "k", "v" + std::to_string(i)).ok());
  }
  EXPECT_EQ(MustGet(db.get(), "k"), "v19");

  // Same, but with a flush in the middle so the versions span memtable and disk.
  ASSERT_TRUE(db->FlushMemTable().ok());
  ASSERT_TRUE(db->Put(WriteOptions(), "k", "after_flush").ok());
  EXPECT_EQ(MustGet(db.get(), "k"), "after_flush");
  ASSERT_TRUE(db->FlushMemTable().ok());
  EXPECT_EQ(MustGet(db.get(), "k"), "after_flush");
}

TEST(DB, EmptyValuesAndBinaryKeys) {
  ScopedTempDir dir("db_binary");
  auto db = OpenDB(dir.File("db"), SmallOptions());
  const std::string binary_key("\0\x01\xff""mid\0", 7);

  ASSERT_TRUE(db->Put(WriteOptions(), binary_key, "").ok());
  EXPECT_EQ(MustGet(db.get(), binary_key), "");
  ASSERT_TRUE(db->FlushMemTable().ok());
  EXPECT_EQ(MustGet(db.get(), binary_key), "");
}

TEST(DB, ReadsSpanMemtableAndSSTables) {
  ScopedTempDir dir("db_layers");
  auto db = OpenDB(dir.File("db"), SmallOptions());

  for (int i = 0; i < 500; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Value(i)).ok());
  }
  ASSERT_TRUE(db->FlushMemTable().ok());  // 0..499 now on disk.
  for (int i = 500; i < 1000; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Value(i)).ok());
  }

  for (int i = 0; i < 1000; ++i) EXPECT_EQ(MustGet(db.get(), Key(i)), Value(i));
}

TEST(DB, FlushProducesSSTables) {
  ScopedTempDir dir("db_flush");
  auto db = OpenDB(dir.File("db"), SmallOptions());
  for (int i = 0; i < 2000; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Value(i)).ok());
  }
  ASSERT_TRUE(db->FlushMemTable().ok());

  const DbStats stats = db->GetStats();
  EXPECT_GT(stats.memtable_flushes, 0u);
  EXPECT_FALSE(db->DebugLevelSummary().empty());
}

TEST(DB, CompactionPushesDataOutOfLevelZero) {
  ScopedTempDir dir("db_compact");
  auto db = OpenDB(dir.File("db"), SmallOptions());

  for (int i = 0; i < 20000; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Value(i)).ok());
  }
  ASSERT_TRUE(db->CompactAll().ok());

  const DbStats stats = db->GetStats();
  EXPECT_GT(stats.compactions, 0u);
  EXPECT_GT(stats.memtable_flushes, 1u);

  for (int i = 0; i < 20000; i += 7) {
    EXPECT_EQ(MustGet(db.get(), Key(i)), Value(i)) << i;
  }
}

TEST(DB, CompactionDropsShadowedVersions) {
  ScopedTempDir dir("db_shadow");
  Options options = SmallOptions();
  auto db = OpenDB(dir.File("db"), options);

  // Rewrite a small key space many times over. Compaction should collapse each
  // key to its newest version, so the on-disk footprint stays bounded even
  // though we wrote 20x that much.
  constexpr int kKeys = 500;
  for (int round = 0; round < 20; ++round) {
    for (int i = 0; i < kKeys; ++i) {
      ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Value(round * 100000 + i)).ok());
    }
    ASSERT_TRUE(db->FlushMemTable().ok());
  }
  ASSERT_TRUE(db->CompactAll().ok());

  for (int i = 0; i < kKeys; ++i) {
    EXPECT_EQ(MustGet(db.get(), Key(i)), Value(19 * 100000 + i)) << i;
  }

  const DbStats stats = db->GetStats();
  EXPECT_LT(stats.compaction_output_bytes, stats.compaction_input_bytes)
      << "compaction should shrink the data it merges";
}

TEST(DB, DeletedKeysStayDeletedAcrossCompaction) {
  ScopedTempDir dir("db_delete_compact");
  auto db = OpenDB(dir.File("db"), SmallOptions());

  for (int i = 0; i < 5000; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Value(i)).ok());
  }
  ASSERT_TRUE(db->FlushMemTable().ok());

  for (int i = 0; i < 5000; i += 2) {
    ASSERT_TRUE(db->Delete(WriteOptions(), Key(i)).ok());
  }
  ASSERT_TRUE(db->CompactAll().ok());

  // The tombstone must win even though the older value sits in a deeper level.
  for (int i = 0; i < 5000; ++i) {
    if (i % 2 == 0) {
      ExpectMissing(db.get(), Key(i));
    } else {
      EXPECT_EQ(MustGet(db.get(), Key(i)), Value(i)) << i;
    }
  }
}

TEST(DB, ReinsertAfterDeleteIsVisible) {
  ScopedTempDir dir("db_resurrect");
  auto db = OpenDB(dir.File("db"), SmallOptions());

  ASSERT_TRUE(db->Put(WriteOptions(), "k", "first").ok());
  ASSERT_TRUE(db->FlushMemTable().ok());
  ASSERT_TRUE(db->Delete(WriteOptions(), "k").ok());
  ASSERT_TRUE(db->FlushMemTable().ok());
  ASSERT_TRUE(db->Put(WriteOptions(), "k", "second").ok());
  ASSERT_TRUE(db->CompactAll().ok());

  EXPECT_EQ(MustGet(db.get(), "k"), "second");
}

TEST(DB, IteratorScansLiveKeysInOrder) {
  ScopedTempDir dir("db_iter");
  auto db = OpenDB(dir.File("db"), SmallOptions());

  constexpr int kKeys = 3000;
  for (const int i : testing_support::ShuffledRange(kKeys, 5)) {
    ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Value(i)).ok());
  }
  ASSERT_TRUE(db->FlushMemTable().ok());
  for (int i = 0; i < kKeys; i += 3) {
    ASSERT_TRUE(db->Delete(WriteOptions(), Key(i)).ok());
  }

  auto iter = db->NewIterator(ReadOptions());
  int expected = 0;
  int seen = 0;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    while (expected % 3 == 0) ++expected;  // Skip the keys we deleted.
    ASSERT_EQ(iter->key(), Key(expected)) << "at position " << seen;
    EXPECT_EQ(iter->value(), Value(expected));
    ++expected;
    ++seen;
  }
  EXPECT_EQ(seen, kKeys - (kKeys + 2) / 3);
  EXPECT_TRUE(iter->status().ok());
}

TEST(DB, IteratorSeekPositionsCorrectly) {
  ScopedTempDir dir("db_iter_seek");
  auto db = OpenDB(dir.File("db"), SmallOptions());
  for (int i = 0; i < 1000; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions(), Key(i * 10), Value(i)).ok());
  }
  ASSERT_TRUE(db->CompactAll().ok());

  auto iter = db->NewIterator(ReadOptions());
  iter->Seek(Key(5000));
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), Key(5000));

  iter->Seek(Key(5001));  // Between entries.
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(iter->key(), Key(5010));

  iter->Seek(Key(999999));
  EXPECT_FALSE(iter->Valid());
}

TEST(DB, IteratorOverEmptyDatabase) {
  ScopedTempDir dir("db_iter_empty");
  auto db = OpenDB(dir.File("db"), SmallOptions());
  auto iter = db->NewIterator(ReadOptions());
  iter->SeekToFirst();
  EXPECT_FALSE(iter->Valid());
}

TEST(DB, StatsTrackWriteAmplification) {
  ScopedTempDir dir("db_stats");
  auto db = OpenDB(dir.File("db"), SmallOptions());
  for (int i = 0; i < 10000; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Value(i)).ok());
  }
  ASSERT_TRUE(db->CompactAll().ok());

  const DbStats stats = db->GetStats();
  EXPECT_EQ(stats.writes, 10000u);
  EXPECT_GT(stats.user_bytes_written, 0u);
  // Every byte is written at least once to the WAL and once to an SSTable, so
  // amplification is above 2 by construction; compaction adds more.
  EXPECT_GT(stats.WriteAmplification(), 2.0);
  std::printf("[db] write amplification: %.2fx across %llu compactions\n",
              stats.WriteAmplification(),
              static_cast<unsigned long long>(stats.compactions));
}

TEST(DB, BackgroundCompactionKeepsDataCorrect) {
  ScopedTempDir dir("db_background");
  Options options = SmallOptions();
  options.background_compaction = true;
  auto db = OpenDB(dir.File("db"), options);

  for (int i = 0; i < 20000; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Value(i)).ok());
  }
  db->WaitForBackgroundWork();
  for (int i = 0; i < 20000; i += 11) {
    EXPECT_EQ(MustGet(db.get(), Key(i)), Value(i)) << i;
  }
}

}  // namespace lsm
