#include "lsm/sstable.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstring>

#include "lsm/coding.h"
#include "lsm/crc32c.h"

namespace lsm {
namespace {

Status PosixError(const std::string& what) {
  return Status::IOError(what + ": " + std::strerror(errno));
}

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

Status ReadFullyAt(int fd, uint64_t offset, size_t n, std::string* out) {
  out->resize(n);
  size_t done = 0;
  while (done < n) {
    const ssize_t r = ::pread(fd, &(*out)[done], n - done,
                              static_cast<off_t>(offset + done));
    if (r < 0) {
      if (errno == EINTR) continue;
      return PosixError("pread");
    }
    if (r == 0) return Status::Corruption("unexpected end of SSTable");
    done += static_cast<size_t>(r);
  }
  return Status::OK();
}

Status SyncFd(int fd) {
#if defined(__APPLE__)
  if (::fcntl(fd, F_FULLFSYNC, 0) == -1) {
    if (::fsync(fd) != 0) return PosixError("fsync");
  }
#else
  if (::fsync(fd) != 0) return PosixError("fsync");
#endif
  return Status::OK();
}

}  // namespace

struct Table::Stats {
  std::atomic<uint64_t> blocks_read{0};
  std::atomic<uint64_t> bloom_rejections{0};
  std::atomic<uint64_t> cache_hits{0};
};

// ---------------------------------------------------------------- TableBuilder

TableBuilder::TableBuilder(const Options& options) : options_(options) {}

TableBuilder::~TableBuilder() {
  if (fd_ >= 0 && !finished_) Abandon();
}

Status TableBuilder::Open(const std::string& path) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return PosixError("open " + path);
  fd_ = fd;
  path_ = path;
  return Status::OK();
}

Status TableBuilder::Add(std::string_view internal_key, std::string_view value) {
  if (fd_ < 0) return Status::InvalidArgument("TableBuilder not open");
  assert(last_key_in_block_.empty() ||
         CompareInternalKey(last_key_in_block_, internal_key) < 0);

  if (num_entries_ == 0) smallest_key_.assign(internal_key);
  largest_key_.assign(internal_key);
  last_key_in_block_.assign(internal_key);
  ++num_entries_;

  filter_keys_.emplace_back(ExtractUserKey(internal_key));

  PutLengthPrefixed(&data_block_, internal_key);
  PutLengthPrefixed(&data_block_, value);

  // Cut the block once it is at least a page. Overshooting by one entry is
  // deliberate: splitting an entry across blocks would force two reads for one
  // lookup.
  if (data_block_.size() >= options_.block_size_bytes) return FlushDataBlock();
  return Status::OK();
}

Status TableBuilder::WriteBlock(const std::string& contents, uint64_t* offset,
                                uint64_t* size) {
  *offset = file_size_;
  *size = contents.size();

  Status s = WriteFully(fd_, contents.data(), contents.size());
  if (!s.ok()) return s;

  std::string trailer;
  PutFixed32(&trailer, crc32c::Value(contents.data(), contents.size()));
  s = WriteFully(fd_, trailer.data(), trailer.size());
  if (!s.ok()) return s;

  file_size_ += contents.size() + kBlockTrailerSize;
  return Status::OK();
}

Status TableBuilder::FlushDataBlock() {
  if (data_block_.empty()) return Status::OK();

  uint64_t offset = 0, size = 0;
  const Status s = WriteBlock(data_block_, &offset, &size);
  if (!s.ok()) return s;

  // One index entry per block, keyed by the block's largest key: a binary
  // search for the first entry whose last_key >= target lands on the only block
  // that can contain the target.
  PutLengthPrefixed(&index_block_, last_key_in_block_);
  PutFixed64(&index_block_, offset);
  PutFixed64(&index_block_, size);

  data_block_.clear();
  return Status::OK();
}

Status TableBuilder::Finish() {
  if (fd_ < 0) return Status::InvalidArgument("TableBuilder not open");

  Status s = FlushDataBlock();
  if (!s.ok()) return s;

  const std::string bloom = BloomFilter::Build(filter_keys_, options_.bloom_bits_per_key);
  uint64_t bloom_offset = 0, bloom_size = 0;
  s = WriteBlock(bloom, &bloom_offset, &bloom_size);
  if (!s.ok()) return s;

  uint64_t index_offset = 0, index_size = 0;
  s = WriteBlock(index_block_, &index_offset, &index_size);
  if (!s.ok()) return s;

  std::string footer;
  PutFixed64(&footer, index_offset);
  PutFixed64(&footer, index_size);
  PutFixed64(&footer, bloom_offset);
  PutFixed64(&footer, bloom_size);
  PutFixed64(&footer, kTableMagic);
  assert(footer.size() == kFooterSize);
  s = WriteFully(fd_, footer.data(), footer.size());
  if (!s.ok()) return s;
  file_size_ += footer.size();

  // The magic goes out in the same write as the rest of the footer, so a table
  // is either fully readable or is rejected at open. fsync before we let the
  // manifest reference it.
  s = SyncFd(fd_);
  if (!s.ok()) return s;

  finished_ = true;
  if (::close(fd_) != 0) {
    fd_ = -1;
    return PosixError("close");
  }
  fd_ = -1;
  return Status::OK();
}

