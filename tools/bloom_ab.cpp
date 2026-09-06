// Bloom filter A/B measurement.
//
// Added for the resume audit. Measures two things the engine already counts but
// no existing binary reports:
//
//   1. Point-lookup read amplification, defined as SSTable data blocks actually
//      read from disk per Get(). DbStats::sstable_blocks_read is incremented
//      only past the block-cache hit path, so this counts cache misses only --
//      which is the definition that makes the Bloom filter's contribution
//      visible.
//
//   2. The observed false-positive rate at a given bits/key, measured over
//      lookups of keys that are genuinely absent. For an absent key, every
//      table probe that the filter does NOT reject is by definition a false
//      positive, so:
//
//          fp_rate = (tables_probed - bloom_rejections) / tables_probed
//
// The build phase is identical in both arms; only Options::bloom_bits_per_key
// differs. Pass --bloom-bits 0 to write empty filters (see src/sstable.cpp).
//
// This tool does not modify engine behaviour. It only reads counters.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <memory>
#include <random>
#include <string>

#include "lsm/db.h"
#include "lsm/options.h"

namespace {

// Only EVEN indices are stored; only ODD indices are queried as absent.
//
// This matters. An earlier version of this tool used a distinct key prefix for
// absent keys, which sorted outside every SSTable's key range -- so the
// per-file range check discarded them before any Bloom filter was consulted,
// and the measured "false positive rate" was really measuring the range check.
// Interleaving keeps absent keys inside the stored range, so the filter is
// actually exercised.
std::string PresentKey(uint64_t i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "user%016llu",
                static_cast<unsigned long long>(2 * i));
  return buf;
}

std::string AbsentKey(uint64_t i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "user%016llu",
                static_cast<unsigned long long>(2 * i + 1));
  return buf;
}

}  // namespace

