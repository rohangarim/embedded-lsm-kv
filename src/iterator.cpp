#include "lsm/iterator.h"

#include <cassert>
#include <string>
#include <utility>

#include "lsm/internal_key.h"

namespace lsm {
namespace {

// N-way merge over a binary min-heap of child cursors.
//
// The obvious implementation picks the smallest child by scanning all of them,
// which costs O(children) per step. That is fine for a compaction, which merges
// a handful of files, but a range scan opens a cursor per L0 file plus one per
// deeper level, and then has to step over every shadowed version of a key to
// reach the next live one. The linear scan gets paid hundreds of times per
// logical row, and it dominated scan throughput.
//
// A heap makes each step O(log children). Only the root moves per advance, so
// the work is one sift-down rather than a full rescan.
class MergingIterator : public Iterator {
 public:
  explicit MergingIterator(std::vector<std::unique_ptr<Iterator>> children)
      : children_(std::move(children)) {
    heap_.reserve(children_.size());
  }

  bool Valid() const override {
    return direction_ == kForward ? !heap_.empty() : current_ < children_.size();
  }

  void SeekToFirst() override {
    for (auto& child : children_) child->SeekToFirst();
    direction_ = kForward;
    BuildHeap();
  }

  void SeekToLast() override {
    for (auto& child : children_) child->SeekToLast();
    direction_ = kReverse;
    FindLargest();
  }

  void Seek(std::string_view target) override {
    for (auto& child : children_) child->Seek(target);
    direction_ = kForward;
    BuildHeap();
  }

  void Next() override {
    assert(Valid());
    if (direction_ != kForward) {
      // Every child but the current one is left over from reverse iteration,
      // positioned at or before key(). Bring each one to just past it so the
      // heap's invariant -- no child positioned earlier than what was last
      // reported -- holds again.
      const std::string saved_key(key());
      const size_t current = current_;
      for (size_t i = 0; i < children_.size(); ++i) {
        if (i == current) continue;
        Iterator* child = children_[i].get();
        child->Seek(saved_key);
        if (child->Valid() && CompareInternalKey(child->key(), saved_key) == 0) {
          child->Next();
        }
      }
      // The current child is still sitting on saved_key too -- it must move
      // past it like every other child did, or the very next entry reported
      // would be a duplicate of the one already returned.
      children_[current].get()->Next();
      direction_ = kForward;
      BuildHeap();
      return;
    }
    Iterator* top = children_[heap_.front()].get();
    top->Next();
    if (top->Valid()) {
      SiftDown(0);
    } else {
      // Exhausted: pull the last element into the root and shrink.
      heap_.front() = heap_.back();
      heap_.pop_back();
      if (!heap_.empty()) SiftDown(0);
    }
  }

  void Prev() override {
    assert(Valid());
    if (direction_ != kReverse) {
      // Mirror image of the forward switch: bring every other child to just
      // before key(), so the next linear scan for the largest key is correct.
      const std::string saved_key(key());
      const size_t current = heap_.front();
      for (size_t i = 0; i < children_.size(); ++i) {
        if (i == current) continue;
        Iterator* child = children_[i].get();
        child->Seek(saved_key);
        if (child->Valid()) {
          child->Prev();
        } else {
          child->SeekToLast();
        }
      }
      // Same reasoning as the forward switch, mirrored: the current child is
      // still on saved_key and has to move off it too, or FindLargest below
      // would just find saved_key again.
      children_[current].get()->Prev();
      current_ = current;
      direction_ = kReverse;
      FindLargest();
      return;
    }
    children_[current_]->Prev();
    FindLargest();
  }

  std::string_view key() const override {
    assert(Valid());
    return direction_ == kForward ? children_[heap_.front()]->key()
                                  : children_[current_]->key();
  }

  std::string_view value() const override {
    assert(Valid());
    return direction_ == kForward ? children_[heap_.front()]->value()
                                  : children_[current_]->value();
  }

  Status status() const override {
    for (const auto& child : children_) {
      const Status s = child->status();
      if (!s.ok()) return s;
    }
    return Status::OK();
  }

 private:
  // True when the child at slot `a` must sort after the one at slot `b`.
  // Equal internal keys are broken by child order, so callers that pass their
  // children newest-first get the newest source on a tie -- the same rule the
  // linear version followed by using a strict less-than.
  bool Greater(size_t a, size_t b) const {
    const int c = CompareInternalKey(children_[a]->key(), children_[b]->key());
    if (c != 0) return c > 0;
    return a > b;
  }

  void BuildHeap() {
    heap_.clear();
    for (size_t i = 0; i < children_.size(); ++i) {
      if (children_[i]->Valid()) heap_.push_back(i);
    }
    // Floyd's construction: O(n), rather than n pushes at O(log n) each.
    for (size_t i = heap_.size() / 2; i-- > 0;) SiftDown(i);
  }

  void SiftDown(size_t pos) {
    const size_t n = heap_.size();
    const size_t slot = heap_[pos];
    while (true) {
      const size_t left = 2 * pos + 1;
      if (left >= n) break;
      const size_t right = left + 1;
      size_t smallest = left;
      if (right < n && Greater(heap_[left], heap_[right])) smallest = right;
      if (!Greater(slot, heap_[smallest])) break;
      heap_[pos] = heap_[smallest];
      pos = smallest;
    }
    heap_[pos] = slot;
  }

  // Reverse iteration is rare next to the compaction/scan path the heap is
  // tuned for, so it falls back to a plain linear scan for whichever child
  // holds the largest key -- no heap maintenance, just current_ updated in
  // place.
  void FindLargest() {
    current_ = children_.size();
    for (size_t i = 0; i < children_.size(); ++i) {
      if (!children_[i]->Valid()) continue;
      if (current_ == children_.size()) {
        current_ = i;
        continue;
      }
      // Ties keep the lower index, matching Greater()'s forward tie rule.
      if (CompareInternalKey(children_[i]->key(), children_[current_]->key()) > 0) {
        current_ = i;
      }
    }
  }

  std::vector<std::unique_ptr<Iterator>> children_;
  // Indices into children_, arranged as a min-heap by current key. Children
  // that have run out are simply absent. Only meaningful in kForward.
  std::vector<size_t> heap_;
  enum Direction { kForward, kReverse };
  Direction direction_ = kForward;
  // Index of the child holding the largest key. Only meaningful in kReverse.
  size_t current_ = 0;
};

}  // namespace

std::unique_ptr<Iterator> NewMergingIterator(
    std::vector<std::unique_ptr<Iterator>> children) {
  return std::make_unique<MergingIterator>(std::move(children));
}

}  // namespace lsm
