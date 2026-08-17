#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "lsm/db.h"
#include "test_util.h"

namespace lsm {
namespace {

using testing_support::Key;
using testing_support::ScopedTempDir;
using testing_support::Value;

Options SnapshotOptions() {
  Options options;
  options.memtable_size_bytes = 64 * 1024;
  options.block_size_bytes = 1024;
  options.max_bytes_for_level_base = 256 * 1024;
  options.target_file_size = 128 * 1024;
  options.l0_compaction_trigger = 3;
  options.sync_policy = SyncPolicy::kNever;
  options.background_compaction = false;  // Deterministic for assertions.
  return options;
}

std::unique_ptr<DB> OpenDB(const std::string& path) {
  std::unique_ptr<DB> db;
  const Status s = DB::Open(SnapshotOptions(), path, &db);
  EXPECT_TRUE(s.ok()) << s.ToString();
  return db;
}

// Reads `key` as of `snapshot` (or latest when null).
Status ReadAt(DB* db, const Snapshot* snapshot, const std::string& key,
              std::string* value) {
  ReadOptions options;
  options.snapshot = snapshot;
  return db->Get(options, key, value);
}

}  // namespace

TEST(Snapshot, SeesTheValueThatExistedWhenItWasTaken) {
  ScopedTempDir dir("snap_overwrite");
  auto db = OpenDB(dir.File("db"));

  ASSERT_TRUE(db->Put(WriteOptions(), "k", "original").ok());
  const Snapshot* snapshot = db->GetSnapshot();
  ASSERT_TRUE(db->Put(WriteOptions(), "k", "updated").ok());

  std::string value;
  ASSERT_TRUE(ReadAt(db.get(), snapshot, "k", &value).ok());
  EXPECT_EQ(value, "original");

  ASSERT_TRUE(ReadAt(db.get(), nullptr, "k", &value).ok());
  EXPECT_EQ(value, "updated");

  db->ReleaseSnapshot(snapshot);
}

TEST(Snapshot, SeesAKeyThatWasLaterDeleted) {
  ScopedTempDir dir("snap_delete");
  auto db = OpenDB(dir.File("db"));

  ASSERT_TRUE(db->Put(WriteOptions(), "k", "alive").ok());
  const Snapshot* snapshot = db->GetSnapshot();
  ASSERT_TRUE(db->Delete(WriteOptions(), "k").ok());

  std::string value;
  ASSERT_TRUE(ReadAt(db.get(), snapshot, "k", &value).ok());
  EXPECT_EQ(value, "alive");
  EXPECT_TRUE(ReadAt(db.get(), nullptr, "k", &value).IsNotFound());

  db->ReleaseSnapshot(snapshot);
}

TEST(Snapshot, DoesNotSeeKeysWrittenAfterIt) {
  ScopedTempDir dir("snap_future");
  auto db = OpenDB(dir.File("db"));

  ASSERT_TRUE(db->Put(WriteOptions(), "before", "v").ok());
  const Snapshot* snapshot = db->GetSnapshot();
  ASSERT_TRUE(db->Put(WriteOptions(), "after", "v").ok());

  std::string value;
  EXPECT_TRUE(ReadAt(db.get(), snapshot, "before", &value).ok());
  EXPECT_TRUE(ReadAt(db.get(), snapshot, "after", &value).IsNotFound());
  EXPECT_TRUE(ReadAt(db.get(), nullptr, "after", &value).ok());

  db->ReleaseSnapshot(snapshot);
}

// The flush collapses shadowed versions. It must keep the ones a snapshot can
// still see -- this is the case that made the collapse unsafe before snapshots
// were accounted for.
TEST(Snapshot, SurvivesTheMemtableFlushCollapse) {
  ScopedTempDir dir("snap_flush");
  auto db = OpenDB(dir.File("db"));

  constexpr int kKeys = 100;
  for (int i = 0; i < kKeys; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Value(i)).ok());
  }
  const Snapshot* snapshot = db->GetSnapshot();

  // Bury each key under many newer versions, all in the same memtable.
  for (int round = 1; round <= 50; ++round) {
    for (int i = 0; i < kKeys; ++i) {
      ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Value(i + round * 1000)).ok());
    }
  }
  ASSERT_TRUE(db->FlushMemTable().ok());

  std::string value;
  for (int i = 0; i < kKeys; ++i) {
    ASSERT_TRUE(ReadAt(db.get(), snapshot, Key(i), &value).ok()) << i;
    EXPECT_EQ(value, Value(i)) << i;
    ASSERT_TRUE(ReadAt(db.get(), nullptr, Key(i), &value).ok()) << i;
    EXPECT_EQ(value, Value(i + 50 * 1000)) << i;
  }
  db->ReleaseSnapshot(snapshot);
}

// Same guarantee, but across compaction, which is where versions are physically
// discarded.
TEST(Snapshot, SurvivesCompaction) {
  ScopedTempDir dir("snap_compact");
  auto db = OpenDB(dir.File("db"));

  constexpr int kKeys = 300;
  for (int i = 0; i < kKeys; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Value(i)).ok());
  }
  ASSERT_TRUE(db->FlushMemTable().ok());
  const Snapshot* snapshot = db->GetSnapshot();

  for (int round = 1; round <= 30; ++round) {
    for (int i = 0; i < kKeys; ++i) {
      ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Value(i + round * 1000)).ok());
    }
    ASSERT_TRUE(db->FlushMemTable().ok());
  }
  ASSERT_TRUE(db->CompactAll().ok());

  std::string value;
  for (int i = 0; i < kKeys; ++i) {
    ASSERT_TRUE(ReadAt(db.get(), snapshot, Key(i), &value).ok()) << i;
    EXPECT_EQ(value, Value(i)) << i;
  }
  db->ReleaseSnapshot(snapshot);
}

