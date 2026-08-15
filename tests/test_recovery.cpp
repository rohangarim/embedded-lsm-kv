#include <dirent.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
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

Options RecoveryOptions() {
  Options options;
  options.memtable_size_bytes = 32 * 1024;
  options.block_size_bytes = 1024;
  options.max_bytes_for_level_base = 128 * 1024;
  options.target_file_size = 64 * 1024;
  options.l0_compaction_trigger = 3;
  options.sync_policy = SyncPolicy::kEveryWrite;
  options.background_compaction = false;
  return options;
}

std::unique_ptr<DB> OpenDB(const std::string& path, const Options& options) {
  std::unique_ptr<DB> db;
  const Status s = DB::Open(options, path, &db);
  EXPECT_TRUE(s.ok()) << s.ToString();
  return db;
}

std::vector<std::string> ListFiles(const std::string& dir,
                                   const std::string& suffix) {
  std::vector<std::string> names;
  if (DIR* d = ::opendir(dir.c_str())) {
    while (struct dirent* entry = ::readdir(d)) {
      const std::string name = entry->d_name;
      if (name.size() > suffix.size() &&
          name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
        names.push_back(name);
      }
    }
    ::closedir(d);
  }
  std::sort(names.begin(), names.end());
  return names;
}

// Chops `bytes` off the newest WAL, simulating a write that was interrupted
// partway through hitting the disk.
void TruncateNewestLog(const std::string& dir, off_t bytes) {
  const auto logs = ListFiles(dir, ".log");
  ASSERT_FALSE(logs.empty());
  const std::string path = dir + "/" + logs.back();
  struct stat st;
  ASSERT_EQ(::stat(path.c_str(), &st), 0);
  ASSERT_EQ(::truncate(path.c_str(), std::max<off_t>(0, st.st_size - bytes)), 0);
}

}  // namespace

TEST(Recovery, ReopenAfterCleanCloseSeesEverything) {
  ScopedTempDir dir("rec_clean");
  const std::string path = dir.File("db");

  {
    auto db = OpenDB(path, RecoveryOptions());
    for (int i = 0; i < 2000; ++i) {
      ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Value(i)).ok());
    }
  }

  auto db = OpenDB(path, RecoveryOptions());
  for (int i = 0; i < 2000; ++i) {
    std::string value;
    ASSERT_TRUE(db->Get(ReadOptions(), Key(i), &value).ok()) << i;
    EXPECT_EQ(value, Value(i));
  }
}

// The core durability claim: data that only ever reached the WAL -- never an
// SSTable -- comes back after a restart.
TEST(Recovery, DataStillInTheMemtableIsRecoveredFromTheWal) {
  ScopedTempDir dir("rec_wal");
  const std::string path = dir.File("db");

  {
    auto db = OpenDB(path, RecoveryOptions());
    for (int i = 0; i < 100; ++i) {  // Far below the flush threshold.
      ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Value(i)).ok());
    }
    // No flush, no compaction: everything lives in the memtable and the log.
    EXPECT_EQ(db->GetStats().memtable_flushes, 0u);
  }

  auto db = OpenDB(path, RecoveryOptions());
  for (int i = 0; i < 100; ++i) {
    std::string value;
    ASSERT_TRUE(db->Get(ReadOptions(), Key(i), &value).ok()) << i;
    EXPECT_EQ(value, Value(i));
  }
}

TEST(Recovery, DeletesSurviveRestart) {
  ScopedTempDir dir("rec_delete");
  const std::string path = dir.File("db");

  {
    auto db = OpenDB(path, RecoveryOptions());
    for (int i = 0; i < 500; ++i) {
      ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Value(i)).ok());
    }
    ASSERT_TRUE(db->FlushMemTable().ok());
    for (int i = 0; i < 500; i += 3) {
      ASSERT_TRUE(db->Delete(WriteOptions(), Key(i)).ok());
    }
  }

  auto db = OpenDB(path, RecoveryOptions());
  for (int i = 0; i < 500; ++i) {
    std::string value;
    const Status s = db->Get(ReadOptions(), Key(i), &value);
    if (i % 3 == 0) {
      EXPECT_TRUE(s.IsNotFound()) << i << ": " << s.ToString();
    } else {
      ASSERT_TRUE(s.ok()) << i;
      EXPECT_EQ(value, Value(i));
    }
  }
}

TEST(Recovery, SurvivesRepeatedRestarts) {
  ScopedTempDir dir("rec_repeat");
  const std::string path = dir.File("db");
  int written = 0;

  for (int round = 0; round < 10; ++round) {
    auto db = OpenDB(path, RecoveryOptions());
    for (int i = 0; i < 300; ++i) {
      ASSERT_TRUE(db->Put(WriteOptions(), Key(written), Value(written)).ok());
      ++written;
    }
    // Everything written in any previous round must still be readable.
    for (int i = 0; i < written; i += 17) {
      std::string value;
      ASSERT_TRUE(db->Get(ReadOptions(), Key(i), &value).ok())
          << "round " << round << " key " << i;
      EXPECT_EQ(value, Value(i));
    }
  }
}

