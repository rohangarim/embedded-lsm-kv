#include "lsm/cache.h"

#include <algorithm>
#include <utility>

namespace lsm {

BlockCache::BlockCache(size_t capacity_bytes, int num_shards)
    : capacity_bytes_(capacity_bytes),
      shards_(static_cast<size_t>(std::max(1, num_shards))) {
  // Split the budget evenly. Shards drift apart slightly under a skewed key
  // distribution, which is the accepted cost of not having one global lock.
  const size_t per_shard = capacity_bytes / shards_.size();
  for (Shard& shard : shards_) shard.capacity = per_shard;
}

BlockCache::Handle BlockCache::Lookup(uint64_t file_number, uint64_t offset) {
  const Key key{file_number, offset};
  Shard& shard = ShardFor(key);

  std::lock_guard<std::mutex> guard(shard.mu);
  const auto it = shard.index.find(key);
  if (it == shard.index.end()) {
    misses_.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  // Touch: move to the front of the LRU order. splice keeps the iterator (and
  // therefore the index entry) valid, so there is nothing to fix up.
  shard.order.splice(shard.order.begin(), shard.order, it->second);
  hits_.fetch_add(1, std::memory_order_relaxed);
  return it->second->value;
}

BlockCache::Handle BlockCache::Insert(uint64_t file_number, uint64_t offset,
                                      std::string block) {
  const Key key{file_number, offset};
  const size_t charge = block.size();
  auto value = std::make_shared<const std::string>(std::move(block));

  Shard& shard = ShardFor(key);
  std::lock_guard<std::mutex> guard(shard.mu);

  // Two readers can miss on the same block and both insert it. Keep the
  // existing entry so any handle already handed out stays the canonical one.
  const auto existing = shard.index.find(key);
  if (existing != shard.index.end()) {
    shard.order.splice(shard.order.begin(), shard.order, existing->second);
    return existing->second->value;
  }

  shard.order.push_front(Entry{key, value, charge});
  shard.index.emplace(key, shard.order.begin());
  shard.charge += charge;

  while (shard.charge > shard.capacity && !shard.order.empty()) {
    // Never evict the entry we just inserted, even if it alone exceeds the
    // shard budget -- the caller is about to read it.
    auto victim = std::prev(shard.order.end());
    if (victim == shard.order.begin()) break;
    shard.charge -= victim->charge;
    shard.index.erase(victim->key);
    shard.order.erase(victim);
    evictions_.fetch_add(1, std::memory_order_relaxed);
  }
  return value;
}

void BlockCache::EraseFile(uint64_t file_number) {
  for (Shard& shard : shards_) {
    std::lock_guard<std::mutex> guard(shard.mu);
    for (auto it = shard.order.begin(); it != shard.order.end();) {
      if (it->key.file_number == file_number) {
        shard.charge -= it->charge;
        shard.index.erase(it->key);
        it = shard.order.erase(it);
      } else {
        ++it;
      }
    }
  }
}

size_t BlockCache::charge_bytes() const {
  size_t total = 0;
  for (const Shard& shard : shards_) {
    std::lock_guard<std::mutex> guard(shard.mu);
    total += shard.charge;
  }
  return total;
}

}  // namespace lsm
