#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "lsm/internal_key.h"
#include "lsm/write_batch.h"
#include "lsm/status.h"

namespace lsm {

// Write-ahead log.
//
// File layout:
//   [magic : 7]["LSMWAL"][format version : 1]
//   then a sequence of records:
//     [crc32c : 4][payload_length : 4][payload]
//     payload = [base sequence : 8][entry count : 4][entries]
//
// One record holds an entire write batch, which is what makes a batch atomic:
// the CRC covers all of it, so a crash leaves the batch whole or leaves a
// fragment the reader discards. There is no encoding for half a batch.
//
// A process killed mid-write leaves a short or mis-CRC'd record at the tail;
// the reader treats the first such record as the end of the log, which is
// correct because a torn record is by construction the last thing that was
// being written and was never acknowledged.
//
// The header exists so a log written by a build with a different record format
// is rejected outright rather than decoded into plausible-looking garbage.
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

  // Appends a batch as one record. Entries receive consecutive sequence
  // numbers starting at `seq`. The bytes have reached the OS on return, but are
  // not on stable storage until Sync() -- see the SyncPolicy discussion in
  // options.h.
  Status AddRecord(SequenceNumber seq, const WriteBatch& batch);

  // Convenience for a single write, which is just a batch of one.
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
  // Called once per entry, with the sequence number that entry was assigned.
  // Batches are expanded, so recovery does not need to know they existed.
  using Handler = std::function<void(SequenceNumber, ValueType, std::string_view,
                                     std::string_view)>;

  // `*saw_truncated_tail` (optional) reports whether replay stopped early
  // because of a partial record, which is the expected shape of a crash.
  static Status Replay(const std::string& path, const Handler& handler,
                       bool* saw_truncated_tail = nullptr);
};

}  // namespace lsm