// A torn tail record must be dropped, and every complete record before it must
// survive. Losing a prefix would be data loss; keeping a fragment would be
// corruption.
TEST(Recovery, TruncatedWalTailLosesOnlyTheTail) {
  for (const off_t cut : {1, 7, 30, 100}) {
    ScopedTempDir dir("rec_torn");
    const std::string path = dir.File("db");
    constexpr int kWrites = 200;

    {
      auto db = OpenDB(path, RecoveryOptions());
      for (int i = 0; i < kWrites; ++i) {
        ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Value(i)).ok());
      }
    }
    TruncateNewestLog(path, cut);

    auto db = OpenDB(path, RecoveryOptions());
    int recovered = 0;
    for (int i = 0; i < kWrites; ++i) {
      std::string value;
      if (db->Get(ReadOptions(), Key(i), &value).ok()) {
        EXPECT_EQ(value, Value(i));
        ++recovered;
      }
    }
    // A prefix survived, and it is a *contiguous* prefix.
    EXPECT_GE(recovered, kWrites - 3) << "cut=" << cut;
    for (int i = 0; i < recovered; ++i) {
      std::string value;
      EXPECT_TRUE(db->Get(ReadOptions(), Key(i), &value).ok())
          << "cut=" << cut << " key=" << i;
    }
  }
}

TEST(Recovery, ObsoleteLogsAreCleanedUp) {
  ScopedTempDir dir("rec_cleanup");
  const std::string path = dir.File("db");
  {
    // fsync-per-write is slow by design, so keep the volume modest here; the
    // throughput cost of the policy is measured in the benchmark instead.
    Options options = RecoveryOptions();
    options.sync_policy = SyncPolicy::kNever;
    auto db = OpenDB(path, options);
    for (int i = 0; i < 5000; ++i) {
      ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Value(i)).ok());
    }
    ASSERT_TRUE(db->CompactAll().ok());
  }
  auto db = OpenDB(path, RecoveryOptions());
  // Logs whose contents are already in an SSTable named by the manifest are
  // deleted; at most the active one and one being flushed remain.
  EXPECT_LE(ListFiles(path, ".log").size(), 2u);
}

TEST(Recovery, CorruptManifestIsRejectedRatherThanSilentlyIgnored) {
  ScopedTempDir dir("rec_manifest");
  const std::string path = dir.File("db");
  {
    Options options = RecoveryOptions();
    options.sync_policy = SyncPolicy::kNever;  // Not what this test is about.
    auto db = OpenDB(path, options);
    for (int i = 0; i < 3000; ++i) {
      ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Value(i)).ok());
    }
    ASSERT_TRUE(db->CompactAll().ok());
  }

  const std::string manifest = path + "/MANIFEST";
  struct stat st;
  ASSERT_EQ(::stat(manifest.c_str(), &st), 0);
  ASSERT_GT(st.st_size, 40);
  const int fd = ::open(manifest.c_str(), O_RDWR);
  ASSERT_GE(fd, 0);
  char byte;
  ASSERT_EQ(::pread(fd, &byte, 1, 33), 1);
  byte = static_cast<char>(byte ^ 0x5a);
  ASSERT_EQ(::pwrite(fd, &byte, 1, 33), 1);
  ::close(fd);

  std::unique_ptr<DB> db;
  const Status s = DB::Open(RecoveryOptions(), path, &db);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

TEST(Recovery, SequenceNumbersDoNotRestartAfterReopen) {
  ScopedTempDir dir("rec_seq");
  const std::string path = dir.File("db");

  {
    auto db = OpenDB(path, RecoveryOptions());
    ASSERT_TRUE(db->Put(WriteOptions(), "k", "old").ok());
    ASSERT_TRUE(db->FlushMemTable().ok());
  }
  {
    // If the sequence counter reset to zero here, this newer write could be
    // shadowed by the older on-disk version.
    auto db = OpenDB(path, RecoveryOptions());
    ASSERT_TRUE(db->Put(WriteOptions(), "k", "new").ok());
  }
  auto db = OpenDB(path, RecoveryOptions());
  std::string value;
  ASSERT_TRUE(db->Get(ReadOptions(), "k", &value).ok());
  EXPECT_EQ(value, "new");
}

TEST(Recovery, NonSyncedWritesStillSurviveProcessLevelRestart) {
  // With sync_policy kNever the bytes are only in the page cache, which the
  // kernel owns -- so a restart of the process (as opposed to the machine)
  // still recovers them.
  ScopedTempDir dir("rec_nosync");
  const std::string path = dir.File("db");
  Options options = RecoveryOptions();
  options.sync_policy = SyncPolicy::kNever;

  {
    auto db = OpenDB(path, options);
    for (int i = 0; i < 300; ++i) {
      ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Value(i)).ok());
    }
    EXPECT_EQ(db->GetStats().wal_syncs, 0u);
  }
  auto db = OpenDB(path, options);
  for (int i = 0; i < 300; ++i) {
    std::string value;
    ASSERT_TRUE(db->Get(ReadOptions(), Key(i), &value).ok()) << i;
  }
}

}  // namespace lsm
