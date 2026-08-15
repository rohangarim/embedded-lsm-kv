// Child process for the fault-injection harness.
//
// Writes keys 0, 1, 2, ... into a database and publishes the count of
// *acknowledged* writes into a shared memory-mapped counter. The parent kills
// this process at an arbitrary moment; because the counter lives in a
// MAP_SHARED page, the kernel keeps it after SIGKILL and the parent can read
// exactly how far we got.
//
// Two kinds of crash are exercised:
//   - asynchronous SIGKILL from the parent, which can land anywhere, including
//     in the middle of a write() or an fsync();
//   - a deliberately torn WAL record: the write hook truncates the encoded
//     record to a random prefix and we exit immediately afterwards, without
//     acknowledging it. Recovery must drop that fragment and keep everything
//     before it.

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>

#include "lsm/db.h"

namespace {

// Set by the WAL write hook after it truncates a record, so the very next thing
// we do is die -- before the write is acknowledged.
bool g_die_after_next_write = false;

std::string Key(uint64_t i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key%012llu", static_cast<unsigned long long>(i));
  return buf;
}

std::string Value(uint64_t i) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "value%012llu",
                static_cast<unsigned long long>(i));
  std::string out(buf);
  out.resize(100, '#');
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 6) {
    std::fprintf(stderr,
                 "usage: %s <db_path> <progress_file> <seed> <sync_policy> "
                 "<num_writes>\n"
                 "  sync_policy: every | interval | never\n",
                 argv[0]);
    return 2;
  }
  const std::string db_path = argv[1];
  const std::string progress_path = argv[2];
  const uint32_t seed = static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 10));
  const std::string policy = argv[4];
  const uint64_t num_writes = std::strtoull(argv[5], nullptr, 10);

  // Shared counter of acknowledged writes. It is a file-backed MAP_SHARED page
  // so it outlives a SIGKILL of this process.
  const int fd = ::open(progress_path.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0) return 3;
  if (::ftruncate(fd, sizeof(uint64_t)) != 0) return 3;
  void* mapped = ::mmap(nullptr, sizeof(uint64_t), PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd, 0);
  ::close(fd);
  if (mapped == MAP_FAILED) return 3;
  auto* acked = static_cast<std::atomic<uint64_t>*>(mapped);
  acked->store(0, std::memory_order_release);

  lsm::Options options;
  options.memtable_size_bytes = 64 * 1024;
  options.block_size_bytes = 4096;
  options.max_bytes_for_level_base = 512 * 1024;
  options.target_file_size = 256 * 1024;
  options.l0_compaction_trigger = 3;
  options.background_compaction = true;
  if (policy == "every") {
    options.sync_policy = lsm::SyncPolicy::kEveryWrite;
  } else if (policy == "interval") {
    options.sync_policy = lsm::SyncPolicy::kInterval;
    options.fsync_interval_ms = 5;
  } else {
    options.sync_policy = lsm::SyncPolicy::kNever;
  }

  std::unique_ptr<lsm::DB> db;
  const lsm::Status s = lsm::DB::Open(options, db_path, &db);
  if (!s.ok()) {
    std::fprintf(stderr, "worker: open failed: %s\n", s.ToString().c_str());
    return 4;
  }

  std::mt19937 rng(seed);
  // Roughly a third of runs tear a record instead of waiting for the parent's
  // signal, so both failure shapes get covered.
  const bool tear_a_record = (rng() % 3) == 0;
  const uint64_t tear_at = tear_a_record ? (rng() % std::max<uint64_t>(1, num_writes))
                                         : num_writes + 1;

  lsm::WriteOptions write_options;
  for (uint64_t i = 0; i < num_writes; ++i) {
    if (i == tear_at) {
      db->SetWalWriteHook([&rng](std::string* record) {
        // Cut the record somewhere in the middle: sometimes inside the header,
        // sometimes inside the payload.
        const size_t keep = record->empty() ? 0 : rng() % record->size();
        record->resize(keep);
        g_die_after_next_write = true;
      });
    }

    const lsm::Status put = db->Put(write_options, Key(i), Value(i));
    if (g_die_after_next_write) {
      // The record on disk is a fragment. We never acknowledge it, so recovery
      // is free to drop it -- but everything before it must survive.
      ::_exit(9);
    }
    if (!put.ok()) {
      std::fprintf(stderr, "worker: put failed: %s\n", put.ToString().c_str());
      return 5;
    }
    acked->store(i + 1, std::memory_order_release);
  }

  // Ran to completion without being killed. Exit hard anyway so the parent
  // always exercises the recovery path rather than a clean shutdown.
  ::_exit(0);
}
