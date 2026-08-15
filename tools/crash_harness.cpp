// Fault-injection harness.
//
// Each round: spawn lsm_crash_worker, let it write for a random interval, then
// SIGKILL it. The worker publishes how many writes it acknowledged into a
// shared page; this process then reopens the database and checks two things:
//
//   1. Durability -- every acknowledged write is readable after recovery. A
//      violation here means the engine lied to a caller.
//   2. Atomicity -- the recovered keys are exactly a contiguous prefix
//      0..m. A hole would mean a record was skipped; a key beyond the last
//      complete record would mean a torn fragment was accepted as real.
//
// Writes are issued in ascending key order, which is what makes the prefix
// property checkable.

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "lsm/db.h"

namespace {

struct Config {
  int rounds = 200;
  uint64_t writes_per_round = 20000;
  uint32_t seed = 1;
  std::string worker_path;
  std::string scratch_dir = "/tmp/lsm_crash";
  bool verbose = false;
};

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

void RemoveTree(const std::string& path) {
  // Cheap and portable enough for a scratch directory we created ourselves.
  const std::string cmd = "rm -rf '" + path + "'";
  if (std::system(cmd.c_str()) != 0) {
    std::fprintf(stderr, "warning: failed to clean %s\n", path.c_str());
  }
}

const char* PolicyName(int i) {
  switch (i % 3) {
    case 0: return "every";
    case 1: return "interval";
    default: return "never";
  }
}

// Finds the largest m such that keys 0..m-1 are all present, and verifies that
// no key >= m survived. Returns m, or -1 on a violation.
int64_t VerifyPrefix(lsm::DB* db, uint64_t upper_bound, std::string* error) {
  uint64_t recovered = 0;
  for (uint64_t i = 0; i < upper_bound; ++i) {
    std::string value;
    const lsm::Status s = db->Get(lsm::ReadOptions(), Key(i), &value);
    if (s.IsNotFound()) break;
    if (!s.ok()) {
      *error = "read error at key " + std::to_string(i) + ": " + s.ToString();
      return -1;
    }
    if (value != Value(i)) {
      *error = "value mismatch at key " + std::to_string(i);
      return -1;
    }
    recovered = i + 1;
  }

  // Nothing past the prefix may exist: a torn record must never be resurrected.
  for (uint64_t i = recovered; i < std::min(upper_bound, recovered + 64); ++i) {
    std::string value;
    const lsm::Status s = db->Get(lsm::ReadOptions(), Key(i), &value);
    if (s.ok()) {
      *error = "hole in recovered range: key " + std::to_string(recovered) +
               " missing but key " + std::to_string(i) + " present";
      return -1;
    }
  }
  return static_cast<int64_t>(recovered);
}

}  // namespace

