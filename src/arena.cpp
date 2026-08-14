#include "lsm/arena.h"

namespace lsm {

char* Arena::AllocateNewBlock(size_t block_bytes) {
  blocks_.push_back(std::unique_ptr<char[]>(new char[block_bytes]));
  memory_usage_.fetch_add(block_bytes + sizeof(std::unique_ptr<char[]>),
                          std::memory_order_relaxed);
  return blocks_.back().get();
}

char* Arena::AllocateFallback(size_t bytes, bool aligned) {
  (void)aligned;  // new[] is already suitably aligned for max_align_t.
  if (bytes > kBlockSize / 4) {
    // Big request: give it its own block rather than wasting the tail of the
    // current one.
    return AllocateNewBlock(bytes);
  }
  // Abandon whatever is left in the current block; at most kBlockSize/4 bytes.
  alloc_ptr_ = AllocateNewBlock(kBlockSize);
  alloc_bytes_remaining_ = kBlockSize;

  char* result = alloc_ptr_;
  alloc_ptr_ += bytes;
  alloc_bytes_remaining_ -= bytes;
  return result;
}

}  // namespace lsm