void TableBuilder::Abandon() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  if (!path_.empty() && !finished_) ::unlink(path_.c_str());
}

// ----------------------------------------------------------------------- Table

Status Table::Open(const std::string& path, const Options& options,
                   std::unique_ptr<Table>* out,
                   std::shared_ptr<BlockCache> cache, uint64_t file_number) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return PosixError("open " + path);

  struct stat st;
  if (::fstat(fd, &st) != 0) {
    const Status s = PosixError("fstat " + path);
    ::close(fd);
    return s;
  }
  const uint64_t file_size = static_cast<uint64_t>(st.st_size);
  if (file_size < kFooterSize) {
    ::close(fd);
    return Status::Corruption(path + ": file shorter than footer");
  }

  std::unique_ptr<Table> table(new Table());
  table->path_ = path;
  table->fd_ = fd;
  table->file_size_ = file_size;
  table->verify_checksums_ = options.verify_checksums;
  table->cache_ = std::move(cache);
  table->file_number_ = file_number;
  table->stats_ = std::make_unique<Stats>();

  std::string footer;
  Status s = ReadFullyAt(fd, file_size - kFooterSize, kFooterSize, &footer);
  if (!s.ok()) return s;

  const uint64_t index_offset = DecodeFixed64(footer.data());
  const uint64_t index_size = DecodeFixed64(footer.data() + 8);
  const uint64_t bloom_offset = DecodeFixed64(footer.data() + 16);
  const uint64_t bloom_size = DecodeFixed64(footer.data() + 24);
  if (DecodeFixed64(footer.data() + 32) != kTableMagic) {
    return Status::Corruption(path + ": bad table magic");
  }
  if (index_offset + index_size + kBlockTrailerSize > file_size ||
      bloom_offset + bloom_size + kBlockTrailerSize > file_size) {
    return Status::Corruption(path + ": footer points past end of file");
  }

  // The index and the bloom filter are read once and held for the life of the
  // table -- they are the two structures every lookup touches. They bypass the
  // block cache precisely because they are already retained here; caching them
  // would just take capacity away from data blocks.
  BlockCache::Handle bloom_contents;
  s = table->ReadBlock(IndexEntry{"", bloom_offset, bloom_size}, &bloom_contents,
                       /*cacheable=*/false);
  if (!s.ok()) return s;
  table->bloom_ = BloomFilter(*bloom_contents);

  BlockCache::Handle index_contents;
  s = table->ReadBlock(IndexEntry{"", index_offset, index_size}, &index_contents,
                       /*cacheable=*/false);
  if (!s.ok()) return s;

  std::string_view rest(*index_contents);
  while (!rest.empty()) {
    std::string_view last_key;
    if (!GetLengthPrefixed(&rest, &last_key) || rest.size() < 16) {
      return Status::Corruption(path + ": malformed index block");
    }
    IndexEntry entry;
    entry.last_key.assign(last_key);
    entry.offset = DecodeFixed64(rest.data());
    entry.size = DecodeFixed64(rest.data() + 8);
    rest.remove_prefix(16);
    table->index_.push_back(std::move(entry));
  }

  // Reads of data blocks are counted; the index and bloom loads above are not
  // part of the per-lookup cost being measured.
  table->stats_->blocks_read.store(0, std::memory_order_relaxed);

  *out = std::move(table);
  return Status::OK();
}

Table::~Table() {
  if (fd_ >= 0) ::close(fd_);
}

uint64_t Table::blocks_read() const {
  return stats_->blocks_read.load(std::memory_order_relaxed);
}

uint64_t Table::bloom_rejections() const {
  return stats_->bloom_rejections.load(std::memory_order_relaxed);
}

uint64_t Table::block_cache_hits() const {
  return stats_->cache_hits.load(std::memory_order_relaxed);
}

