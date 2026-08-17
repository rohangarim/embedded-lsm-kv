#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "lsm/cache.h"
#include "lsm/internal_key.h"
#include "lsm/iterator.h"
#include "lsm/memtable.h"
#include "lsm/options.h"
#include "lsm/sstable.h"
#include "lsm/status.h"
#include "lsm/wal.h"

namespace lsm {

// L0 holds flushed memtables whose key ranges overlap each other; L1 and below
// are each internally non-overlapping, so a lookup touches at most one file per
// level. Seven levels at 10x growth covers ~10 TB from a 10 MiB L1.
constexpr int kNumLevels = 7;

struct FileMetaData {
  uint64_t number = 0;
  uint64_t file_size = 0;
  std::string smallest;  // Smallest internal key in the file.
  std::string largest;   // Largest internal key in the file.
  std::shared_ptr<Table> table;
};

// An immutable snapshot of which files live at which level. Readers take a
// shared_ptr and then work without holding the lock; a compaction installs a
// wholly new Version rather than mutating this one, so an in-flight read never
// sees a half-applied compaction and never touches a closed file.
struct Version {
  std::vector<std::vector<std::shared_ptr<FileMetaData>>> levels =
      std::vector<std::vector<std::shared_ptr<FileMetaData>>>(kNumLevels);

  uint64_t LevelBytes(int level) const;
  size_t NumFiles() const;
};

// A point in time that reads can be pinned to.
//
// Holding one also pins the versions compaction is allowed to discard: an entry
// may only be dropped once a newer version of the same key is visible to every
// live snapshot. Long-lived snapshots therefore cost space, which is why they
// are handed out explicitly and must be released.
class Snapshot {
 public:
  SequenceNumber sequence() const { return sequence_; }

 private:
  friend class DB;
  explicit Snapshot(SequenceNumber sequence) : sequence_(sequence) {}

  const SequenceNumber sequence_;
};

// Observable counters, for benchmarks and for the read/write amplification
// story. All are monotonic since Open.
struct DbStats {
  uint64_t writes = 0;
  uint64_t wal_syncs = 0;
  uint64_t memtable_flushes = 0;
  uint64_t compactions = 0;
  // Bytes read into, and written out of, compactions only -- flushes are
  // counted separately so the two ratios stay meaningful on their own.
  uint64_t compaction_input_bytes = 0;
  uint64_t compaction_output_bytes = 0;
  uint64_t flush_output_bytes = 0;
  // Shadowed versions collapsed while writing a memtable out to L0.
  uint64_t flush_versions_dropped = 0;
  uint64_t bytes_written_to_wal = 0;
  uint64_t user_bytes_written = 0;
  uint64_t gets = 0;
  uint64_t get_hits_memtable = 0;
  uint64_t sstable_blocks_read = 0;
  uint64_t bloom_rejections = 0;
  uint64_t tables_probed = 0;
  uint64_t block_cache_hits = 0;
  uint64_t block_cache_misses = 0;
  uint64_t block_cache_evictions = 0;
  size_t block_cache_bytes_used = 0;

  // Iterator work. `scan_entries_skipped` counts stored entries a scan had to
  // step over -- shadowed versions, tombstones, and versions newer than the
  // snapshot -- to produce `scan_entries_returned` live rows. The ratio is the
  // read amplification a range scan actually pays.
  uint64_t scan_entries_returned = 0;
  uint64_t scan_entries_skipped = 0;

  // Fraction of data-block reads served from memory rather than the
  // filesystem.
  double BlockCacheHitRate() const;

  // Bytes the engine wrote to disk per byte the user handed it. The cost of
  // turning random writes into sequential ones.
  double WriteAmplification() const;
};

class DB {
 public:
  static Status Open(const Options& options, const std::string& path,
                     std::unique_ptr<DB>* db);

  ~DB();

  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  Status Put(const WriteOptions& opts, std::string_view key,
             std::string_view value);
  Status Delete(const WriteOptions& opts, std::string_view key);
  Status Get(const ReadOptions& opts, std::string_view key, std::string* value);

  // Forward scan over live user keys. Tombstoned and shadowed versions are
  // filtered out.
  std::unique_ptr<Iterator> NewIterator(const ReadOptions& opts);

  // Pins the current state. Reads passing this in ReadOptions see the database
  // as of this call, whatever happens afterwards. Must be released.
  const Snapshot* GetSnapshot();
  void ReleaseSnapshot(const Snapshot* snapshot);

  // Forces the active memtable to disk and waits for it. Test/benchmark hook.
  Status FlushMemTable();

  // Runs compactions until no level exceeds its budget. Test/benchmark hook.
  Status CompactAll();

  // Waits for any in-flight background work to finish.
  void WaitForBackgroundWork();

  DbStats GetStats() const;
  std::string DebugLevelSummary() const;

