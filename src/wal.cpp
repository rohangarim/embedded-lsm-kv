#include "lsm/wal.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <vector>

#include "lsm/coding.h"
#include "lsm/crc32c.h"

namespace lsm {
namespace {

constexpr size_t kHeaderSize = 8;  // crc32 + payload length

Status PosixError(const std::string& what) {
  return Status::IOError(what + ": " + std::strerror(errno));
}

// write() may return short. Loop until everything is out or we fail.
Status WriteFully(int fd, const char* data, size_t n) {
  while (n > 0) {
    const ssize_t written = ::write(fd, data, n);
    if (written < 0) {
      if (errno == EINTR) continue;
      return PosixError("write");
    }
    data += written;
    n -= static_cast<size_t>(written);
  }
  return Status::OK();
}

}  // namespace

WalWriter::~WalWriter() {
  if (fd_ >= 0) ::close(fd_);
}

Status WalWriter::Open(const std::string& path) {
  if (fd_ >= 0) return Status::InvalidArgument("WAL writer already open");
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0) return PosixError("open " + path);
  fd_ = fd;
  file_size_ = static_cast<uint64_t>(::lseek(fd_, 0, SEEK_END));
  return Status::OK();
}

Status WalWriter::Close() {
  if (fd_ < 0) return Status::OK();
  const int fd = fd_;
  fd_ = -1;
  if (::close(fd) != 0) return PosixError("close");
  return Status::OK();
}

Status WalWriter::AddRecord(SequenceNumber seq, ValueType type,
                            std::string_view key, std::string_view value) {
  if (fd_ < 0) return Status::InvalidArgument("WAL writer not open");

  std::string payload;
  payload.reserve(kTagSize + 8 + key.size() + value.size());
  PutFixed64(&payload, PackTag(seq, type));
  PutLengthPrefixed(&payload, key);
  PutLengthPrefixed(&payload, value);

  scratch_.clear();
  PutFixed32(&scratch_, crc32c::Value(payload.data(), payload.size()));
  PutFixed32(&scratch_, static_cast<uint32_t>(payload.size()));
  scratch_.append(payload);

  if (hook_) hook_(&scratch_);

  // One write() per record. The kernel appends atomically with respect to
  // other writers on this fd (O_APPEND), and a crash can only ever truncate
  // the tail, never interleave.
  const Status s = WriteFully(fd_, scratch_.data(), scratch_.size());
  if (!s.ok()) return s;
  file_size_ += scratch_.size();
  return Status::OK();
}

Status WalWriter::Sync() {
  if (fd_ < 0) return Status::InvalidArgument("WAL writer not open");
#if defined(__APPLE__)
  // On macOS fsync() only pushes to the drive, which may still hold the data in
  // a volatile write cache. F_FULLFSYNC is the call that actually survives
  // power loss. It is markedly slower, which is exactly the tradeoff being
  // measured.
  if (::fcntl(fd_, F_FULLFSYNC, 0) == -1) {
    if (::fsync(fd_) != 0) return PosixError("fsync");
  }
#else
  if (::fsync(fd_) != 0) return PosixError("fsync");
#endif
  return Status::OK();
}

Status WalReader::Replay(const std::string& path, const Handler& handler,
                         bool* saw_truncated_tail) {
  if (saw_truncated_tail != nullptr) *saw_truncated_tail = false;

  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    if (errno == ENOENT) return Status::OK();  // No log: nothing to replay.
    return PosixError("open " + path);
  }

  std::string buf;
  {
    // WAL files are bounded by the memtable size threshold, so slurping the
    // whole file is fine and keeps the parser simple.
    char chunk[1 << 16];
    while (true) {
      const ssize_t n = ::read(fd, chunk, sizeof(chunk));
      if (n < 0) {
        if (errno == EINTR) continue;
        const Status s = PosixError("read " + path);
        ::close(fd);
        return s;
      }
      if (n == 0) break;
      buf.append(chunk, static_cast<size_t>(n));
    }
  }
  ::close(fd);

  size_t offset = 0;
  while (offset < buf.size()) {
    if (buf.size() - offset < kHeaderSize) {
      if (saw_truncated_tail != nullptr) *saw_truncated_tail = true;
      break;  // Header itself was torn.
    }
    const uint32_t expected_crc = DecodeFixed32(buf.data() + offset);
    const uint32_t length = DecodeFixed32(buf.data() + offset + 4);
    if (buf.size() - offset - kHeaderSize < length) {
      if (saw_truncated_tail != nullptr) *saw_truncated_tail = true;
      break;  // Payload was torn.
    }
    const char* payload = buf.data() + offset + kHeaderSize;
    if (crc32c::Value(payload, length) != expected_crc) {
      // A bad CRC in the middle of a log would be real corruption, but we
      // cannot distinguish that from a torn tail without a per-record sequence
      // check -- and the tail is overwhelmingly the likelier cause. Stop here
      // and drop everything after; those writes were never acknowledged.
      if (saw_truncated_tail != nullptr) *saw_truncated_tail = true;
      break;
    }

    std::string_view rest(payload, length);
    if (rest.size() < kTagSize) return Status::Corruption("WAL record too short");
    const uint64_t tag = DecodeFixed64(rest.data());
    rest.remove_prefix(kTagSize);

    std::string_view key, value;
    if (!GetLengthPrefixed(&rest, &key) || !GetLengthPrefixed(&rest, &value)) {
      return Status::Corruption("WAL record has malformed key/value framing");
    }
    handler(tag >> 8, static_cast<ValueType>(tag & 0xffu), key, value);

    offset += kHeaderSize + length;
  }
  return Status::OK();
}

}  // namespace lsm
