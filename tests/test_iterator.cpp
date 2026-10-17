#include "lsm/iterator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "lsm/internal_key.h"

namespace lsm {
namespace {

// Minimal in-memory cursor, so the merge can be tested without going near a
// file. Entries must already be in internal-key order.
class VectorIterator : public Iterator {
 public:
  explicit VectorIterator(std::vector<std::pair<std::string, std::string>> entries)
      : entries_(std::move(entries)) {}

  bool Valid() const override { return index_ < entries_.size(); }
  void SeekToFirst() override { index_ = 0; }
  void SeekToLast() override {
    index_ = entries_.empty() ? 0 : entries_.size() - 1;
  }
  void Next() override { ++index_; }
  void Prev() override {
    if (index_ == 0) {
      index_ = entries_.size();  // Before-the-beginning: invalid, like Next().
    } else {
      --index_;
    }
  }
  void Seek(std::string_view target) override {
    index_ = 0;
    while (index_ < entries_.size() &&
           CompareInternalKey(entries_[index_].first, target) < 0) {
      ++index_;
    }
  }
  std::string_view key() const override { return entries_[index_].first; }
  std::string_view value() const override { return entries_[index_].second; }

 private:
  std::vector<std::pair<std::string, std::string>> entries_;
  size_t index_ = 0;
};

std::string Key(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "k%08d", i);
  return buf;
}

using Entries = std::vector<std::pair<std::string, std::string>>;

std::unique_ptr<Iterator> Child(Entries entries) {
  return std::make_unique<VectorIterator>(std::move(entries));
}

// Collects everything the merge yields.
Entries Drain(Iterator* iter) {
  Entries out;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    out.emplace_back(std::string(iter->key()), std::string(iter->value()));
  }
  return out;
}

}  // namespace

TEST(MergingIterator, NoChildrenIsEmpty) {
  auto merged = NewMergingIterator({});
  merged->SeekToFirst();
  EXPECT_FALSE(merged->Valid());
  merged->Seek(EncodeInternalKey("anything", 1, ValueType::kValue));
  EXPECT_FALSE(merged->Valid());
}

TEST(MergingIterator, EmptyChildrenAreSkipped) {
  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(Child({}));
  children.push_back(
      Child({{EncodeInternalKey("b", 1, ValueType::kValue), "vb"}}));
  children.push_back(Child({}));

  auto merged = NewMergingIterator(std::move(children));
  const Entries got = Drain(merged.get());
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(ExtractUserKey(got[0].first), "b");
}

TEST(MergingIterator, MergesTwoChildrenInOrder) {
  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(Child({{EncodeInternalKey("a", 1, ValueType::kValue), "va"},
                            {EncodeInternalKey("c", 1, ValueType::kValue), "vc"},
                            {EncodeInternalKey("e", 1, ValueType::kValue), "ve"}}));
  children.push_back(Child({{EncodeInternalKey("b", 1, ValueType::kValue), "vb"},
                            {EncodeInternalKey("d", 1, ValueType::kValue), "vd"}}));

  auto merged = NewMergingIterator(std::move(children));
  const Entries got = Drain(merged.get());

  ASSERT_EQ(got.size(), 5u);
  const std::string expected[] = {"a", "b", "c", "d", "e"};
  for (size_t i = 0; i < got.size(); ++i) {
    EXPECT_EQ(ExtractUserKey(got[i].first), expected[i]) << "position " << i;
  }
}

// Equal user keys must come out newest-sequence-first, which is what lets the
// read path treat the first hit as authoritative.
TEST(MergingIterator, EqualUserKeysComeOutNewestFirst) {
  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(Child({{EncodeInternalKey("k", 5, ValueType::kValue), "v5"}}));
  children.push_back(Child({{EncodeInternalKey("k", 9, ValueType::kValue), "v9"}}));
  children.push_back(Child({{EncodeInternalKey("k", 1, ValueType::kValue), "v1"}}));

  auto merged = NewMergingIterator(std::move(children));
  const Entries got = Drain(merged.get());

  ASSERT_EQ(got.size(), 3u);
  EXPECT_EQ(got[0].second, "v9");
  EXPECT_EQ(got[1].second, "v5");
  EXPECT_EQ(got[2].second, "v1");
}

