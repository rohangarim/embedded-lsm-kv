// YCSB-style benchmark.
//
// Workloads follow the YCSB core set:
//   load    100% insert, sequential keys      (bulk load)
//   A       50% read  / 50% update            (update heavy)
//   B       95% read  /  5% update            (read mostly)
//   C       100% read                          (read only)
//   D       95% read (latest) / 5% insert      (read latest)
//   E       95% scan  /  5% insert             (short ranges)
//   F       50% read / 50% read-modify-write   (read-modify-write)
//
// Reports throughput and the p50/p95/p99/p99.9 latency of each operation, plus
// the engine's own amplification counters. Latencies come from a full sorted
// sample rather than a histogram: at these operation counts the memory is
// trivial and the tail numbers are then exact rather than bucketed.

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "engine.h"
#include "lsm/db.h"

namespace {

struct Config {
  std::string db_path = "/tmp/lsm_bench_db";
  uint64_t num_keys = 500000;
  uint64_t num_ops = 500000;
  size_t value_size = 100;
  int threads = 1;
  int scan_length = 50;
  std::string sync_policy = "never";
  std::string workloads = "load,A,B,C,D,E,F";
  std::string engine = "lsmtree";
  size_t block_cache_bytes = 8u << 20;
  bool keep_db = false;
  double zipf_theta = 0.99;
};

std::string MakeKey(uint64_t i) {
  // Zero-padded and fixed width: keeps lexicographic order equal to numeric
  // order, which the scan workload depends on.
  char buf[32];
  std::snprintf(buf, sizeof(buf), "user%016llu",
                static_cast<unsigned long long>(i));
  return buf;
}

std::string MakeValue(uint64_t i, size_t size) {
  std::string out = "v" + std::to_string(i) + ":";
  out.resize(size, 'x');
  return out;
}

// Zipfian generator (Gray et al., as used by YCSB): most requests concentrate
// on a small set of hot keys, which is what makes block and OS page caching
// matter. A uniform distribution would flatter the engine.
class ZipfGenerator {
 public:
  ZipfGenerator(uint64_t n, double theta, uint32_t seed)
      : n_(n), theta_(theta), rng_(seed) {
    zeta_n_ = Zeta(n, theta);
    alpha_ = 1.0 / (1.0 - theta);
    eta_ = (1.0 - std::pow(2.0 / static_cast<double>(n), 1.0 - theta)) /
           (1.0 - Zeta(2, theta) / zeta_n_);
  }

  uint64_t Next() {
    const double u = Uniform();
    const double uz = u * zeta_n_;
    if (uz < 1.0) return 0;
    if (uz < 1.0 + std::pow(0.5, theta_)) return 1;
    const double value =
        static_cast<double>(n_) * std::pow(eta_ * u - eta_ + 1.0, alpha_);
    const uint64_t index = static_cast<uint64_t>(value);
    return index >= n_ ? n_ - 1 : index;
  }

 private:
  double Uniform() {
    return static_cast<double>(rng_()) / static_cast<double>(rng_.max());
  }
  static double Zeta(uint64_t n, double theta) {
    double sum = 0.0;
    for (uint64_t i = 1; i <= n; ++i) {
      sum += 1.0 / std::pow(static_cast<double>(i), theta);
    }
    return sum;
  }

  uint64_t n_;
  double theta_;
  double zeta_n_ = 0;
  double alpha_ = 0;
  double eta_ = 0;
  std::mt19937_64 rng_;
};

class LatencyRecorder {
 public:
  void Reserve(size_t n) { samples_.reserve(n); }
  void Add(uint64_t nanos) { samples_.push_back(nanos); }
  void Merge(const LatencyRecorder& other) {
    samples_.insert(samples_.end(), other.samples_.begin(), other.samples_.end());
  }
  size_t count() const { return samples_.size(); }

  // Sorts in place; call once, after all samples are in.
  void Finalize() { std::sort(samples_.begin(), samples_.end()); }

