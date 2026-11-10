#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

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

Options ManifestOptions() {
  Options options;
  options.memtable_size_bytes = 16 * 1024;
  options.block_size_bytes = 1024;
  options.max_bytes_for_level_base = 128 * 1024;
  options.target_file_size = 32 * 1024;
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

off_t FileSize(const std::string& path) {
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) return -1;
  return st.st_size;
}

}  // namespace

// A commit records what changed, not the whole file list. The cost of a commit
// should therefore track the size of the change, not the size of the database.
TEST(Manifest, CommitCostDoesNotGrowWithTheDatabase) {
  ScopedTempDir dir("manifest_cost");
  Options options = ManifestOptions();
  options.manifest_rewrite_bytes = 1u << 30;  // Never rewrite during this test.
  auto db = OpenDB(dir.File("db"), options);

  uint64_t bytes_early = 0, commits_early = 0;
  for (int i = 0; i < 20000; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Value(i)).ok());
    if (i == 4000) {
      const DbStats s = db->GetStats();
      bytes_early = s.manifest_bytes_written;
      commits_early = s.manifest_commits;
    }
  }
  ASSERT_TRUE(db->CompactAll().ok());

  const DbStats stats = db->GetStats();
  ASSERT_GT(commits_early, 1u);
  ASSERT_GT(stats.manifest_commits, commits_early);

  const double early_per_commit =
      static_cast<double>(bytes_early) / static_cast<double>(commits_early);
  const double late_per_commit =
      static_cast<double>(stats.manifest_bytes_written - bytes_early) /
      static_cast<double>(stats.manifest_commits - commits_early);

  std::printf("[manifest] bytes/commit: %.0f early, %.0f late (%llu files)\n",
              early_per_commit, late_per_commit,
              static_cast<unsigned long long>(db->GetStats().manifest_commits));

  // With a full rewrite per commit this ratio would climb with the file count.
  EXPECT_LT(late_per_commit, early_per_commit * 4.0);
}

TEST(Manifest, ReplaysManyEditsIntoTheRightFileSet) {
  ScopedTempDir dir("manifest_replay");
  const std::string path = dir.File("db");
  Options options = ManifestOptions();
  options.manifest_rewrite_bytes = 1u << 30;  // Force a long edit log.

  constexpr int kKeys = 20000;
  {
    auto db = OpenDB(path, options);
    for (int i = 0; i < kKeys; ++i) {
      ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Value(i)).ok());
    }
    ASSERT_TRUE(db->CompactAll().ok());
    EXPECT_GT(db->GetStats().manifest_commits, 5u) << "test needs many edits";
  }

  auto db = OpenDB(path, options);
  std::string value;
  for (int i = 0; i < kKeys; ++i) {
    ASSERT_TRUE(db->Get(ReadOptions(), Key(i), &value).ok()) << i;
    EXPECT_EQ(value, Value(i)) << i;
  }
}

// The edit log has to be collapsed occasionally or replay time grows without
// bound. Rewriting is atomic, so a crash mid-rewrite leaves the old manifest.
TEST(Manifest, RewritesWhenTheEditLogGrows) {
  ScopedTempDir dir("manifest_rewrite");
  const std::string path = dir.File("db");
  Options options = ManifestOptions();
  options.manifest_rewrite_bytes = 4 * 1024;  // Collapse aggressively.

  constexpr int kKeys = 20000;
  {
    auto db = OpenDB(path, options);
    for (int i = 0; i < kKeys; ++i) {
      ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Value(i)).ok());
    }
    ASSERT_TRUE(db->CompactAll().ok());
    // One rewrite happens at open; the rest are threshold-driven.
    EXPECT_GT(db->GetStats().manifest_rewrites, 1u);
    EXPECT_LT(FileSize(path + "/MANIFEST"), 64 * 1024)
        << "a collapsed manifest should stay small";
  }

  auto db = OpenDB(path, options);
  std::string value;
  for (int i = 0; i < kKeys; i += 7) {
    ASSERT_TRUE(db->Get(ReadOptions(), Key(i), &value).ok()) << i;
    EXPECT_EQ(value, Value(i)) << i;
  }
}