Status Table::ReadBlock(const IndexEntry& entry, BlockCache::Handle* out,
                        bool cacheable) const {
  const bool use_cache = cacheable && cache_ != nullptr;
  if (use_cache) {
    if (BlockCache::Handle cached = cache_->Lookup(file_number_, entry.offset)) {
      stats_->cache_hits.fetch_add(1, std::memory_order_relaxed);
      *out = std::move(cached);
      return Status::OK();
    }
  }

  std::string buf;
  const Status s =
      ReadFullyAt(fd_, entry.offset, entry.size + kBlockTrailerSize, &buf);
  if (!s.ok()) return s;

  const uint32_t stored = DecodeFixed32(buf.data() + entry.size);
  if (verify_checksums_ && crc32c::Value(buf.data(), entry.size) != stored) {
    return Status::Corruption(path_ + ": block checksum mismatch");
  }
  buf.resize(entry.size);
  stats_->blocks_read.fetch_add(1, std::memory_order_relaxed);

  if (use_cache) {
    *out = cache_->Insert(file_number_, entry.offset, std::move(buf));
  } else {
    *out = std::make_shared<const std::string>(std::move(buf));
  }
  return Status::OK();
}

bool Table::Get(std::string_view key, SequenceNumber seq, std::string* value,
                Status* status) const {
  // The filter is checked first because a negative answer means we skip the
  // index search and the block read entirely -- the whole point of carrying it.
  if (!bloom_.MayContain(key)) {
    stats_->bloom_rejections.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  const LookupKeyBuffer lookup(key, seq);
  const std::string_view target = lookup.key();

  // First block whose largest key is >= target. Sparse index: this is the only
  // block that can hold the key.
  size_t lo = 0, hi = index_.size();
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (CompareInternalKey(index_[mid].last_key, target) < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo >= index_.size()) return false;  // Past the end of the table.

  BlockCache::Handle contents;
  const Status s = ReadBlock(index_[lo], &contents);
  if (!s.ok()) {
    *status = s;
    return true;  // Surface the I/O error rather than silently reading past it.
  }

  std::string_view rest(*contents);
  while (!rest.empty()) {
    std::string_view ikey, val;
    if (!GetLengthPrefixed(&rest, &ikey) || !GetLengthPrefixed(&rest, &val)) {
      *status = Status::Corruption(path_ + ": malformed data block");
      return true;
    }
    if (CompareInternalKey(ikey, target) < 0) continue;  // Older or smaller.
    if (ExtractUserKey(ikey) != key) return false;       // Ran past the key.

    if (ExtractValueType(ikey) == ValueType::kDeletion) {
      *status = Status::NotFound();
    } else {
      value->assign(val.data(), val.size());
      *status = Status::OK();
    }
    return true;
  }
  return false;
}

// ------------------------------------------------------------- TableIterator

class TableIterator : public Iterator {
 public:
  explicit TableIterator(const Table* table) : table_(table) {}

  bool Valid() const override { return valid_; }

  void SeekToFirst() override {
    block_index_ = 0;
    LoadBlock();  // Also parses the block's first entry.
  }

  void Seek(std::string_view target) override {
    const auto& index = table_->index_;
    size_t lo = 0, hi = index.size();
    while (lo < hi) {
      const size_t mid = lo + (hi - lo) / 2;
      if (CompareInternalKey(index[mid].last_key, target) < 0) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    block_index_ = lo;
    if (!LoadBlock()) return;
    while (valid_ && CompareInternalKey(key_, target) < 0) Next();
  }

  void Next() override {
    if (!valid_) return;
    if (rest_.empty()) {
      ++block_index_;
      if (!LoadBlock()) return;
      return;
    }
    ParseCurrent();
  }

  std::string_view key() const override { return key_; }
  std::string_view value() const override { return value_; }
  Status status() const override { return status_; }

 private:
  // Loads block_index_, skipping empty blocks; sets valid_ and the first entry.
  bool LoadBlock() {
    while (block_index_ < table_->index_.size()) {
      status_ = table_->ReadBlock(table_->index_[block_index_], &block_);
      if (!status_.ok()) {
        valid_ = false;
        return false;
      }
      rest_ = std::string_view(*block_);
      if (!rest_.empty()) {
        ParseCurrent();
        return valid_;
      }
      ++block_index_;
    }
    valid_ = false;
    return false;
  }

  void ParseCurrent() {
    std::string_view k, v;
    if (!GetLengthPrefixed(&rest_, &k) || !GetLengthPrefixed(&rest_, &v)) {
      status_ = Status::Corruption(table_->path() + ": malformed data block");
      valid_ = false;
      return;
    }
    key_ = k;
    value_ = v;
    valid_ = true;
  }

  const Table* table_;
  size_t block_index_ = 0;
  // Keeps the bytes key_/value_ point into alive, even if the cache evicts the
  // block while this cursor is still walking it.
  BlockCache::Handle block_;
  std::string_view rest_;     // Unparsed remainder of block_.
  std::string_view key_;
  std::string_view value_;
  bool valid_ = false;
  Status status_;
};

std::unique_ptr<Iterator> Table::NewIterator() const {
  return std::make_unique<TableIterator>(this);
}

}  // namespace lsm