int main(int argc, char** argv) {
  std::string db_path = "/tmp/lsm_bloom_ab";
  uint64_t num_keys = 500000;
  uint64_t num_hit_lookups = 200000;
  uint64_t num_miss_lookups = 1000000;
  int bloom_bits = 10;
  size_t value_size = 100;
  size_t block_cache_mb = 8;
  std::string sync_policy = "never";

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&]() { return std::string(argv[++i]); };
    if (arg == "--db") db_path = next();
    else if (arg == "--keys") num_keys = std::strtoull(next().c_str(), nullptr, 10);
    else if (arg == "--hits") num_hit_lookups = std::strtoull(next().c_str(), nullptr, 10);
    else if (arg == "--misses") num_miss_lookups = std::strtoull(next().c_str(), nullptr, 10);
    else if (arg == "--bloom-bits") bloom_bits = std::atoi(next().c_str());
    else if (arg == "--value-size") value_size = std::strtoul(next().c_str(), nullptr, 10);
    else if (arg == "--block-cache-mb") block_cache_mb = std::strtoul(next().c_str(), nullptr, 10);
    else if (arg == "--sync") sync_policy = next();
    else {
      std::fprintf(stderr,
                   "usage: %s [--db PATH] [--keys N] [--hits N] [--misses N]\n"
                   "          [--bloom-bits N] [--value-size N] [--block-cache-mb N]\n",
                   argv[0]);
      return 2;
    }
  }

  std::string rm = "rm -rf " + db_path;
  if (std::system(rm.c_str()) != 0) {
    std::fprintf(stderr, "warning: could not clear %s\n", db_path.c_str());
  }

  lsm::Options options;
  options.bloom_bits_per_key = bloom_bits;
  options.block_cache_bytes = block_cache_mb << 20;
  // The engine defaults to fsync-per-write, which on macOS is F_FULLFSYNC and
  // would make the load phase take hours. Sync policy has no bearing on the
  // read-path metrics this tool measures; it is set explicitly so the run is
  // tractable and the condition is recorded rather than implicit.
  if (sync_policy == "every") options.sync_policy = lsm::SyncPolicy::kEveryWrite;
  else if (sync_policy == "interval") options.sync_policy = lsm::SyncPolicy::kInterval;
  else options.sync_policy = lsm::SyncPolicy::kNever;

  std::unique_ptr<lsm::DB> db;
  lsm::Status s = lsm::DB::Open(options, db_path, &db);
  if (!s.ok()) {
    std::fprintf(stderr, "open failed: %s\n", s.ToString().c_str());
    return 1;
  }

  std::printf("bloom A/B\n");
  std::printf("  db=%s keys=%llu value_size=%zu bloom_bits_per_key=%d\n", db_path.c_str(),
              static_cast<unsigned long long>(num_keys), value_size, bloom_bits);
  std::printf("  sync_policy=%s (load phase only; irrelevant to read metrics)\n",
              sync_policy.c_str());
  std::printf("  block_cache=%zu MiB hits=%llu misses=%llu\n", block_cache_mb,
              static_cast<unsigned long long>(num_hit_lookups),
              static_cast<unsigned long long>(num_miss_lookups));

  // ---- load -------------------------------------------------------------
  std::string value(value_size, 'x');
  lsm::WriteOptions wo;
  for (uint64_t i = 0; i < num_keys; ++i) {
    s = db->Put(wo, PresentKey(i), value);
    if (!s.ok()) {
      std::fprintf(stderr, "put failed: %s\n", s.ToString().c_str());
      return 1;
    }
  }
  s = db->FlushMemTable();
  if (!s.ok()) { std::fprintf(stderr, "flush failed\n"); return 1; }
  db->WaitForBackgroundWork();

  std::printf("\nlevel shape after load:\n%s\n", db->DebugLevelSummary().c_str());

  auto report = [&](const char* phase, const lsm::DbStats& before,
                    const lsm::DbStats& after, uint64_t lookups, double seconds,
                    bool absent_phase) {
    const uint64_t gets = after.gets - before.gets;
    const uint64_t blocks = after.sstable_blocks_read - before.sstable_blocks_read;
    const uint64_t probes = after.tables_probed - before.tables_probed;
    const uint64_t rejects = after.bloom_rejections - before.bloom_rejections;
    const uint64_t ch = after.block_cache_hits - before.block_cache_hits;
    const uint64_t cm = after.block_cache_misses - before.block_cache_misses;

    std::printf("\n[%s]\n", phase);
    std::printf("  lookups issued        %llu\n", static_cast<unsigned long long>(lookups));
    std::printf("  gets counted          %llu\n", static_cast<unsigned long long>(gets));
    std::printf("  sstable blocks read   %llu\n", static_cast<unsigned long long>(blocks));
    std::printf("  READ AMP (blocks/get) %.6f\n",
                gets ? static_cast<double>(blocks) / static_cast<double>(gets) : 0.0);
    std::printf("  tables probed         %llu\n", static_cast<unsigned long long>(probes));
    std::printf("  bloom rejections      %llu\n", static_cast<unsigned long long>(rejects));
    if (probes > 0) {
      std::printf("  bloom pass-through    %llu\n",
                  static_cast<unsigned long long>(probes - rejects));
      // Only meaningful when the key is known absent: then every probe the
      // filter fails to reject is by definition a false positive. For present
      // keys a pass-through is a true positive and this ratio means nothing.
      if (absent_phase) {
        std::printf("  FP RATE               %.6f%%\n",
                    100.0 * static_cast<double>(probes - rejects) / static_cast<double>(probes));
      }
    }
    std::printf("  block cache hits/miss %llu / %llu\n", static_cast<unsigned long long>(ch),
                static_cast<unsigned long long>(cm));
    std::printf("  elapsed               %.3f s  (%.0f lookups/sec)\n", seconds,
                seconds > 0 ? static_cast<double>(lookups) / seconds : 0.0);
  };

  lsm::ReadOptions ro;
  std::string out;

  // ---- phase 1: present keys (100% hit) ---------------------------------
  {
    const lsm::DbStats before = db->GetStats();
    std::mt19937_64 rng(12345);
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t found = 0;
    for (uint64_t i = 0; i < num_hit_lookups; ++i) {
      const uint64_t k = rng() % num_keys;
      if (db->Get(ro, PresentKey(k), &out).ok()) ++found;
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const lsm::DbStats after = db->GetStats();
    report("phase 1: 100% present keys (hits)", before, after, num_hit_lookups, secs,
           /*absent_phase=*/false);
    std::printf("  keys found            %llu / %llu\n", static_cast<unsigned long long>(found),
                static_cast<unsigned long long>(num_hit_lookups));
  }

  // ---- phase 2: absent keys (100% miss) ---------------------------------
  {
    const lsm::DbStats before = db->GetStats();
    std::mt19937_64 rng(67890);
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t found = 0;
    for (uint64_t i = 0; i < num_miss_lookups; ++i) {
      const uint64_t k = rng() % num_keys;
      if (db->Get(ro, AbsentKey(k), &out).ok()) ++found;
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const lsm::DbStats after = db->GetStats();
    report("phase 2: 100% absent keys (misses)", before, after, num_miss_lookups, secs,
           /*absent_phase=*/true);
    std::printf("  keys found            %llu  (MUST be 0)\n",
                static_cast<unsigned long long>(found));
  }

  // ---- phase 3: stated 50/50 mix ----------------------------------------
  //
  // The reduction a Bloom filter delivers is entirely a function of how many
  // lookups miss. At 100% hits it saves nothing (every probe is a true
  // positive); at 100% misses it saves nearly everything. Any single headline
  // number is meaningless without the ratio attached, so this phase pins one.
  {
    const lsm::DbStats before = db->GetStats();
    std::mt19937_64 rng(24680);
    const uint64_t mixed = num_hit_lookups + num_miss_lookups;
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t hits = 0;
    for (uint64_t i = 0; i < mixed; ++i) {
      const uint64_t k = rng() % num_keys;
      if ((i & 1) == 0) {
        if (db->Get(ro, PresentKey(k), &out).ok()) ++hits;
      } else {
        (void)db->Get(ro, AbsentKey(k), &out);
      }
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const lsm::DbStats after = db->GetStats();
    report("phase 3: 50% present / 50% absent", before, after, mixed, secs,
           /*absent_phase=*/false);
    std::printf("  present-key hits      %llu / %llu\n", static_cast<unsigned long long>(hits),
                static_cast<unsigned long long>(mixed / 2));
  }

  return 0;
}