  double PercentileMicros(double p) const {
    if (samples_.empty()) return 0.0;
    size_t index = static_cast<size_t>(p * static_cast<double>(samples_.size()));
    if (index >= samples_.size()) index = samples_.size() - 1;
    return static_cast<double>(samples_[index]) / 1000.0;
  }

 private:
  std::vector<uint64_t> samples_;
};

class Timer {
 public:
  Timer() : start_(std::chrono::steady_clock::now()) {}
  uint64_t ElapsedNanos() const {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start_)
            .count());
  }
  double ElapsedSeconds() const {
    return static_cast<double>(ElapsedNanos()) / 1e9;
  }

 private:
  std::chrono::steady_clock::time_point start_;
};

struct Result {
  std::string name;
  uint64_t operations = 0;
  double seconds = 0;
  LatencyRecorder latency;
};

void PrintHeader() {
  std::printf("\n%-10s %12s %12s %10s %10s %10s %10s\n", "workload", "ops",
              "ops/sec", "p50 (us)", "p95 (us)", "p99 (us)", "p99.9 (us)");
  std::printf("%s\n", std::string(80, '-').c_str());
}

void PrintResult(Result& result) {
  result.latency.Finalize();
  const double ops_per_sec =
      result.seconds > 0 ? static_cast<double>(result.operations) / result.seconds
                         : 0.0;
  std::printf("%-10s %12llu %12.0f %10.2f %10.2f %10.2f %10.2f\n",
              result.name.c_str(),
              static_cast<unsigned long long>(result.operations), ops_per_sec,
              result.latency.PercentileMicros(0.50),
              result.latency.PercentileMicros(0.95),
              result.latency.PercentileMicros(0.99),
              result.latency.PercentileMicros(0.999));
}

lsm::SyncPolicy ParseSyncPolicy(const std::string& name) {
  if (name == "every") return lsm::SyncPolicy::kEveryWrite;
  if (name == "interval") return lsm::SyncPolicy::kInterval;
  return lsm::SyncPolicy::kNever;
}

// One unit of work in a mixed workload.
enum class Op { kRead, kUpdate, kInsert, kScan, kReadModifyWrite };

struct WorkloadSpec {
  std::string name;
  // Cumulative probabilities, in the order read / update / insert / scan / rmw.
  double read = 0, update = 0, insert = 0, scan = 0, rmw = 0;
  bool latest_distribution = false;
};

WorkloadSpec SpecFor(const std::string& name) {
  WorkloadSpec spec;
  spec.name = name;
  if (name == "A") {
    spec.read = 0.50;
    spec.update = 0.50;
  } else if (name == "B") {
    spec.read = 0.95;
    spec.update = 0.05;
  } else if (name == "C") {
    spec.read = 1.00;
  } else if (name == "D") {
    spec.read = 0.95;
    spec.insert = 0.05;
    spec.latest_distribution = true;
  } else if (name == "E") {
    spec.scan = 0.95;
    spec.insert = 0.05;
  } else if (name == "F") {
    spec.read = 0.50;
    spec.rmw = 0.50;
  }
  return spec;
}

Op PickOp(const WorkloadSpec& spec, double u) {
  double acc = spec.read;
  if (u < acc) return Op::kRead;
  acc += spec.update;
  if (u < acc) return Op::kUpdate;
  acc += spec.insert;
  if (u < acc) return Op::kInsert;
  acc += spec.scan;
  if (u < acc) return Op::kScan;
  return Op::kReadModifyWrite;
}

Result RunLoad(bench::Engine* db, const Config& config) {
  Result result;
  result.name = "load";
  result.latency.Reserve(config.num_keys);

  Timer total;
  for (uint64_t i = 0; i < config.num_keys; ++i) {
    const Timer op;
    const bool ok = db->Put(MakeKey(i), MakeValue(i, config.value_size));
    result.latency.Add(op.ElapsedNanos());
    if (!ok) {
      std::fprintf(stderr, "load failed at %llu\n",
                   static_cast<unsigned long long>(i));
      std::exit(1);
    }
  }
  result.seconds = total.ElapsedSeconds();
  result.operations = config.num_keys;
  return result;
}