// On a byte-identical internal key the earlier child wins, since callers pass
// their children newest-source-first.
TEST(MergingIterator, ExactTieResolvesToTheEarlierChild) {
  const std::string ikey = EncodeInternalKey("k", 7, ValueType::kValue);
  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(Child({{ikey, "from_newer_source"}}));
  children.push_back(Child({{ikey, "from_older_source"}}));

  auto merged = NewMergingIterator(std::move(children));
  merged->SeekToFirst();
  ASSERT_TRUE(merged->Valid());
  EXPECT_EQ(merged->value(), "from_newer_source");
}

TEST(MergingIterator, SeekPositionsAllChildren) {
  std::vector<std::unique_ptr<Iterator>> children;
  for (int child = 0; child < 4; ++child) {
    Entries entries;
    for (int i = child; i < 200; i += 4) {
      entries.emplace_back(EncodeInternalKey(Key(i), 1, ValueType::kValue),
                           "v" + std::to_string(i));
    }
    children.push_back(Child(std::move(entries)));
  }

  auto merged = NewMergingIterator(std::move(children));
  merged->Seek(EncodeInternalKey(Key(137), kMaxSequenceNumber, ValueType::kValue));
  ASSERT_TRUE(merged->Valid());
  EXPECT_EQ(ExtractUserKey(merged->key()), Key(137));

  // And keeps yielding in order from there.
  int expected = 137;
  for (; merged->Valid(); merged->Next()) {
    ASSERT_EQ(ExtractUserKey(merged->key()), Key(expected));
    ++expected;
  }
  EXPECT_EQ(expected, 200);
}

TEST(MergingIterator, SeekPastTheEndIsInvalid) {
  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(Child({{EncodeInternalKey("a", 1, ValueType::kValue), "va"}}));
  auto merged = NewMergingIterator(std::move(children));
  merged->Seek(EncodeInternalKey("zzz", 1, ValueType::kValue));
  EXPECT_FALSE(merged->Valid());
}

TEST(MergingIterator, ChildrenExhaustingAtDifferentTimes) {
  // One long child and several short ones, so the heap has to shrink repeatedly
  // mid-iteration.
  std::vector<std::unique_ptr<Iterator>> children;
  Entries long_child;
  for (int i = 0; i < 100; ++i) {
    long_child.emplace_back(EncodeInternalKey(Key(i * 10), 1, ValueType::kValue),
                            "long");
  }
  children.push_back(Child(std::move(long_child)));
  // Offset by one so these interleave with the long child's multiples of ten
  // rather than duplicating them -- the strict-ordering check below only holds
  // if every internal key is distinct.
  for (int c = 0; c < 5; ++c) {
    children.push_back(Child(
        {{EncodeInternalKey(Key(c * 10 + 1), 1, ValueType::kValue), "short"}}));
  }

  auto merged = NewMergingIterator(std::move(children));
  const Entries got = Drain(merged.get());
  EXPECT_EQ(got.size(), 105u);

  for (size_t i = 1; i < got.size(); ++i) {
    ASSERT_LT(CompareInternalKey(got[i - 1].first, got[i].first), 0)
        << "out of order at " << i;
  }
}

