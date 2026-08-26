#pragma once

#include <cstddef>

#include "lsm/compression.h"
#include <cstdint>

namespace lsm {

class Snapshot;

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

  // How many entries share a prefix-compression run inside a data block. Every
  // Nth key is stored whole and becomes a binary-search landing point; the ones
  // between store only the bytes that differ from their predecessor. Smaller is
  // faster to search and larger compresses better -- 16 is the usual balance.
  int block_restart_interval = 16;

  // Compression for data blocks. A block that does not shrink by at least an
  // eighth is stored raw regardless, so incompressible data costs only the
  // attempt. Index and Bloom blocks are never compressed: the filter is already
  // dense bits, and the index is read once and held.
  CompressionType compression = CompressionType::kNoCompression;

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

  // Collapse the manifest's accumulated edits back into a single snapshot once
  // it grows past this. Larger means fewer rewrites but a longer replay at
  // open; the cost of a rewrite is proportional to the number of live files.
  uint64_t manifest_rewrite_bytes = 1u << 20;  // 1 MiB

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

  // Read as of this snapshot rather than the latest state. The snapshot must
  // still be live -- released snapshots dangle, exactly like a freed pointer.
  // Null reads the newest data.
  const Snapshot* snapshot = nullptr;
};

}  // namespace lsm