TEST(Manifest, RejectsAForeignFile) {
  ScopedTempDir dir("manifest_foreign");
  const std::string path = dir.File("db");
  { auto db = OpenDB(path, ManifestOptions()); }

  const int fd = ::open((path + "/MANIFEST").c_str(), O_WRONLY);
  ASSERT_GE(fd, 0);
  const char junk[8] = {'N', 'O', 'T', 'A', 'M', 'F', 'T', 9};
  ASSERT_EQ(::pwrite(fd, junk, sizeof(junk), 0), 8);
  ::close(fd);

  std::unique_ptr<DB> db;
  const Status s = DB::Open(ManifestOptions(), path, &db);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

TEST(Manifest, RejectsAnUnsupportedFormatVersion) {
  ScopedTempDir dir("manifest_version");
  const std::string path = dir.File("db");
  { auto db = OpenDB(path, ManifestOptions()); }

  const int fd = ::open((path + "/MANIFEST").c_str(), O_WRONLY);
  ASSERT_GE(fd, 0);
  const char future = 99;
  ASSERT_EQ(::pwrite(fd, &future, 1, 7), 1);
  ::close(fd);

  std::unique_ptr<DB> db;
  const Status s = DB::Open(ManifestOptions(), path, &db);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

// A crash partway through appending a commit leaves an incomplete trailing
// record. Recovery must drop that fragment and open on the last complete
// record.
//
// Note what this test does NOT do: truncate a *completed* record. That would
// simulate a state a crash cannot produce. AppendManifestRecord fsyncs the
// record before RemoveObsoleteFiles acts on it, so a durable record always
// precedes the cleanup it authorises -- rewinding past one would strand files
// that were legitimately deleted, and recovery would fail to find them. The
// ordering is the guarantee; the fragment is the only thing a crash can leave.
TEST(Manifest, PartiallyWrittenTrailingRecordIsDiscarded) {
  Options options = ManifestOptions();
  options.manifest_rewrite_bytes = 1u << 30;

  // A header claiming more payload than follows, a header alone, and a header
  // with a payload whose CRC will not match.
  const std::vector<std::string> fragments = {
      std::string("\x11\x22\x33\x44\xff\x00\x00\x00", 8),
      std::string("\x01\x02\x03", 3),
      std::string("\xaa\xbb\xcc\xdd\x08\x00\x00\x00garbage!", 16),
  };

  for (size_t i = 0; i < fragments.size(); ++i) {
    ScopedTempDir dir("manifest_torn");
    const std::string path = dir.File("db");
    constexpr int kKeys = 4000;
    {
      auto db = OpenDB(path, options);
      for (int k = 0; k < kKeys; ++k) {
        ASSERT_TRUE(db->Put(WriteOptions(), Key(k), Value(k)).ok());
      }
      ASSERT_TRUE(db->CompactAll().ok());
    }

    const std::string manifest = path + "/MANIFEST";
    const int fd = ::open(manifest.c_str(), O_WRONLY | O_APPEND);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(::write(fd, fragments[i].data(), fragments[i].size()),
              static_cast<ssize_t>(fragments[i].size()));
    ::close(fd);

    std::unique_ptr<DB> db;
    const Status s = DB::Open(options, path, &db);
    ASSERT_TRUE(s.ok()) << "fragment " << i << ": " << s.ToString();

    // The fragment never committed anything, so every acknowledged write is
    // still there.
    std::string value;
    for (int k = 0; k < kKeys; k += 11) {
      ASSERT_TRUE(db->Get(ReadOptions(), Key(k), &value).ok())
          << "fragment " << i << " key " << k;
      EXPECT_EQ(value, Value(k)) << "fragment " << i << " key " << k;
    }
  }
}

TEST(Manifest, EmptyDatabaseRoundTrips) {
  ScopedTempDir dir("manifest_empty");
  const std::string path = dir.File("db");
  { auto db = OpenDB(path, ManifestOptions()); }
  {
    auto db = OpenDB(path, ManifestOptions());
    std::string value;
    EXPECT_TRUE(db->Get(ReadOptions(), "anything", &value).IsNotFound());
    ASSERT_TRUE(db->Put(WriteOptions(), "k", "v").ok());
  }
  auto db = OpenDB(path, ManifestOptions());
  std::string value;
  ASSERT_TRUE(db->Get(ReadOptions(), "k", &value).ok());
  EXPECT_EQ(value, "v");
}

TEST(Manifest, SurvivesManyReopens) {
  ScopedTempDir dir("manifest_reopen");
  const std::string path = dir.File("db");
  int written = 0;
  for (int round = 0; round < 12; ++round) {
    auto db = OpenDB(path, ManifestOptions());
    for (int i = 0; i < 400; ++i) {
      ASSERT_TRUE(db->Put(WriteOptions(), Key(written), Value(written)).ok());
      ++written;
    }
    std::string value;
    for (int i = 0; i < written; i += 29) {
      ASSERT_TRUE(db->Get(ReadOptions(), Key(i), &value).ok())
          << "round " << round << " key " << i;
      EXPECT_EQ(value, Value(i));
    }
  }
}

}  // namespace lsm
