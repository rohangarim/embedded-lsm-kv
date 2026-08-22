#pragma once

// Thin adapter so the benchmark driver can run identical workloads against
// this engine and against LevelDB. Keeping the interface this narrow means the
// comparison measures the storage engines rather than the harness around them.

#include <memory>
#include <string>
#include <string_view>

#include "lsm/db.h"

#ifdef LSM_BENCH_LEVELDB
#include <leveldb/cache.h>
#include <leveldb/db.h>
#include <leveldb/filter_policy.h>
#include <leveldb/options.h>
#include <leveldb/write_batch.h>
#endif

namespace bench {

struct EngineConfig {
  std::string path;
  size_t memtable_size_bytes = 4u << 20;
  size_t block_size_bytes = 4096;
  int bloom_bits_per_key = 10;
  size_t block_cache_bytes = 8u << 20;
  bool sync_every_write = false;
  bool verify_checksums = true;
};

class Engine {
 public:
  virtual ~Engine() = default;

  virtual bool Put(std::string_view key, std::string_view value) = 0;
  // Returns true if found. `missing` distinguishes a clean miss from an error.
  virtual bool Get(std::string_view key, std::string* value, bool* error) = 0;
  // Steps forward from `start` and returns how many entries it saw.
  virtual int Scan(std::string_view start, int max_entries) = 0;

  virtual void WaitForBackgroundWork() {}
  virtual std::string CounterReport() const { return ""; }
  virtual const char* name() const = 0;
};

class LsmEngine : public Engine {
 public:
  static std::unique_ptr<LsmEngine> Open(const EngineConfig& config,
                                         lsm::SyncPolicy policy,
                                         std::string* error) {
    lsm::Options options;
    options.memtable_size_bytes = config.memtable_size_bytes;
    options.block_size_bytes = config.block_size_bytes;
    options.bloom_bits_per_key = config.bloom_bits_per_key;
    options.block_cache_bytes = config.block_cache_bytes;
    options.max_bytes_for_level_base = 16u << 20;
    options.target_file_size = 4u << 20;
    options.l0_compaction_trigger = 4;
    options.sync_policy = policy;
    options.verify_checksums = config.verify_checksums;
    options.background_compaction = true;

    std::unique_ptr<lsm::DB> db;
    const lsm::Status s = lsm::DB::Open(options, config.path, &db);
    if (!s.ok()) {
      *error = s.ToString();
      return nullptr;
    }
    auto engine = std::unique_ptr<LsmEngine>(new LsmEngine());
    engine->db_ = std::move(db);
    return engine;
  }

  bool Put(std::string_view key, std::string_view value) override {
    return db_->Put(write_options_, key, value).ok();
  }

  bool Get(std::string_view key, std::string* value, bool* error) override {
    const lsm::Status s = db_->Get(read_options_, key, value);
    if (s.ok()) return true;
    if (!s.IsNotFound()) *error = true;
    return false;
  }

  int Scan(std::string_view start, int max_entries) override {
    auto iter = db_->NewIterator(read_options_);
    int seen = 0;
    for (iter->Seek(start); iter->Valid() && seen < max_entries; iter->Next()) {
      ++seen;
    }
    return seen;
  }

  void WaitForBackgroundWork() override { db_->WaitForBackgroundWork(); }

