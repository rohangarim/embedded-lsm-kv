#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "lsm/internal_key.h"
#include "lsm/status.h"

namespace lsm {

// Write-ahead log.
//
// Record layout on disk:
//   [crc32c : 4][payload_length : 4][payload]
//   payload = [tag : 8][key_len : 4][key][value_len : 4][value]
//
// The CRC covers the payload only. A process killed mid-write leaves a short or
// mis-CRC'd record at the tail; the reader treats the first such record as the
// end of the log, which is correct because a torn record is by construction the
// last thing that was being written and was never acknowledged.
//
// The log is append-only and never read during normal operation -- only on
// recovery. That is what makes durability cheap: one sequential append per
// write instead of a random page update.
class WalWriter {
 public:
  WalWriter() = default;
  ~WalWriter();

  WalWriter(const WalWriter&) = delete;
  WalWriter& operator=(const WalWriter&) = delete;

  Status Open(const std::string& path);
  Status Close();

  // Appends one record. The bytes have reached the OS on return, but are not
  // on stable storage until Sync() -- see the SyncPolicy discussion in
  // options.h.
  Status AddRecord(SequenceNumber seq, ValueType type, std::string_view key,
                   std::string_view value);

  // fsync. Returns only once the device reports the data durable.
  Status Sync();

  uint64_t FileSize() const { return file_size_; }
  bool IsOpen() const { return fd_ >= 0; }

  // Test hook: invoked with the fully-encoded record just before write(). A
  // handler may truncate the record or _exit() to simulate a torn write.
  using WriteHook = std::function<void(std::string* record)>;
  void SetWriteHook(WriteHook hook) { hook_ = std::move(hook); }

 private:
  int fd_ = -1;
  uint64_t file_size_ = 0;
  std::string scratch_;
  WriteHook hook_;
};

// Replays a WAL file. Stops cleanly at the first truncated or bad-CRC record.
class WalReader {
 public:
  using Handler = std::function<void(SequenceNumber, ValueType, std::string_view,
                                     std::string_view)>;

  // `*saw_truncated_tail` (optional) reports whether replay stopped early
  // because of a partial record, which is the expected shape of a crash.
  static Status Replay(const std::string& path, const Handler& handler,
                       bool* saw_truncated_tail = nullptr);
};

}  // namespace lsm