// The property that matters most: whatever the shape of the inputs, the merge
// yields exactly their union in internal-key order.
TEST(MergingIterator, MatchesASortedUnionOnRandomInputs) {
  std::mt19937 rng(20260816);
  for (int trial = 0; trial < 50; ++trial) {
    const int num_children = 1 + static_cast<int>(rng() % 20);
    Entries expected;
    std::vector<std::unique_ptr<Iterator>> children;

    for (int c = 0; c < num_children; ++c) {
      Entries entries;
      const int count = static_cast<int>(rng() % 40);
      for (int i = 0; i < count; ++i) {
        const std::string ikey = EncodeInternalKey(
            Key(static_cast<int>(rng() % 500)),
            static_cast<SequenceNumber>(1 + rng() % 100), ValueType::kValue);
        entries.emplace_back(ikey, "v");
      }
      std::sort(entries.begin(), entries.end(),
                [](const auto& a, const auto& b) {
                  return CompareInternalKey(a.first, b.first) < 0;
                });
      // Children must be internally sorted and duplicate-free.
      entries.erase(std::unique(entries.begin(), entries.end(),
                                [](const auto& a, const auto& b) {
                                  return a.first == b.first;
                                }),
                    entries.end());
      expected.insert(expected.end(), entries.begin(), entries.end());
      children.push_back(Child(std::move(entries)));
    }

    std::stable_sort(expected.begin(), expected.end(),
                     [](const auto& a, const auto& b) {
                       return CompareInternalKey(a.first, b.first) < 0;
                     });

    auto merged = NewMergingIterator(std::move(children));
    const Entries got = Drain(merged.get());

    ASSERT_EQ(got.size(), expected.size()) << "trial " << trial;
    for (size_t i = 0; i < got.size(); ++i) {
      ASSERT_EQ(got[i].first, expected[i].first) << "trial " << trial << " at " << i;
    }
  }
}

TEST(MergingIterator, ScalesToManyChildren) {
  // 200 children is far past what the engine creates, but the heap should not
  // care and the ordering must still hold.
  std::vector<std::unique_ptr<Iterator>> children;
  for (int c = 0; c < 200; ++c) {
    children.push_back(Child(
        {{EncodeInternalKey(Key(c), 1, ValueType::kValue), "v"}}));
  }
  auto merged = NewMergingIterator(std::move(children));
  const Entries got = Drain(merged.get());

  ASSERT_EQ(got.size(), 200u);
  for (int i = 0; i < 200; ++i) {
    EXPECT_EQ(ExtractUserKey(got[i].first), Key(i)) << i;
  }
}

// Collects everything a full backward walk yields, oldest call site to the
// caller's mind but newest-key-first on the wire -- same shape as Drain, the
// other direction.
Entries DrainReverse(Iterator* iter) {
  Entries out;
  for (iter->SeekToLast(); iter->Valid(); iter->Prev()) {
    out.emplace_back(std::string(iter->key()), std::string(iter->value()));
  }
  return out;
}

TEST(MergingIterator, SeekToLastOnNoChildrenIsInvalid) {
  auto merged = NewMergingIterator({});
  merged->SeekToLast();
  EXPECT_FALSE(merged->Valid());
}

TEST(MergingIterator, ReverseWalkIsTheForwardWalkBackwards) {
  std::vector<std::unique_ptr<Iterator>> children;
  for (int c = 0; c < 4; ++c) {
    Entries entries;
    for (int i = c; i < 200; i += 4) {
      entries.emplace_back(EncodeInternalKey(Key(i), 1, ValueType::kValue),
                           "v" + std::to_string(i));
    }
    children.push_back(Child(std::move(entries)));
  }
  // Rebuild the same children for the forward pass -- the merge consumes them.
  std::vector<std::unique_ptr<Iterator>> children2;
  for (int c = 0; c < 4; ++c) {
    Entries entries;
    for (int i = c; i < 200; i += 4) {
      entries.emplace_back(EncodeInternalKey(Key(i), 1, ValueType::kValue),
                           "v" + std::to_string(i));
    }
    children2.push_back(Child(std::move(entries)));
  }

  auto forward = NewMergingIterator(std::move(children2));
  Entries expected = Drain(forward.get());
  std::reverse(expected.begin(), expected.end());

  auto merged = NewMergingIterator(std::move(children));
  const Entries got = DrainReverse(merged.get());

  ASSERT_EQ(got, expected);
}

