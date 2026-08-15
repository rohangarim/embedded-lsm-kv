#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "lsm/arena.h"
#include "lsm/coding.h"
#include "lsm/internal_key.h"
#include "lsm/skiplist.h"
#include "lsm/status.h"

namespace lsm {

namespace internal {

// A memtable record is a single buffer: length-prefixed internal key followed
// by length-prefixed value. One allocation per record, and the comparator only
// ever decodes the first field.
inline std::string_view DecodeEntryKey(std::string_view entry) {
  std::string_view rest = entry;
  std::string_view key;
  GetLengthPrefixed(&rest, &key);
  return key;
}

inline std::string_view DecodeEntryValue(std::string_view entry) {
  std::string_view rest = entry;
  std::string_view key, value;
  GetLengthPrefixed(&rest, &key);
  GetLengthPrefixed(&rest, &value);
  return value;
}

struct EntryComparator {
  int operator()(std::string_view a, std::string_view b) const {
    return CompareInternalKey(DecodeEntryKey(a), DecodeEntryKey(b));
  }
};

}  // namespace internal

// The in-memory write buffer. Every Put and Delete lands here (after the WAL),
// so a write costs a sorted-structure insert and nothing else -- no disk seek,
// no page rewrite. That is the whole reason an LSM tree beats a B-tree on write
// throughput.
//
// A Delete inserts a tombstone rather than removing anything: the key may still
// exist in an older SSTable, and the tombstone is what shadows it until
// compaction can physically drop both.
class MemTable {
 public:
  MemTable();

  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  void Add(SequenceNumber seq, ValueType type, std::string_view key,
           std::string_view value);

  // Looks up the newest version of `key` at or below `seq`.
  //   - found, live value  -> returns true, *status = OK, *value set
  //   - found, tombstone   -> returns true, *status = NotFound
  //   - not in this table  -> returns false; the caller must look further down
  bool Get(std::string_view key, SequenceNumber seq, std::string* value,
           Status* status) const;

  size_t ApproximateMemoryUsage() const { return arena_.MemoryUsage(); }
  size_t NumEntries() const { return table_.size(); }
  bool empty() const { return table_.size() == 0; }

  // Forward iterator in internal-key order. Used by the flush path to build an
  // SSTable and by the read path for range scans.
  class Iterator {
   public:
    explicit Iterator(const MemTable* mem) : iter_(&mem->table_) {}

    bool Valid() const { return iter_.Valid(); }
    void SeekToFirst() { iter_.SeekToFirst(); }
    void Next() { iter_.Next(); }
    // `target` is an internal key, not a user key.
    void Seek(std::string_view target);

    std::string_view internal_key() const {
      return internal::DecodeEntryKey(iter_.key());
    }
    std::string_view value() const {
      return internal::DecodeEntryValue(iter_.key());
    }

   private:
    SkipList<internal::EntryComparator>::Iterator iter_;
    std::string seek_buf_;
  };

 private:
  Arena arena_;
  SkipList<internal::EntryComparator> table_;
};

}  // namespace lsm
