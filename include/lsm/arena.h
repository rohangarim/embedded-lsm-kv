#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace lsm {

// Bump allocator backing the memtable. A memtable is written once and dropped
// whole, so per-node free() is wasted work: we hand out pointers from big
// blocks and release everything in the destructor. This also keeps skip-list
// nodes dense in memory, which matters more for lookup cost than the allocator
// savings do.
class Arena {
 public:
  Arena() = default;
  ~Arena() = default;

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  char* Allocate(size_t bytes);

  // Aligned to max_align_t so the caller can placement-new anything into it.
  char* AllocateAligned(size_t bytes);

  // Bytes handed to malloc, not bytes handed out -- this is what the memtable
  // flush threshold is compared against.
  size_t MemoryUsage() const {
    return memory_usage_.load(std::memory_order_relaxed);
  }

 private:
  static constexpr size_t kBlockSize = 4096;

  char* AllocateFallback(size_t bytes, bool aligned);
  char* AllocateNewBlock(size_t block_bytes);

  char* alloc_ptr_ = nullptr;
  size_t alloc_bytes_remaining_ = 0;
  std::vector<std::unique_ptr<char[]>> blocks_;
  std::atomic<size_t> memory_usage_{0};
};

inline char* Arena::Allocate(size_t bytes) {
  if (bytes <= alloc_bytes_remaining_) {
    char* result = alloc_ptr_;
    alloc_ptr_ += bytes;
    alloc_bytes_remaining_ -= bytes;
    return result;
  }
  return AllocateFallback(bytes, /*aligned=*/false);
}

inline char* Arena::AllocateAligned(size_t bytes) {
  constexpr size_t kAlign = alignof(std::max_align_t);
  static_assert((kAlign & (kAlign - 1)) == 0, "alignment must be a power of 2");
  const size_t current = reinterpret_cast<uintptr_t>(alloc_ptr_) & (kAlign - 1);
  const size_t slop = (current == 0) ? 0 : kAlign - current;
  if (bytes + slop <= alloc_bytes_remaining_) {
    char* result = alloc_ptr_ + slop;
    alloc_ptr_ += bytes + slop;
    alloc_bytes_remaining_ -= bytes + slop;
    return result;
  }
  return AllocateFallback(bytes, /*aligned=*/true);
}

}  // namespace lsm