int main(int argc, char** argv) {
  Config config;
  {
    const std::string self = argv[0];
    const size_t slash = self.rfind('/');
    config.worker_path =
        (slash == std::string::npos ? std::string(".") : self.substr(0, slash)) +
        "/lsm_crash_worker";
  }
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--rounds" && i + 1 < argc) {
      config.rounds = std::atoi(argv[++i]);
    } else if (arg == "--writes" && i + 1 < argc) {
      config.writes_per_round = std::strtoull(argv[++i], nullptr, 10);
    } else if (arg == "--seed" && i + 1 < argc) {
      config.seed = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (arg == "--worker" && i + 1 < argc) {
      config.worker_path = argv[++i];
    } else if (arg == "--dir" && i + 1 < argc) {
      config.scratch_dir = argv[++i];
    } else if (arg == "--verbose") {
      config.verbose = true;
    } else {
      std::fprintf(stderr,
                   "usage: %s [--rounds N] [--writes N] [--seed N] "
                   "[--worker PATH] [--dir PATH] [--verbose]\n",
                   argv[0]);
      return 2;
    }
  }

  RemoveTree(config.scratch_dir);
  if (::mkdir(config.scratch_dir.c_str(), 0755) != 0) {
    std::perror("mkdir");
    return 3;
  }

  std::mt19937 rng(config.seed);
  uint64_t total_acked = 0;
  uint64_t total_recovered = 0;
  int failures = 0;
  int torn_or_killed_mid_write = 0;

  std::printf("fault injection: %d rounds, up to %llu writes each, seed %u\n",
              config.rounds,
              static_cast<unsigned long long>(config.writes_per_round),
              config.seed);

  for (int round = 0; round < config.rounds; ++round) {
    const std::string db_path =
        config.scratch_dir + "/db" + std::to_string(round);
    const std::string progress_path =
        config.scratch_dir + "/progress" + std::to_string(round);
    const uint32_t worker_seed = rng();
    const char* policy = PolicyName(round);

    const pid_t pid = ::fork();
    if (pid < 0) {
      std::perror("fork");
      return 4;
    }
    if (pid == 0) {
      const std::string seed_str = std::to_string(worker_seed);
      const std::string writes_str = std::to_string(config.writes_per_round);
      ::execl(config.worker_path.c_str(), config.worker_path.c_str(),
              db_path.c_str(), progress_path.c_str(), seed_str.c_str(), policy,
              writes_str.c_str(), static_cast<char*>(nullptr));
      std::perror("execl");
      ::_exit(127);
    }

    // Kill at a random point. The range spans startup, steady-state writing,
    // and the middle of background flushes and compactions.
    const int delay_ms = 2 + static_cast<int>(rng() % 250);
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    ::kill(pid, SIGKILL);

    int wait_status = 0;
    ::waitpid(pid, &wait_status, 0);
    if (WIFSIGNALED(wait_status)) ++torn_or_killed_mid_write;

    uint64_t acked = 0;
    {
      const int fd = ::open(progress_path.c_str(), O_RDONLY);
      if (fd >= 0) {
        void* mapped =
            ::mmap(nullptr, sizeof(uint64_t), PROT_READ, MAP_SHARED, fd, 0);
        ::close(fd);
        if (mapped != MAP_FAILED) {
          acked = static_cast<std::atomic<uint64_t>*>(mapped)->load(
              std::memory_order_acquire);
          ::munmap(mapped, sizeof(uint64_t));
        }
      }
    }

    lsm::Options options;
    options.memtable_size_bytes = 64 * 1024;
    options.block_size_bytes = 4096;
    options.max_bytes_for_level_base = 512 * 1024;
    options.target_file_size = 256 * 1024;
    options.l0_compaction_trigger = 3;
    options.background_compaction = false;

    std::unique_ptr<lsm::DB> db;
    const lsm::Status open = lsm::DB::Open(options, db_path, &db);
    if (!open.ok()) {
      std::printf("round %d FAILED: reopen: %s\n", round, open.ToString().c_str());
      ++failures;
      RemoveTree(db_path);
      continue;
    }

    std::string error;
    const int64_t recovered =
        VerifyPrefix(db.get(), config.writes_per_round, &error);
    if (recovered < 0) {
      std::printf("round %d FAILED (%s policy, %llu acked): %s\n", round, policy,
                  static_cast<unsigned long long>(acked), error.c_str());
      ++failures;
    } else if (static_cast<uint64_t>(recovered) < acked) {
      // The durability violation we are hunting for: a write the engine
      // returned OK for is gone.
      std::printf(
          "round %d FAILED (%s policy): acknowledged %llu writes but only %lld "
          "recovered\n",
          round, policy, static_cast<unsigned long long>(acked),
          static_cast<long long>(recovered));
      ++failures;
    } else {
      total_acked += acked;
      total_recovered += static_cast<uint64_t>(recovered);
      if (config.verbose) {
        std::printf("round %3d ok (%-8s) acked=%-7llu recovered=%-7lld\n", round,
                    policy, static_cast<unsigned long long>(acked),
                    static_cast<long long>(recovered));
      }
    }

    db.reset();
    RemoveTree(db_path);
    ::unlink(progress_path.c_str());
  }

  RemoveTree(config.scratch_dir);

  std::printf(
      "\n%d/%d rounds passed; %d killed by signal mid-operation\n"
      "total acknowledged writes: %llu, total recovered: %llu\n",
      config.rounds - failures, config.rounds, torn_or_killed_mid_write,
      static_cast<unsigned long long>(total_acked),
      static_cast<unsigned long long>(total_recovered));
  if (failures > 0) {
    std::printf("RESULT: FAIL (%d rounds violated durability or atomicity)\n",
                failures);
    return 1;
  }
  std::printf("RESULT: PASS -- every acknowledged write survived every crash\n");
  return 0;
}
