#pragma once

#include <cstddef>
#include <cstdint>

namespace lsm {

// When the WAL is forced to stable storage. This is the durability vs.
// throughput knob: kEveryWrite survives a machine power loss, kNever survives
// only a process crash (the page cache outlives the process), and kInterval
// bounds the window of acknowledged-but-lost writes to roughly fsync_interval.
enum class SyncPolicy {
  kEveryWrite,
  kInterval,
  kNever,
};

struct Options {
  // Create the database directory if it is missing.
  bool create_if_missing = true;

  // Refuse to open an existing database.
  bool error_if_exists = false;

  // Flush the memtable once its approximate memory footprint exceeds this.
  size_t memtable_size_bytes = 4u << 20;  // 4 MiB

  // Target size of an uncompressed data block inside an SSTable. Blocks exist
  // so a point lookup reads one page-sized chunk instead of the whole file.
  size_t block_size_bytes = 4096;

  // Bits of Bloom filter per key. 10 bits/key gives ~1% false positives.
  int bloom_bits_per_key = 10;

  // Capacity of the shared cache of decoded data blocks. Zero disables it, in
  // which case every block a read touches is pread and re-parsed from scratch.
  size_t block_cache_bytes = 8u << 20;  // 8 MiB

  // L0 holds files with overlapping key ranges, so a point lookup may touch
  // every one of them. Compact once L0 reaches this many files.
  int l0_compaction_trigger = 4;

  // Byte budget of level 1. Each deeper level is 10x larger.
  uint64_t max_bytes_for_level_base = 10u << 20;  // 10 MiB

  // Target size of an individual SSTable produced by compaction.
  uint64_t target_file_size = 2u << 20;  // 2 MiB

  SyncPolicy sync_policy = SyncPolicy::kEveryWrite;

  // Only consulted when sync_policy == kInterval.
  uint64_t fsync_interval_ms = 1000;

  // Run flushes and compactions on a background thread. Disable to get fully
  // deterministic behaviour in tests.
  bool background_compaction = true;

  // Paranoid CRC verification of every block read from an SSTable.
  bool verify_checksums = true;
};

struct WriteOptions {
  // Override the database-wide sync policy for this write. When true the WAL
  // record is fsync'd before Put/Delete returns.
  bool sync = false;

  // Use the database-wide policy rather than the `sync` field above.
  bool use_default_policy = true;
};

struct ReadOptions {
  bool verify_checksums = true;
  bool fill_cache = true;
};

}  // namespace lsm
