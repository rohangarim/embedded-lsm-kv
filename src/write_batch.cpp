#include "lsm/write_batch.h"

#include "lsm/coding.h"

namespace lsm {

void WriteBatch::EncodeEntry(std::string* dst, ValueType type,
                             std::string_view key, std::string_view value) {
  dst->push_back(static_cast<char>(type));
  PutVarint32(dst, static_cast<uint32_t>(key.size()));
  if (!key.empty()) dst->append(key.data(), key.size());
  PutVarint32(dst, static_cast<uint32_t>(value.size()));
  if (!value.empty()) dst->append(value.data(), value.size());
}

void WriteBatch::Put(std::string_view key, std::string_view value) {
  EncodeEntry(&rep_, ValueType::kValue, key, value);
  ++count_;
}

void WriteBatch::Delete(std::string_view key) {
  // A tombstone carries no value; the empty string keeps the encoding uniform.
  EncodeEntry(&rep_, ValueType::kDeletion, key, std::string_view());
  ++count_;
}

void WriteBatch::Clear() {
  rep_.clear();
  count_ = 0;
}

Status WriteBatch::Decode(std::string_view entries, uint32_t count,
                          const Handler& handler) {
  const char* p = entries.data();
  const char* limit = p + entries.size();

  for (uint32_t i = 0; i < count; ++i) {
    if (p >= limit) return Status::Corruption("write batch ended early");
    const auto type = static_cast<ValueType>(static_cast<uint8_t>(*p++));
    if (type != ValueType::kValue && type != ValueType::kDeletion) {
      return Status::Corruption("write batch has an unknown entry type");
    }

    uint32_t key_len = 0, value_len = 0;
    p = GetVarint32Ptr(p, limit, &key_len);
    if (p == nullptr || static_cast<size_t>(limit - p) < key_len) {
      return Status::Corruption("write batch has a truncated key");
    }
    const std::string_view key(p, key_len);
    p += key_len;

    p = GetVarint32Ptr(p, limit, &value_len);
    if (p == nullptr || static_cast<size_t>(limit - p) < value_len) {
      return Status::Corruption("write batch has a truncated value");
    }
    const std::string_view value(p, value_len);
    p += value_len;

    handler(type, key, value);
  }
  // A batch whose count disagrees with its bytes means the two were written by
  // different code paths, which is worth failing loudly rather than ignoring.
  if (p != limit) return Status::Corruption("write batch has trailing bytes");
  return Status::OK();
}

}  // namespace lsm