  std::string CounterReport() const override {
    const lsm::DbStats stats = db_->GetStats();
    char buf[1024];
    std::snprintf(
        buf, sizeof(buf),
        "  writes                 %llu\n"
        "  wal fsyncs             %llu\n"
        "  write groups           %llu (%.2f writes per group)\n"
        "  memtable flushes       %llu\n"
        "  compactions            %llu\n"
        "  compaction in / out    %.1f MiB / %.1f MiB\n"
        "  write amplification    %.2fx\n"
        "  sstable blocks read    %llu\n"
        "  bloom rejections       %llu\n"
        "  tables probed          %llu\n"
        "  block cache hit rate   %.1f%% (%llu hits / %llu misses)\n"
        "  block cache evictions  %llu\n"
        "  block cache resident   %.1f MiB\n"
        "  flush versions dropped %llu\n"
        "  scan rows returned     %llu\n"
        "  scan entries skipped   %llu (%.1f per row returned)\n"
        "\nlevel shape\n%s",
        (unsigned long long)stats.writes, (unsigned long long)stats.wal_syncs,
        (unsigned long long)stats.write_groups,
        stats.write_groups == 0
            ? 0.0
            : static_cast<double>(stats.writes_grouped) /
                  static_cast<double>(stats.write_groups),
        (unsigned long long)stats.memtable_flushes,
        (unsigned long long)stats.compactions,
        stats.compaction_input_bytes / 1048576.0,
        stats.compaction_output_bytes / 1048576.0, stats.WriteAmplification(),
        (unsigned long long)stats.sstable_blocks_read,
        (unsigned long long)stats.bloom_rejections,
        (unsigned long long)stats.tables_probed,
        stats.BlockCacheHitRate() * 100.0,
        (unsigned long long)stats.block_cache_hits,
        (unsigned long long)stats.block_cache_misses,
        (unsigned long long)stats.block_cache_evictions,
        stats.block_cache_bytes_used / 1048576.0,
        (unsigned long long)stats.flush_versions_dropped,
        (unsigned long long)stats.scan_entries_returned,
        (unsigned long long)stats.scan_entries_skipped,
        stats.scan_entries_returned == 0
            ? 0.0
            : static_cast<double>(stats.scan_entries_skipped) /
                  static_cast<double>(stats.scan_entries_returned),
        db_->DebugLevelSummary().c_str());
    return buf;
  }

  const char* name() const override { return "lsmtree"; }

 private:
  LsmEngine() = default;

  std::unique_ptr<lsm::DB> db_;
  lsm::WriteOptions write_options_;
  lsm::ReadOptions read_options_;
};

#ifdef LSM_BENCH_LEVELDB
// Configured to match the settings above as closely as LevelDB allows, so the
// comparison is about implementation rather than about tuning.
class LevelDbEngine : public Engine {
 public:
  static std::unique_ptr<LevelDbEngine> Open(const EngineConfig& config,
                                             std::string* error) {
    leveldb::Options options;
    options.create_if_missing = true;
    options.write_buffer_size = config.memtable_size_bytes;
    options.block_size = config.block_size_bytes;
    options.compression = leveldb::kNoCompression;  // We do not compress either.
    options.filter_policy =
        leveldb::NewBloomFilterPolicy(config.bloom_bits_per_key);
    // Match our block cache budget rather than leaving LevelDB on its 8 MiB
    // default, so neither engine is handed an advantage here.
    options.block_cache = leveldb::NewLRUCache(config.block_cache_bytes);

    leveldb::DB* raw = nullptr;
    const leveldb::Status s = leveldb::DB::Open(options, config.path, &raw);
    if (!s.ok()) {
      *error = s.ToString();
      delete options.filter_policy;
      delete options.block_cache;
      return nullptr;
    }
    auto engine = std::unique_ptr<LevelDbEngine>(new LevelDbEngine());
    engine->db_.reset(raw);
    engine->filter_policy_ = options.filter_policy;
    engine->block_cache_ = options.block_cache;
    engine->write_options_.sync = config.sync_every_write;
    return engine;
  }

  ~LevelDbEngine() override {
    db_.reset();
    delete filter_policy_;
    delete block_cache_;
  }

  bool Put(std::string_view key, std::string_view value) override {
    return db_->Put(write_options_, leveldb::Slice(key.data(), key.size()),
                    leveldb::Slice(value.data(), value.size()))
        .ok();
  }

  bool Get(std::string_view key, std::string* value, bool* error) override {
    const leveldb::Status s =
        db_->Get(read_options_, leveldb::Slice(key.data(), key.size()), value);
    if (s.ok()) return true;
    if (!s.IsNotFound()) *error = true;
    return false;
  }

  int Scan(std::string_view start, int max_entries) override {
    std::unique_ptr<leveldb::Iterator> iter(db_->NewIterator(read_options_));
    int seen = 0;
    for (iter->Seek(leveldb::Slice(start.data(), start.size()));
         iter->Valid() && seen < max_entries; iter->Next()) {
      ++seen;
    }
    return seen;
  }

  std::string CounterReport() const override {
    std::string out;
    db_->GetProperty("leveldb.stats", &out);
    return out;
  }

  const char* name() const override { return "leveldb"; }

 private:
  LevelDbEngine() = default;

  std::unique_ptr<leveldb::DB> db_;
  const leveldb::FilterPolicy* filter_policy_ = nullptr;
  leveldb::Cache* block_cache_ = nullptr;
  leveldb::WriteOptions write_options_;
  leveldb::ReadOptions read_options_;
};
#endif  // LSM_BENCH_LEVELDB

}  // namespace bench
