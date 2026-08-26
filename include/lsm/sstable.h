#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "lsm/block.h"
#include "lsm/bloom.h"
#include "lsm/cache.h"
#include "lsm/internal_key.h"
#include "lsm/iterator.h"
#include "lsm/options.h"
#include "lsm/status.h"

namespace lsm {

// On-disk layout of an SSTable:
//
//   [data block 0]
//   [data block 1]
//   ...
//   [bloom block]
//   [index block]
//   [footer : 40 bytes, fixed]
//
// Every block is followed by a 1-byte compression type and a 4-byte CRC. The
// CRC covers the stored bytes and the type together, so a block cannot be
// decompressed the wrong way without being detected.
//
// Data block contents: prefix-compressed entries followed by a restart array.
//
//   entry:   [shared:varint32][non_shared:varint32][value_len:varint32]
//            [key suffix bytes][value bytes]
//   trailer: [restart_0:u32] ... [restart_k:u32][num_restarts:u32]
//
// Keys arrive sorted, so consecutive ones share long prefixes -- an entry
// stores only the bytes that differ from its predecessor. Every
// block_restart_interval-th entry resets to a whole key and records its offset
// in the restart array. Those restart points are what keep the block
// searchable: a lookup binary-searches them and then decodes forward through at
// most one interval, instead of walking the block from byte zero.
//
// Blocks exist because storage is addressed in pages: a point lookup should
// transfer one ~4 KiB chunk, not the whole file and not a byte at a time.
//
// Index block contents: one [key_len:4][last key in block][offset:8][size:8]
// entry per data block. The index is *sparse* -- one entry per block, not per
// key -- so it stays small enough to keep resident, and a lookup is a binary
// search in memory followed by exactly one block read.
//
// Footer: index_offset, index_size, bloom_offset, bloom_size, magic.
constexpr size_t kFooterSize = 40;
// Bumped when the data block format gained restart points and prefix
// compression. An older file now fails at open with a clear corruption error
// rather than being decoded as garbage.
constexpr uint64_t kTableMagic = 0x4c534d5442aa5503ull;
constexpr size_t kBlockTrailerSize = 5;  // compression type + crc32c

// Immutable-file writer. Keys must be added in ascending internal-key order.
class TableBuilder {
 public:
  TableBuilder(const Options& options);
  ~TableBuilder();

  TableBuilder(const TableBuilder&) = delete;
  TableBuilder& operator=(const TableBuilder&) = delete;

  Status Open(const std::string& path);

  // Requires internal_key > every previously added key.
  Status Add(std::string_view internal_key, std::string_view value);

  // Writes the bloom block, index block and footer, then fsyncs and closes.
  Status Finish();

  // Closes and unlinks the partial file.
  void Abandon();

  uint64_t NumEntries() const { return num_entries_; }
  uint64_t FileSize() const { return file_size_; }
  const std::string& SmallestKey() const { return smallest_key_; }
  const std::string& LargestKey() const { return largest_key_; }

 private:
  Status FlushDataBlock();
  // Appends contents + trailer, returns the handle of the written block.
  // `compressible` is false for the index and Bloom blocks.
  Status WriteBlock(const std::string& contents, bool compressible,
                    uint64_t* offset, uint64_t* size);
  std::string compress_buf_;

  Options options_;
  std::string path_;
  int fd_ = -1;
  bool finished_ = false;

  BlockBuilder block_;       // Data block under construction.
  std::string index_block_;  // Accumulated index entries.
  std::string last_key_in_block_;

  std::vector<std::string> filter_keys_;  // User keys, for the bloom filter.

  uint64_t file_size_ = 0;
  uint64_t num_entries_ = 0;
  std::string smallest_key_;
  std::string largest_key_;
};

// Read-only handle on a finished SSTable. Index and bloom filter are loaded
// once at open; data blocks are read on demand.
class Table {
 public:
  // `cache` and `file_number` may be omitted; without a cache every block read
  // goes to the filesystem. The file number is the cache's namespace, so two
  // tables sharing a cache must never share one.
  static Status Open(const std::string& path, const Options& options,
                     std::unique_ptr<Table>* table,
                     std::shared_ptr<BlockCache> cache = nullptr,
                     uint64_t file_number = 0);

  ~Table();

  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;

  // Newest version of `key` at or below `seq`.
  //   - returns true  -> resolved here; *status is OK (value set) or NotFound
  //                      (tombstone)
  //   - returns false -> not in this table; keep looking
  bool Get(std::string_view key, SequenceNumber seq, std::string* value,
           Status* status) const;

  std::unique_ptr<Iterator> NewIterator() const;

  const std::string& path() const { return path_; }
  uint64_t file_size() const { return file_size_; }
  size_t num_data_blocks() const { return index_.size(); }
  const BloomFilter& bloom() const { return bloom_; }

  // Counters for the benchmark's read-amplification story. blocks_read counts
  // blocks actually fetched from the filesystem, so cache hits do not inflate
  // it.
  uint64_t blocks_read() const;
  uint64_t bloom_rejections() const;
  uint64_t block_cache_hits() const;

 private:
  friend class TableIterator;

  // Deliberately a POD with the key held out of line, rather than an inline
  // std::string. A block index runs to thousands of entries and every lookup
  // binary-searches it, so each comparison used to chase a pointer into a
  // separate heap allocation -- an internal key is ~28 bytes, past libc++'s
  // small-string threshold. That is a likely cache miss per comparison, ten
  // comparisons per file, several files per seek. Keys now live end to end in
  // one buffer and the entries themselves pack densely.
  struct IndexEntry {
    uint32_t key_offset;  // Into index_keys_.
    uint32_t key_length;
    uint64_t offset;
    uint64_t size;  // Contents size, excluding the CRC trailer.
  };

  // Largest internal key in block `i`.
  std::string_view IndexKey(size_t i) const {
    return std::string_view(index_keys_.data() + index_[i].key_offset,
                            index_[i].key_length);
  }

  Table() = default;

  // Returns a handle rather than a copy so a cached block is shared, not
  // duplicated, and stays alive for as long as the caller holds the handle.
  // `cacheable` is false for the index and bloom blocks, which are read once at
  // open and retained for the life of the table anyway.
  Status ReadBlock(uint64_t offset, uint64_t size, BlockCache::Handle* out,
                   bool cacheable = true) const;

  std::string path_;
  int fd_ = -1;
  uint64_t file_size_ = 0;
  bool verify_checksums_ = true;
  std::shared_ptr<BlockCache> cache_;
  uint64_t file_number_ = 0;
  std::vector<IndexEntry> index_;
  std::string index_keys_;  // Every index key, concatenated.
  BloomFilter bloom_;

  struct Stats;
  std::unique_ptr<Stats> stats_;
};

}  // namespace lsm