Result RunMixed(bench::Engine* db, const Config& config,
                const WorkloadSpec& spec) {
  const uint64_t ops_per_thread =
      config.num_ops / static_cast<uint64_t>(config.threads);
  std::vector<LatencyRecorder> per_thread(config.threads);
  std::vector<std::thread> workers;
  std::atomic<uint64_t> next_insert_key{config.num_keys};
  std::atomic<uint64_t> errors{0};

  Timer total;
  for (int t = 0; t < config.threads; ++t) {
    workers.emplace_back([&, t] {
      LatencyRecorder& latency = per_thread[t];
      latency.Reserve(ops_per_thread);
      std::mt19937_64 rng(0x5eed ^ static_cast<uint64_t>(t));
      ZipfGenerator zipf(config.num_keys, config.zipf_theta,
                         static_cast<uint32_t>(1234 + t));
      std::string value;

      for (uint64_t i = 0; i < ops_per_thread; ++i) {
        const double u =
            static_cast<double>(rng()) / static_cast<double>(rng.max());
        const Op op = PickOp(spec, u);

        uint64_t key_index;
        if (spec.latest_distribution) {
          // Read-latest: bias hard towards the most recently inserted keys.
          const uint64_t ceiling = next_insert_key.load(std::memory_order_relaxed);
          const uint64_t back = zipf.Next();
          key_index = back < ceiling ? ceiling - 1 - back : 0;
        } else {
          key_index = zipf.Next();
        }

        const Timer timer;
        switch (op) {
          case Op::kRead: {
            bool error = false;
            db->Get(MakeKey(key_index), &value, &error);
            if (error) errors.fetch_add(1);
            break;
          }
          case Op::kUpdate: {
            if (!db->Put(MakeKey(key_index),
                         MakeValue(key_index, config.value_size))) {
              errors.fetch_add(1);
            }
            break;
          }
          case Op::kInsert: {
            const uint64_t id =
                next_insert_key.fetch_add(1, std::memory_order_relaxed);
            if (!db->Put(MakeKey(id), MakeValue(id, config.value_size))) {
              errors.fetch_add(1);
            }
            break;
          }
          case Op::kScan: {
            db->Scan(MakeKey(key_index), config.scan_length);
            break;
          }
          case Op::kReadModifyWrite: {
            bool error = false;
            db->Get(MakeKey(key_index), &value, &error);
            if (error) errors.fetch_add(1);
            if (!db->Put(MakeKey(key_index),
                         MakeValue(key_index + 1, config.value_size))) {
              errors.fetch_add(1);
            }
            break;
          }
        }
        latency.Add(timer.ElapsedNanos());
      }
    });
  }
  for (auto& w : workers) w.join();

  Result result;
  result.name = spec.name;
  result.seconds = total.ElapsedSeconds();
  result.operations = ops_per_thread * static_cast<uint64_t>(config.threads);
  for (const auto& recorder : per_thread) result.latency.Merge(recorder);
  if (errors.load() > 0) {
    std::fprintf(stderr, "workload %s: %llu errors\n", spec.name.c_str(),
                 static_cast<unsigned long long>(errors.load()));
  }
  return result;
}