TEST(Snapshot, DeletedKeyIsStillVisibleThroughCompaction) {
  ScopedTempDir dir("snap_tombstone");
  auto db = OpenDB(dir.File("db"));

  constexpr int kKeys = 200;
  for (int i = 0; i < kKeys; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Value(i)).ok());
  }
  ASSERT_TRUE(db->FlushMemTable().ok());
  const Snapshot* snapshot = db->GetSnapshot();

  for (int i = 0; i < kKeys; ++i) {
    ASSERT_TRUE(db->Delete(WriteOptions(), Key(i)).ok());
  }
  ASSERT_TRUE(db->CompactAll().ok());

  std::string value;
  for (int i = 0; i < kKeys; ++i) {
    ASSERT_TRUE(ReadAt(db.get(), snapshot, Key(i), &value).ok()) << i;
    EXPECT_EQ(value, Value(i)) << i;
    EXPECT_TRUE(ReadAt(db.get(), nullptr, Key(i), &value).IsNotFound()) << i;
  }
  db->ReleaseSnapshot(snapshot);
}

TEST(Snapshot, IteratorScansTheSnapshotState) {
  ScopedTempDir dir("snap_iter");
  auto db = OpenDB(dir.File("db"));

  constexpr int kKeys = 500;
  for (int i = 0; i < kKeys; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Value(i)).ok());
  }
  const Snapshot* snapshot = db->GetSnapshot();

  // Rewrite everything and delete half of it after the snapshot.
  for (int i = 0; i < kKeys; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Value(i + 900000)).ok());
  }
  for (int i = 0; i < kKeys; i += 2) {
    ASSERT_TRUE(db->Delete(WriteOptions(), Key(i)).ok());
  }
  ASSERT_TRUE(db->CompactAll().ok());

  ReadOptions options;
  options.snapshot = snapshot;
  auto iter = db->NewIterator(options);
  int rows = 0;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    ASSERT_EQ(iter->key(), Key(rows));
    EXPECT_EQ(iter->value(), Value(rows));
    ++rows;
  }
  EXPECT_EQ(rows, kKeys) << "snapshot scan should see every original key";
  EXPECT_TRUE(iter->status().ok());

  db->ReleaseSnapshot(snapshot);
}

TEST(Snapshot, MultipleSnapshotsEachSeeTheirOwnState) {
  ScopedTempDir dir("snap_multi");
  auto db = OpenDB(dir.File("db"));

  std::vector<const Snapshot*> snapshots;
  for (int round = 0; round < 10; ++round) {
    ASSERT_TRUE(db->Put(WriteOptions(), "k", "v" + std::to_string(round)).ok());
    snapshots.push_back(db->GetSnapshot());
  }
  ASSERT_TRUE(db->CompactAll().ok());

  std::string value;
  for (int round = 0; round < 10; ++round) {
    ASSERT_TRUE(ReadAt(db.get(), snapshots[round], "k", &value).ok()) << round;
    EXPECT_EQ(value, "v" + std::to_string(round)) << round;
  }
  for (const Snapshot* snapshot : snapshots) db->ReleaseSnapshot(snapshot);
}

// Once nothing is pinned, the collapse goes back to being total. Without this,
// snapshots would be a permanent space leak rather than a temporary hold.
TEST(Snapshot, ReleasingLetsTheCollapseResume) {
  ScopedTempDir dir("snap_release");
  auto db = OpenDB(dir.File("db"));

  constexpr int kKeys = 20;
  constexpr int kVersions = 100;
  // Many versions of each key inside a single memtable, so the flush has
  // something it could collapse.
  auto write_versions = [&](int base) {
    for (int round = 0; round < kVersions; ++round) {
      for (int i = 0; i < kKeys; ++i) {
        ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Value(base + round)).ok());
      }
    }
  };

  const Snapshot* snapshot = db->GetSnapshot();
  write_versions(1000);
  ASSERT_TRUE(db->FlushMemTable().ok());
  const uint64_t dropped_while_pinned = db->GetStats().flush_versions_dropped;
  EXPECT_EQ(dropped_while_pinned, 0u)
      << "a live snapshot must keep every version newer than it";

  db->ReleaseSnapshot(snapshot);

  write_versions(9000);
  ASSERT_TRUE(db->FlushMemTable().ok());
  EXPECT_GT(db->GetStats().flush_versions_dropped, dropped_while_pinned)
      << "with nothing pinned the collapse should resume";

  std::string value;
  for (int i = 0; i < kKeys; ++i) {
    ASSERT_TRUE(ReadAt(db.get(), nullptr, Key(i), &value).ok()) << i;
    EXPECT_EQ(value, Value(9000 + kVersions - 1)) << i;
  }
}

TEST(Snapshot, ReleasingNullIsHarmless) {
  ScopedTempDir dir("snap_null");
  auto db = OpenDB(dir.File("db"));
  db->ReleaseSnapshot(nullptr);  // Must not crash.
  ASSERT_TRUE(db->Put(WriteOptions(), "k", "v").ok());
}

TEST(Snapshot, SnapshotOfAnEmptyDatabaseSeesNothing) {
  ScopedTempDir dir("snap_empty");
  auto db = OpenDB(dir.File("db"));

  const Snapshot* snapshot = db->GetSnapshot();
  ASSERT_TRUE(db->Put(WriteOptions(), "k", "v").ok());

  std::string value;
  EXPECT_TRUE(ReadAt(db.get(), snapshot, "k", &value).IsNotFound());

  ReadOptions options;
  options.snapshot = snapshot;
  auto iter = db->NewIterator(options);
  iter->SeekToFirst();
  EXPECT_FALSE(iter->Valid());

  db->ReleaseSnapshot(snapshot);
}

}  // namespace lsm
