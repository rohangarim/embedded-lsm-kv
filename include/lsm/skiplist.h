#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <random>
#include <string_view>
#include <vector>

#include "lsm/arena.h"

namespace lsm {

// Insert-only skip list keyed by raw bytes, ordered by a caller-supplied
// comparator.
//
// Why a skip list rather than a red-black tree: insertion only ever publishes
// forward pointers, never rotates. That means a single writer can insert while
// any number of readers traverse concurrently, with no lock on the read path --
// each `next` pointer is an atomic whose release-store is paired with the
// reader's acquire-load, so a reader either sees the fully-built node or does
// not see it at all. A balanced BST has to rewire a subtree to rebalance, which
// is not something readers can walk through safely without locking.
//
// Nodes and keys live in an Arena and are freed all at once when the list dies;
// there is no per-node destructor to run.
template <class Comparator>
class SkipList {
 private:
  struct Node;

 public:
  explicit SkipList(Comparator cmp, Arena* arena);

  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  // Inserts a copy of `key`. Requires: no equal key is already present, and no
  // other thread is calling Insert concurrently.
  void Insert(std::string_view key);

  bool Contains(std::string_view key) const;

  size_t size() const { return size_.load(std::memory_order_relaxed); }

  class Iterator {
   public:
    explicit Iterator(const SkipList* list) : list_(list), node_(nullptr) {}

    bool Valid() const { return node_ != nullptr; }
    std::string_view key() const {
      assert(Valid());
      return node_->key();
    }
    void Next() {
      assert(Valid());
      node_ = node_->Next(0);
    }
    // Positions at the first entry with key >= target.
    void Seek(std::string_view target) {
      node_ = list_->FindGreaterOrEqual(target, nullptr);
    }
    void SeekToFirst() { node_ = list_->head_->Next(0); }

   private:
    const SkipList* list_;
    Node* node_;
  };

 private:
  static constexpr int kMaxHeight = 12;
  // p = 1/4 gives ~1.33 pointers per node on average and a search cost within a
  // small constant of a balanced tree, while keeping node overhead low.
  static constexpr uint32_t kBranching = 4;

  Node* NewNode(std::string_view key, int height);
  int RandomHeight();
  bool KeyIsAfterNode(std::string_view key, Node* n) const;
  // Returns the first node with key >= `key`. If `prev` is non-null, fills
  // prev[i] with the last node visited at level i (the splice points).
  Node* FindGreaterOrEqual(std::string_view key, Node** prev) const;

  Comparator const compare_;
  Arena* const arena_;
  Node* const head_;

  // Only mutated by the writer thread; read by readers with relaxed ordering
  // because reading a stale (smaller) height is harmless -- it just starts the
  // search lower down.
  std::atomic<int> max_height_{1};
  std::atomic<size_t> size_{0};
  std::mt19937 rnd_{0xdeadbeef};
};

template <class Comparator>
struct SkipList<Comparator>::Node {
  uint32_t key_length;
  int height;

  std::string_view key() const {
    return std::string_view(reinterpret_cast<const char*>(this) + KeyOffset(height),
                            key_length);
  }

  // Node layout: header, then `height` atomic next pointers, then key bytes.
  static size_t KeyOffset(int h) {
    return sizeof(Node) + sizeof(std::atomic<Node*>) * static_cast<size_t>(h);
  }

  std::atomic<Node*>* next_slots() {
    return reinterpret_cast<std::atomic<Node*>*>(reinterpret_cast<char*>(this) +
                                                 sizeof(Node));
  }
  const std::atomic<Node*>* next_slots() const {
    return reinterpret_cast<const std::atomic<Node*>*>(
        reinterpret_cast<const char*>(this) + sizeof(Node));
  }

  Node* Next(int level) const {
    assert(level >= 0 && level < height);
    // Acquire: pairs with the release in SetNext so a reader that observes this
    // node also observes its fully-initialised key bytes.
    return next_slots()[level].load(std::memory_order_acquire);
  }
  void SetNext(int level, Node* n) {
    assert(level >= 0 && level < height);
    next_slots()[level].store(n, std::memory_order_release);
  }
  // Safe only while the node is still unreachable by readers.
  void NoBarrierSetNext(int level, Node* n) {
    next_slots()[level].store(n, std::memory_order_relaxed);
  }
  Node* NoBarrierNext(int level) const {
    return next_slots()[level].load(std::memory_order_relaxed);
  }
};

template <class Comparator>
typename SkipList<Comparator>::Node* SkipList<Comparator>::NewNode(
    std::string_view key, int height) {
  const size_t bytes = Node::KeyOffset(height) + key.size();
  char* mem = arena_->AllocateAligned(bytes);
  auto* node = reinterpret_cast<Node*>(mem);
  node->key_length = static_cast<uint32_t>(key.size());
  node->height = height;
  for (int i = 0; i < height; ++i) {
    new (&node->next_slots()[i]) std::atomic<Node*>(nullptr);
  }
  std::memcpy(mem + Node::KeyOffset(height), key.data(), key.size());
  return node;
}

template <class Comparator>
SkipList<Comparator>::SkipList(Comparator cmp, Arena* arena)
    : compare_(cmp), arena_(arena), head_(NewNode(std::string_view(), kMaxHeight)) {}

template <class Comparator>
int SkipList<Comparator>::RandomHeight() {
  int height = 1;
  while (height < kMaxHeight && (rnd_() % kBranching) == 0) {
    ++height;
  }
  return height;
}

template <class Comparator>
bool SkipList<Comparator>::KeyIsAfterNode(std::string_view key, Node* n) const {
  return n != nullptr && compare_(n->key(), key) < 0;
}

template <class Comparator>
typename SkipList<Comparator>::Node* SkipList<Comparator>::FindGreaterOrEqual(
    std::string_view key, Node** prev) const {
  Node* x = head_;
  int level = max_height_.load(std::memory_order_relaxed) - 1;
  while (true) {
    Node* next = x->Next(level);
    if (KeyIsAfterNode(key, next)) {
      x = next;  // Keep going right on this level.
    } else {
      if (prev != nullptr) prev[level] = x;
      if (level == 0) return next;
      --level;
    }
  }
}

template <class Comparator>
void SkipList<Comparator>::Insert(std::string_view key) {
  Node* prev[kMaxHeight];
  Node* next = FindGreaterOrEqual(key, prev);
  assert(next == nullptr || compare_(next->key(), key) != 0);
  (void)next;

  const int height = RandomHeight();
  const int cur_max = max_height_.load(std::memory_order_relaxed);
  if (height > cur_max) {
    for (int i = cur_max; i < height; ++i) prev[i] = head_;
    // Safe to publish before linking: a concurrent reader that starts at the
    // new height immediately reads head_'s null pointer at that level and drops
    // down, which is exactly what it would have done before.
    max_height_.store(height, std::memory_order_relaxed);
  }

  Node* node = NewNode(key, height);
  for (int i = 0; i < height; ++i) {
    // No barrier needed on the new node: it is not reachable yet.
    node->NoBarrierSetNext(i, prev[i]->NoBarrierNext(i));
    prev[i]->SetNext(i, node);  // Release-store publishes the node.
  }
  size_.fetch_add(1, std::memory_order_relaxed);
}

template <class Comparator>
bool SkipList<Comparator>::Contains(std::string_view key) const {
  Node* x = FindGreaterOrEqual(key, nullptr);
  return x != nullptr && compare_(x->key(), key) == 0;
}

}  // namespace lsm
