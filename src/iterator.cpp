#include "lsm/iterator.h"

#include <cassert>
#include <utility>

#include "lsm/internal_key.h"

namespace lsm {
namespace {

class MergingIterator : public Iterator {
 public:
  explicit MergingIterator(std::vector<std::unique_ptr<Iterator>> children)
      : children_(std::move(children)) {}

  bool Valid() const override { return current_ != nullptr; }

  void SeekToFirst() override {
    for (auto& child : children_) child->SeekToFirst();
    FindSmallest();
  }

  void Seek(std::string_view target) override {
    for (auto& child : children_) child->Seek(target);
    FindSmallest();
  }

  void Next() override {
    assert(Valid());
    current_->Next();
    FindSmallest();
  }

  std::string_view key() const override { return current_->key(); }
  std::string_view value() const override { return current_->value(); }

  Status status() const override {
    for (const auto& child : children_) {
      const Status s = child->status();
      if (!s.ok()) return s;
    }
    return Status::OK();
  }

 private:
  void FindSmallest() {
    Iterator* smallest = nullptr;
    for (auto& child : children_) {
      if (!child->Valid()) continue;
      // Strict less-than keeps the earliest child on an exact tie, which is the
      // newest source.
      if (smallest == nullptr ||
          CompareInternalKey(child->key(), smallest->key()) < 0) {
        smallest = child.get();
      }
    }
    current_ = smallest;
  }

  std::vector<std::unique_ptr<Iterator>> children_;
  Iterator* current_ = nullptr;
};

}  // namespace

std::unique_ptr<Iterator> NewMergingIterator(
    std::vector<std::unique_ptr<Iterator>> children) {
  return std::make_unique<MergingIterator>(std::move(children));
}

}  // namespace lsm