std::vector<std::string> Split(const std::string& s, char sep) {
  std::vector<std::string> parts;
  std::string current;
  for (const char c : s) {
    if (c == sep) {
      if (!current.empty()) parts.push_back(current);
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  if (!current.empty()) parts.push_back(current);
  return parts;
}

}  // namespace

int main(int argc, char** argv) {
  Config config;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&]() { return std::string(argv[++i]); };
    if (arg == "--db" && i + 1 < argc) config.db_path = next();
    else if (arg == "--keys" && i + 1 < argc) config.num_keys = std::strtoull(next().c_str(), nullptr, 10);
    else if (arg == "--ops" && i + 1 < argc) config.num_ops = std::strtoull(next().c_str(), nullptr, 10);
    else if (arg == "--value-size" && i + 1 < argc) config.value_size = std::strtoul(next().c_str(), nullptr, 10);
    else if (arg == "--threads" && i + 1 < argc) config.threads = std::atoi(next().c_str());
    else if (arg == "--scan-length" && i + 1 < argc) config.scan_length = std::atoi(next().c_str());
    else if (arg == "--sync" && i + 1 < argc) config.sync_policy = next();
    else if (arg == "--workloads" && i + 1 < argc) config.workloads = next();
    else if (arg == "--engine" && i + 1 < argc) config.engine = next();
    else if (arg == "--block-cache-mb" && i + 1 < argc) config.block_cache_bytes = std::strtoull(next().c_str(), nullptr, 10) << 20;
    else if (arg == "--keep") config.keep_db = true;
    else {
      std::fprintf(stderr,
                   "usage: %s [--db PATH] [--keys N] [--ops N] [--value-size N]\n"
                   "          [--threads N] [--scan-length N] [--sync every|interval|never]\n"
                   "          [--workloads load,A,B,C,D,E,F] [--engine lsmtree|leveldb] [--keep]\n",
                   argv[0]);
      return 2;
    }
  }

  if (std::system(("rm -rf '" + config.db_path + "'").c_str()) != 0) {
    std::fprintf(stderr, "warning: could not clear %s\n", config.db_path.c_str());
  }

  bench::EngineConfig engine_config;
  engine_config.path = config.db_path;
  engine_config.memtable_size_bytes = 4u << 20;
  engine_config.block_size_bytes = 4096;
  engine_config.bloom_bits_per_key = 10;
  engine_config.block_cache_bytes = config.block_cache_bytes;
  engine_config.sync_every_write = config.sync_policy == "every";

  std::unique_ptr<bench::Engine> db;
  std::string error;
  if (config.engine == "leveldb") {
#ifdef LSM_BENCH_LEVELDB
    db = bench::LevelDbEngine::Open(engine_config, &error);
#else
    std::fprintf(stderr,
                 "this build has no LevelDB baseline; install leveldb and "
                 "reconfigure with -DLSM_BENCH_LEVELDB=ON\n");
    return 2;
#endif
  } else if (config.engine == "lsmtree") {
    db = bench::LsmEngine::Open(engine_config,
                                ParseSyncPolicy(config.sync_policy), &error);
  } else {
    std::fprintf(stderr, "unknown engine '%s'\n", config.engine.c_str());
    return 2;
  }
  if (db == nullptr) {
    std::fprintf(stderr, "open failed: %s\n", error.c_str());
    return 1;
  }

  std::printf("%s benchmark\n", db->name());
  std::printf("  keys=%llu ops=%llu value_size=%zu threads=%d sync=%s\n",
              static_cast<unsigned long long>(config.num_keys),
              static_cast<unsigned long long>(config.num_ops), config.value_size,
              config.threads, config.sync_policy.c_str());
  std::printf("  block_cache=%zu MiB\n", config.block_cache_bytes >> 20);

  PrintHeader();
  for (const std::string& name : Split(config.workloads, ',')) {
    Result result;
    if (name == "load") {
      result = RunLoad(db.get(), config);
      // Settle the tree before measuring reads, so read numbers reflect a
      // compacted shape rather than a backlog of L0 files.
      db->WaitForBackgroundWork();
    } else {
      result = RunMixed(db.get(), config, SpecFor(name));
    }
    PrintResult(result);
  }

  db->WaitForBackgroundWork();
  const std::string counters = db->CounterReport();
  if (!counters.empty()) {
    std::printf("\nengine counters\n%s", counters.c_str());
  }

  db.reset();
  if (!config.keep_db) {
    if (std::system(("rm -rf '" + config.db_path + "'").c_str()) != 0) {
      std::fprintf(stderr, "warning: could not clean %s\n", config.db_path.c_str());
    }
  }
  return 0;
}
