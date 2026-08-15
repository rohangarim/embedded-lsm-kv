#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "lsm/db.h"
#include "test_util.h"

namespace lsm {
namespace {

using testing_support::Key;
using testing_support::ScopedTempDir;
using testing_support::Value;

Options ConcurrentOptions() {
  Options options;
  options.memtable_size_bytes = 64 * 1024;
  options.block_size_bytes = 1024;
  options.max_bytes_for_level_base = 256 * 1024;
  options.target_file_size = 128 * 1024;
  options.l0_compaction_trigger = 3;
  options.sync_policy = SyncPolicy::kNever;
  options.background_compaction = true;  // The point of these tests.
  return options;
}

std::unique_ptr<DB> OpenDB(const std::string& path) {
  std::unique_ptr<DB> db;
  const Status s = DB::Open(ConcurrentOptions(), path, &db);
  EXPECT_TRUE(s.ok()) << s.ToString();
  return db;
}

}  // namespace

// Readers running against a moving target: flushes and compactions swap the
// file set underneath them the whole time. A reader must never see a key
// disappear that was already acknowledged. Run under TSan.
TEST(Concurrency, ReadsAreConsistentWhileCompactionRuns) {
  ScopedTempDir dir("conc_read");
  auto db = OpenDB(dir.File("db"));

  constexpr int kKeys = 30000;
  std::atomic<int> watermark{0};
  std::atomic<bool> done{false};
  std::atomic<int> failures{0};

  std::thread writer([&] {
    for (int i = 0; i < kKeys; ++i) {
      const Status s = db->Put(WriteOptions(), Key(i), Value(i));
      if (!s.ok()) failures.fetch_add(1);
      watermark.store(i + 1, std::memory_order_release);
    }
    done.store(true, std::memory_order_release);
  });

  std::vector<std::thread> readers;
  for (int r = 0; r < 4; ++r) {
    readers.emplace_back([&, r] {
      std::mt19937 rng(1000 + r);
      while (!done.load(std::memory_order_acquire)) {
        const int limit = watermark.load(std::memory_order_acquire);
        if (limit == 0) continue;
        for (int probe = 0; probe < 64; ++probe) {
          const int i = static_cast<int>(rng() % static_cast<uint32_t>(limit));
          std::string value;
          const Status s = db->Get(ReadOptions(), Key(i), &value);
          // Key i was acknowledged before the watermark was published, so it
          // must be visible now.
          if (!s.ok() || value != Value(i)) failures.fetch_add(1);
        }
      }
    });
  }

  writer.join();
  for (auto& t : readers) t.join();
  EXPECT_EQ(failures.load(), 0);

  db->WaitForBackgroundWork();
  for (int i = 0; i < kKeys; i += 13) {
    std::string value;
    ASSERT_TRUE(db->Get(ReadOptions(), Key(i), &value).ok()) << i;
    EXPECT_EQ(value, Value(i));
  }
}

// Writes are serialized internally; this checks that concurrent callers do not
// corrupt the log or the memtable, and that every write lands exactly once.
TEST(Concurrency, ConcurrentWritersFromMultipleThreads) {
  ScopedTempDir dir("conc_write");
  auto db = OpenDB(dir.File("db"));

  constexpr int kThreads = 4;
  constexpr int kPerThread = 5000;
  std::vector<std::thread> writers;
  std::atomic<int> failures{0};

  for (int t = 0; t < kThreads; ++t) {
    writers.emplace_back([&, t] {
      for (int i = 0; i < kPerThread; ++i) {
        const int id = t * kPerThread + i;
        if (!db->Put(WriteOptions(), Key(id), Value(id)).ok()) {
          failures.fetch_add(1);
        }
      }
    });
  }
  for (auto& w : writers) w.join();
  EXPECT_EQ(failures.load(), 0);

  db->WaitForBackgroundWork();
  for (int id = 0; id < kThreads * kPerThread; ++id) {
    std::string value;
    ASSERT_TRUE(db->Get(ReadOptions(), Key(id), &value).ok()) << id;
    ASSERT_EQ(value, Value(id)) << id;
  }
  EXPECT_EQ(db->GetStats().writes,
            static_cast<uint64_t>(kThreads) * kPerThread);
}

TEST(Concurrency, IteratorSurvivesCompactionOfTheFilesItIsReading) {
  ScopedTempDir dir("conc_iter");
  auto db = OpenDB(dir.File("db"));

  constexpr int kKeys = 20000;
  for (int i = 0; i < kKeys; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Value(i)).ok());
  }
  db->WaitForBackgroundWork();

  // The iterator pins the version it opened on. Rewriting every key underneath
  // it forces compactions that unlink the very files it is walking.
  auto iter = db->NewIterator(ReadOptions());
  iter->SeekToFirst();

  std::thread churn([&] {
    for (int round = 0; round < 3; ++round) {
      for (int i = 0; i < kKeys; ++i) {
        db->Put(WriteOptions(), Key(i), Value(i + 1000000));
      }
    }
  });

  int count = 0;
  for (; iter->Valid(); iter->Next()) {
    // Snapshot semantics: the values written after the iterator opened must not
    // appear in it.
    EXPECT_EQ(iter->key(), Key(count));
    EXPECT_EQ(iter->value(), Value(count));
    ++count;
  }
  churn.join();

  EXPECT_EQ(count, kKeys);
  EXPECT_TRUE(iter->status().ok());
}

TEST(Concurrency, MixedReadWriteDeleteWorkload) {
  ScopedTempDir dir("conc_mixed");
  auto db = OpenDB(dir.File("db"));

  constexpr int kKeys = 5000;
  for (int i = 0; i < kKeys; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Value(i)).ok());
  }

  std::atomic<bool> stop{false};
  std::atomic<int> errors{0};
  std::vector<std::thread> threads;

  // Owns the even keys: deletes and reinserts them forever.
  threads.emplace_back([&] {
    std::mt19937 rng(1);
    while (!stop.load()) {
      const int i = static_cast<int>(rng() % (kKeys / 2)) * 2;
      if (!db->Delete(WriteOptions(), Key(i)).ok()) errors.fetch_add(1);
      if (!db->Put(WriteOptions(), Key(i), Value(i)).ok()) errors.fetch_add(1);
    }
  });

  // Reads the odd keys, which nobody mutates -- they must always be present.
  for (int r = 0; r < 3; ++r) {
    threads.emplace_back([&, r] {
      std::mt19937 rng(100 + r);
      for (int n = 0; n < 20000; ++n) {
        const int i = static_cast<int>(rng() % (kKeys / 2)) * 2 + 1;
        std::string value;
        const Status s = db->Get(ReadOptions(), Key(i), &value);
        if (!s.ok() || value != Value(i)) errors.fetch_add(1);
      }
    });
  }

  for (size_t i = 1; i < threads.size(); ++i) threads[i].join();
  stop.store(true);
  threads[0].join();

  EXPECT_EQ(errors.load(), 0);
}

}  // namespace lsm
