#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "lsm/status.h"

namespace lsm {

// Uniform cursor over a sorted sequence of (internal key, value) pairs.
// MemTables, SSTables and merges of both expose this, which is what lets the
// compaction and scan paths be written once.
class Iterator {
 public:
  virtual ~Iterator() = default;

  virtual bool Valid() const = 0;
  virtual void SeekToFirst() = 0;
  virtual void Seek(std::string_view target) = 0;
  virtual void Next() = 0;

  // Valid() must be true.
  virtual std::string_view key() const = 0;
  virtual std::string_view value() const = 0;

  virtual Status status() const { return Status::OK(); }
};

// N-way merge. Yields entries in internal-key order across all children, which
// for equal user keys means newest-sequence-first. Ties between children on a
// byte-identical internal key are broken by child order, so callers must pass
// children newest-first.
//
// Implementation is a linear scan over children rather than a heap: compactions
// here merge a handful of files at a time, and the constant factor of a
// 4-element scan beats a heap's pointer chasing.
std::unique_ptr<Iterator> NewMergingIterator(
    std::vector<std::unique_ptr<Iterator>> children);

}  // namespace lsm
