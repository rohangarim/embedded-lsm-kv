#include "lsm/memtable.h"

namespace lsm {

MemTable::MemTable() : table_(internal::EntryComparator(), &arena_) {}

void MemTable::Add(SequenceNumber seq, ValueType type, std::string_view key,
                   std::string_view value) {
  // Built directly into the entry buffer: going through a temporary
  // std::string for the internal key cost an extra allocation on every write.
  const LookupKeyBuffer internal_key(key, seq, type);
  std::string entry;
  entry.reserve(key.size() + value.size() + kTagSize + 8);
  PutLengthPrefixed(&entry, internal_key.key());
  PutLengthPrefixed(&entry, value);
  table_.Insert(entry);
}

void MemTable::Iterator::Seek(std::string_view target) {
  // The skip list compares entries, not bare keys, so wrap the target in the
  // same length-prefixed framing before seeking.
  seek_buf_.clear();
  PutLengthPrefixed(&seek_buf_, target);
  PutFixed32(&seek_buf_, 0);
  iter_.Seek(seek_buf_);
}

bool MemTable::Get(std::string_view key, SequenceNumber seq, std::string* value,
                   Status* status) const {
  Iterator iter(this);
  const LookupKeyBuffer lookup(key, seq);
  iter.Seek(lookup.key());
  if (!iter.Valid()) return false;

  // Seek landed on the first entry >= (key, seq). Because equal user keys sort
  // by descending sequence, that entry is the newest version visible at `seq`
  // -- but only if the user key actually matches; otherwise we ran past the end
  // of this key's versions.
  const std::string_view found = iter.internal_key();
  if (ExtractUserKey(found) != key) return false;

  if (ExtractValueType(found) == ValueType::kDeletion) {
    *status = Status::NotFound();
    return true;
  }
  const std::string_view v = iter.value();
  value->assign(v.data(), v.size());
  *status = Status::OK();
  return true;
}

}  // namespace lsm
