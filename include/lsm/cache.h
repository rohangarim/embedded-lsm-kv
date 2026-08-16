#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace lsm {

// Sharded LRU cache of decoded SSTable data blocks.
//
// Without this, every block a lookup touches costs a pread plus a fresh parse
// of the block's length prefixes. The OS page cache absorbs the disk I/O but
// not the copy out of it and not the re-parse -- which is exactly the cost that
// dominates scans, where stepping over the shadowed versions of a hot key can
// touch a hundred blocks for a single logical row.
//
// Entries are handed out as shared_ptr, so a reader that is mid-scan keeps its
// block alive even if the cache evicts it underneath. That is what makes
// eviction safe without any reference-counting protocol at the call site.
//
// Sharded by key hash because the cache sits on the hot path of every reader
// thread; a single mutex would serialize them against each other.
class BlockCache {
 public:
  using Handle = std::shared_ptr<const std::string>;

  explicit BlockCache(size_t capacity_bytes, int num_shards = 16);

  BlockCache(const BlockCache&) = delete;
  BlockCache& operator=(const BlockCache&) = delete;

  // Returns nullptr on a miss.
  Handle Lookup(uint64_t file_number, uint64_t offset);

  // Takes ownership of `block` and returns a handle to the cached copy.
  Handle Insert(uint64_t file_number, uint64_t offset, std::string block);

  // Drops every block belonging to a file. Called when a compaction retires an
  // input file, so its blocks stop occupying capacity that live files need.
  void EraseFile(uint64_t file_number);

  size_t capacity_bytes() const { return capacity_bytes_; }
  size_t charge_bytes() const;

  uint64_t hits() const { return hits_.load(std::memory_order_relaxed); }
  uint64_t misses() const { return misses_.load(std::memory_order_relaxed); }
  uint64_t evictions() const { return evictions_.load(std::memory_order_relaxed); }

  double HitRate() const {
    const uint64_t h = hits(), m = misses();
    return (h + m) == 0 ? 0.0 : static_cast<double>(h) / static_cast<double>(h + m);
  }

 private:
  struct Key {
    uint64_t file_number;
    uint64_t offset;
    bool operator==(const Key& other) const {
      return file_number == other.file_number && offset == other.offset;
    }
  };

  struct KeyHash {
    size_t operator()(const Key& k) const {
      // Two independently-varying 64-bit fields; mix rather than xor so that
      // (file 1, offset 2) and (file 2, offset 1) do not collide.
      uint64_t h = k.file_number * 0x9e3779b97f4a7c15ull;
      h ^= k.offset + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
      return static_cast<size_t>(h);
    }
  };

  struct Entry {
    Key key;
    Handle value;
    size_t charge;
  };

  // Least-recently-used lives at the back of `order`.
  struct Shard {
    mutable std::mutex mu;
    std::list<Entry> order;
    std::unordered_map<Key, std::list<Entry>::iterator, KeyHash> index;
    size_t charge = 0;
    size_t capacity = 0;
  };

  Shard& ShardFor(const Key& key) {
    return shards_[KeyHash()(key) % shards_.size()];
  }

  const size_t capacity_bytes_;
  std::vector<Shard> shards_;
  std::atomic<uint64_t> hits_{0};
  std::atomic<uint64_t> misses_{0};
  std::atomic<uint64_t> evictions_{0};
};

}  // namespace lsm