// A raw reverse walk is just the forward walk backwards: forward is
// newest-sequence-first for a given user key, so backward is oldest first.
// (Collapsing to "newest visible version only" is DBIter's job, layered on
// top of this -- the merge itself does not do it.)
TEST(MergingIterator, ReverseEqualUserKeysComeOutOldestFirst) {
  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(Child({{EncodeInternalKey("k", 5, ValueType::kValue), "v5"}}));
  children.push_back(Child({{EncodeInternalKey("k", 9, ValueType::kValue), "v9"}}));
  children.push_back(Child({{EncodeInternalKey("k", 1, ValueType::kValue), "v1"}}));

  auto merged = NewMergingIterator(std::move(children));
  const Entries got = DrainReverse(merged.get());

  ASSERT_EQ(got.size(), 3u);
  EXPECT_EQ(got[0].second, "v1");
  EXPECT_EQ(got[1].second, "v5");
  EXPECT_EQ(got[2].second, "v9");
}

// The case the direction-switch logic exists for: reverse a bit, then go
// forward again, and the stream must pick up exactly where a pure forward
// walk would have been -- no repeated and no skipped entries.
TEST(MergingIterator, ReversingThenForwardResumesCleanly) {
  std::vector<std::unique_ptr<Iterator>> children;
  for (int c = 0; c < 3; ++c) {
    Entries entries;
    for (int i = c; i < 60; i += 3) {
      entries.emplace_back(EncodeInternalKey(Key(i), 1, ValueType::kValue),
                           "v" + std::to_string(i));
    }
    children.push_back(Child(std::move(entries)));
  }
  auto merged = NewMergingIterator(std::move(children));

  merged->SeekToFirst();
  for (int i = 0; i < 30; ++i) merged->Next();  // Walk to key 30.
  ASSERT_TRUE(merged->Valid());
  ASSERT_EQ(ExtractUserKey(merged->key()), Key(30));

  for (int i = 0; i < 10; ++i) merged->Prev();  // Back to key 20.
  ASSERT_TRUE(merged->Valid());
  ASSERT_EQ(ExtractUserKey(merged->key()), Key(20));

  // Forward again: must retrace 20, 21, ..., 59 with nothing missing or
  // repeated, exactly as an uninterrupted forward walk would have.
  for (int expected = 20; expected < 60; ++expected) {
    ASSERT_TRUE(merged->Valid()) << expected;
    ASSERT_EQ(ExtractUserKey(merged->key()), Key(expected));
    merged->Next();
  }
  EXPECT_FALSE(merged->Valid());
}

// And the mirror: forward a bit, reverse past the start, and switching back
// to forward must reproduce the same run instead of repeating or dropping
// what reverse already walked over.
TEST(MergingIterator, ForwardAfterReversingToTheStartResumesCleanly) {
  std::vector<std::unique_ptr<Iterator>> children;
  for (int c = 0; c < 3; ++c) {
    Entries entries;
    for (int i = c; i < 30; i += 3) {
      entries.emplace_back(EncodeInternalKey(Key(i), 1, ValueType::kValue),
                           "v" + std::to_string(i));
    }
    children.push_back(Child(std::move(entries)));
  }
  auto merged = NewMergingIterator(std::move(children));

  merged->SeekToFirst();
  for (int i = 0; i < 5; ++i) merged->Next();  // Walk to key 5.
  ASSERT_EQ(ExtractUserKey(merged->key()), Key(5));

  for (int i = 0; i < 6; ++i) merged->Prev();  // 5,4,3,2,1,0, then off the front.
  EXPECT_FALSE(merged->Valid());

  merged->SeekToFirst();  // Next()/Prev() require Valid(); re-seek to recover.
  int expected = 0;
  for (; merged->Valid(); merged->Next()) {
    ASSERT_EQ(ExtractUserKey(merged->key()), Key(expected)) << expected;
    ++expected;
  }
  EXPECT_EQ(expected, 30);
}

}  // namespace lsm
