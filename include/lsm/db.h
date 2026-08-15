#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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
  uint64_t bytes_written_to_wal = 0;
  uint64_t user_bytes_written = 0;
  uint64_t gets = 0;
  uint64_t get_hits_memtable = 0;
  uint64_t sstable_blocks_read = 0;
  uint64_t bloom_rejections = 0;
  uint64_t tables_probed = 0;

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

  Status WriteManifest(const Version& version, uint64_t log_number);  // Requires mu_.
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
  bool shutting_down_ = false;
  Status bg_error_;

  mutable DbStats stats_;
};

}  // namespace lsm
