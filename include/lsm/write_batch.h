#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "lsm/internal_key.h"
#include "lsm/status.h"

namespace lsm {

// A set of writes applied as a unit.
//
// Atomicity comes from the write-ahead log: the whole batch is one log record
// with one CRC, so a crash either leaves the record complete or leaves a
// fragment that replay discards entirely. There is no way to recover half a
// batch. Individual Put and Delete calls are batches of one, which is why they
// get the same guarantee for free.
//
// The batch is also the unit of sequence numbering: entries receive consecutive
// sequence numbers, so a later write in the same batch shadows an earlier one
// for the same key, exactly as two separate writes would.
class WriteBatch {
 public:
  WriteBatch() = default;

  void Put(std::string_view key, std::string_view value);
  void Delete(std::string_view key);
  void Clear();

  uint32_t Count() const { return count_; }
  bool empty() const { return count_ == 0; }
  // Encoded size, not counting the log record header the WAL adds.
  size_t ApproximateSize() const { return rep_.size(); }

  using Handler =
      std::function<void(ValueType, std::string_view key, std::string_view value)>;

  // Walks the batch in insertion order.
  Status ForEach(const Handler& handler) const {
    return Decode(rep_, count_, handler);
  }

  // Raw encoded entries, for the WAL to embed in a log record. Paired with
  // Count(), which is what tells a decoder how many to expect.
  std::string_view entries() const { return rep_; }

  // Entry encoding, shared with the WAL so both sides have one definition:
  //   [type:1][key_len:varint32][key][value_len:varint32][value]
  static void EncodeEntry(std::string* dst, ValueType type, std::string_view key,
                          std::string_view value);
  static Status Decode(std::string_view entries, uint32_t count,
                       const Handler& handler);

 private:
  friend class DB;

  std::string rep_;
  uint32_t count_ = 0;
};

}  // namespace lsm