  // Test hook forwarded to the WAL writer: lets a fault-injection harness
  // truncate a record or kill the process mid-write.
  void SetWalWriteHook(WalWriter::WriteHook hook);

 private:
  DB(const Options& options, std::string path);

  Status Recover();
  Status Write(const WriteOptions& opts, ValueType type, std::string_view key,
               std::string_view value);

  // Called with mu_ held; may release it while rotating the log.
  Status MakeRoomForWrite(std::unique_lock<std::mutex>& lock);

  void MaybeScheduleCompaction();          // Requires mu_.
  void BackgroundLoop();
  bool HasPendingWork() const;             // Requires mu_.
  Status BackgroundWorkOnce(std::unique_lock<std::mutex>& lock);
  Status DrainBackgroundWork(std::unique_lock<std::mutex>& lock);

  Status FlushImmutableMemTable(std::unique_lock<std::mutex>& lock);
  Status DoCompaction(std::unique_lock<std::mutex>& lock, int level);
  // Returns the level that most needs compaction, or -1.
  int PickCompactionLevel(const Version& version) const;  // Requires mu_.

  // Does filesystem work (fsync, rename, directory fsync) and must NOT be
  // called with mu_ held -- every reader takes mu_, and an F_FULLFSYNC is
  // milliseconds. All mutable DB state it needs is passed in explicitly so it
  // touches nothing that another thread can be writing.
  Status WriteManifest(const Version& version, uint64_t log_number,
                       uint64_t next_file_number, SequenceNumber last_sequence);

  // Installs `version` as current, then commits it to the manifest and cleans
  // up retired files with mu_ released. Requires mu_ held on entry; re-acquires
  // before returning.
  Status CommitVersion(std::unique_lock<std::mutex>& lock,
                       std::shared_ptr<const Version> version);
  Status LoadManifest(uint64_t* log_number);

  Status MaybeSyncWal(const WriteOptions& opts);  // Requires mu_.
  void RemoveObsoleteFiles(const Version& live, uint64_t log_number);  // Requires mu_.

  std::string TablePath(uint64_t number) const;
  std::string LogPath(uint64_t number) const;
  std::string ManifestPath() const;

  const Options options_;
  const std::string path_;

  mutable std::mutex mu_;
  std::condition_variable bg_cv_;

  std::shared_ptr<MemTable> mem_;
  std::shared_ptr<MemTable> imm_;  // Being flushed; still visible to readers.
  std::shared_ptr<const Version> current_;

  // Shared by every open table. Held by shared_ptr because a Table outlives the
  // DB whenever a reader is still holding the version that references it.
  std::shared_ptr<BlockCache> block_cache_;

  // Sequence below which no live reader can see anything, so older versions of
  // a key are safe to discard. Equals the newest sequence when nothing is
  // pinned, which is what makes the collapse total in the common case.
  SequenceNumber SmallestSnapshot() const;  // Requires mu_.

  std::map<const Snapshot*, std::unique_ptr<Snapshot>> snapshots_;

  // Where the next compaction of each level should start, so successive
  // compactions sweep across the key space instead of rewriting the leftmost
  // file over and over.
  std::string compact_pointer_[kNumLevels];

  // The oldest log recovery still needs: the log the immutable memtable was
  // written to while its flush is outstanding, otherwise the active log. This
  // is what goes into the manifest, and it is the cutoff for deleting logs.
  uint64_t OldestLiveLog() const;  // Requires mu_.

  std::unique_ptr<WalWriter> log_;
  uint64_t log_number_ = 0;
  // Log that imm_'s records live in; 0 when no flush is pending.
  uint64_t imm_log_number_ = 0;
  uint64_t next_file_number_ = 1;
  SequenceNumber last_sequence_ = 0;

  std::chrono::steady_clock::time_point last_sync_time_;
  bool wal_dirty_ = false;

  std::thread bg_thread_;
  bool bg_running_ = false;   // A background pass is in flight.
  // A version commit is in flight with mu_ released. Serializes commits so two
  // of them cannot write the manifest out of order.
  bool committing_ = false;
  bool shutting_down_ = false;
  Status bg_error_;

  mutable DbStats stats_;

  // Hot-path counters, kept out of stats_ deliberately. A Get used to take mu_
  // three times -- once to snapshot the memtables and version, and twice more
  // purely to count -- on the same mutex a background compaction holds while it
  // commits a manifest. Counting is not worth contending for.
  mutable std::atomic<uint64_t> stat_gets_{0};
  mutable std::atomic<uint64_t> stat_get_hits_memtable_{0};
  mutable std::atomic<uint64_t> stat_tables_probed_{0};
  mutable std::atomic<uint64_t> stat_scan_returned_{0};
  mutable std::atomic<uint64_t> stat_scan_skipped_{0};
};

}  // namespace lsm
