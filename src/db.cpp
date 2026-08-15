#include "lsm/db.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <set>
#include <sstream>

#include "lsm/coding.h"
#include "lsm/crc32c.h"

namespace lsm {
namespace {

// "MANIFEST" in ASCII, little-endian.
constexpr uint64_t kManifestMagic = 0x54534546494e414dull;

Status PosixError(const std::string& what) {
  return Status::IOError(what + ": " + std::strerror(errno));
}

std::string NumberedFile(const std::string& dir, uint64_t number,
                         const char* suffix) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "/%06llu%s",
                static_cast<unsigned long long>(number), suffix);
  return dir + buf;
}

// Parses "000123.sst" / "000123.log". Returns false for anything else.
bool ParseFileName(const std::string& name, uint64_t* number, std::string* kind) {
  const size_t dot = name.rfind('.');
  if (dot == std::string::npos || dot == 0) return false;
  for (size_t i = 0; i < dot; ++i) {
    if (name[i] < '0' || name[i] > '9') return false;
  }
  *number = std::strtoull(name.substr(0, dot).c_str(), nullptr, 10);
  *kind = name.substr(dot + 1);
  return true;
}

// fsync a file descriptor, using the call that actually reaches stable storage
// on this platform. On macOS plain fsync() only pushes to the drive, which may
// still hold the data in a volatile write cache; F_FULLFSYNC is what survives
// power loss, at a real cost in latency.
Status SyncFd(int fd, const std::string& what) {
#if defined(__APPLE__)
  if (::fcntl(fd, F_FULLFSYNC, 0) != -1) return Status::OK();
  // Not every filesystem implements F_FULLFSYNC; fall back rather than fail.
#endif
  if (::fsync(fd) != 0) return PosixError("fsync " + what);
  return Status::OK();
}

Status SyncDirectory(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return PosixError("open dir " + path);
  // Renaming a file is only durable once the *directory* entry is synced.
  // Skipping this is the classic way to lose a file that fsync said was safe.
  const Status s = SyncFd(fd, "dir " + path);
  ::close(fd);
  return s;
}

uint64_t MaxBytesForLevel(const Options& options, int level) {
  if (level <= 1) return options.max_bytes_for_level_base;
  uint64_t result = options.max_bytes_for_level_base;
  for (int i = 1; i < level; ++i) result *= 10;
  return result;
}

bool RangesOverlap(const FileMetaData& f, std::string_view smallest_user,
                   std::string_view largest_user) {
  if (ExtractUserKey(f.largest) < smallest_user) return false;
  if (ExtractUserKey(f.smallest) > largest_user) return false;
  return true;
}

}  // namespace

uint64_t Version::LevelBytes(int level) const {
  uint64_t total = 0;
  for (const auto& f : levels[level]) total += f->file_size;
  return total;
}

size_t Version::NumFiles() const {
  size_t n = 0;
  for (const auto& level : levels) n += level.size();
  return n;
}

double DbStats::WriteAmplification() const {
  if (user_bytes_written == 0) return 0.0;
  const double denom = static_cast<double>(user_bytes_written);
  return static_cast<double>(bytes_written_to_wal + flush_output_bytes +
                             compaction_output_bytes) /
         denom;
}

// -------------------------------------------------------------------- lifetime

DB::DB(const Options& options, std::string path)
    : options_(options),
      path_(std::move(path)),
      last_sync_time_(std::chrono::steady_clock::now()) {}

DB::~DB() {
  {
    std::unique_lock<std::mutex> lock(mu_);
    shutting_down_ = true;
    bg_cv_.notify_all();
  }
  if (bg_thread_.joinable()) bg_thread_.join();

  std::unique_lock<std::mutex> lock(mu_);
  if (log_) log_->Close();
}

std::string DB::TablePath(uint64_t number) const {
  return NumberedFile(path_, number, ".sst");
}
std::string DB::LogPath(uint64_t number) const {
  return NumberedFile(path_, number, ".log");
}
std::string DB::ManifestPath() const { return path_ + "/MANIFEST"; }

Status DB::Open(const Options& options, const std::string& path,
                std::unique_ptr<DB>* out) {
  struct stat st;
  const bool exists = ::stat(path.c_str(), &st) == 0;
  if (exists && options.error_if_exists) {
    return Status::InvalidArgument(path + " already exists");
  }
  if (!exists) {
    if (!options.create_if_missing) {
      return Status::NotFound(path + " does not exist");
    }
    if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
      return PosixError("mkdir " + path);
    }
  }

  std::unique_ptr<DB> db(new DB(options, path));
  const Status s = db->Recover();
  if (!s.ok()) return s;

  if (options.background_compaction) {
    db->bg_thread_ = std::thread([raw = db.get()] { raw->BackgroundLoop(); });
  }
  *out = std::move(db);
  return Status::OK();
}

// ------------------------------------------------------------------- manifest

uint64_t DB::OldestLiveLog() const {
  // While a flush is outstanding, imm_'s records exist only in memory and in
  // the log it was written to. Naming a newer log in the manifest would let
  // RemoveObsoleteFiles delete that log, and a crash would then lose every
  // acknowledged write still sitting in imm_.
  return imm_ != nullptr ? imm_log_number_ : log_number_;
}

Status DB::WriteManifest(const Version& version, uint64_t log_number) {
  std::string body;
  PutFixed64(&body, kManifestMagic);
  PutFixed64(&body, next_file_number_);
  PutFixed64(&body, last_sequence_);
  PutFixed64(&body, log_number);

  uint32_t num_files = 0;
  for (const auto& level : version.levels) num_files += level.size();
  PutFixed32(&body, num_files);

  for (int level = 0; level < kNumLevels; ++level) {
    for (const auto& f : version.levels[level]) {
      PutFixed32(&body, static_cast<uint32_t>(level));
      PutFixed64(&body, f->number);
      PutFixed64(&body, f->file_size);
      PutLengthPrefixed(&body, f->smallest);
      PutLengthPrefixed(&body, f->largest);
    }
  }
  PutFixed32(&body, crc32c::Value(body.data(), body.size()));

  // Write-to-temp then rename: rename() is atomic, so a crash leaves either the
  // old manifest or the new one, never a half-written list of live files. This
  // is the commit point of a flush or compaction -- the SSTables are already on
  // disk and fsynced before we get here.
  const std::string tmp = ManifestPath() + ".tmp";
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return PosixError("open " + tmp);

  size_t written = 0;
  while (written < body.size()) {
    const ssize_t n = ::write(fd, body.data() + written, body.size() - written);
    if (n < 0) {
      if (errno == EINTR) continue;
      const Status s = PosixError("write manifest");
      ::close(fd);
      return s;
    }
    written += static_cast<size_t>(n);
  }
  {
    const Status sync = SyncFd(fd, "manifest");
    if (!sync.ok()) {
      ::close(fd);
      return sync;
    }
  }
  ::close(fd);

  if (::rename(tmp.c_str(), ManifestPath().c_str()) != 0) {
    return PosixError("rename manifest");
  }
  return SyncDirectory(path_);
}

Status DB::LoadManifest(uint64_t* log_number) {
  *log_number = 0;
  const int fd = ::open(ManifestPath().c_str(), O_RDONLY);
  if (fd < 0) {
    if (errno == ENOENT) {
      // Fresh database.
      current_ = std::make_shared<Version>();
      return Status::OK();
    }
    return PosixError("open manifest");
  }

  std::string body;
  char chunk[1 << 16];
  while (true) {
    const ssize_t n = ::read(fd, chunk, sizeof(chunk));
    if (n < 0) {
      if (errno == EINTR) continue;
      const Status s = PosixError("read manifest");
      ::close(fd);
      return s;
    }
    if (n == 0) break;
    body.append(chunk, static_cast<size_t>(n));
  }
  ::close(fd);

  if (body.size() < 36) return Status::Corruption("manifest too short");
  const size_t body_len = body.size() - 4;
  if (crc32c::Value(body.data(), body_len) != DecodeFixed32(body.data() + body_len)) {
    return Status::Corruption("manifest checksum mismatch");
  }

  std::string_view rest(body.data(), body_len);
  if (DecodeFixed64(rest.data()) != kManifestMagic) {
    return Status::Corruption("bad manifest magic");
  }
  next_file_number_ = DecodeFixed64(rest.data() + 8);
  last_sequence_ = DecodeFixed64(rest.data() + 16);
  *log_number = DecodeFixed64(rest.data() + 24);
  const uint32_t num_files = DecodeFixed32(rest.data() + 32);
  rest.remove_prefix(36);

  auto version = std::make_shared<Version>();
  for (uint32_t i = 0; i < num_files; ++i) {
    if (rest.size() < 20) return Status::Corruption("truncated manifest entry");
    const uint32_t level = DecodeFixed32(rest.data());
    auto meta = std::make_shared<FileMetaData>();
    meta->number = DecodeFixed64(rest.data() + 4);
    meta->file_size = DecodeFixed64(rest.data() + 12);
    rest.remove_prefix(20);

    std::string_view smallest, largest;
    if (!GetLengthPrefixed(&rest, &smallest) ||
        !GetLengthPrefixed(&rest, &largest)) {
      return Status::Corruption("truncated manifest key");
    }
    meta->smallest.assign(smallest);
    meta->largest.assign(largest);
    if (level >= kNumLevels) return Status::Corruption("manifest level out of range");

    std::unique_ptr<Table> table;
    const Status s = Table::Open(TablePath(meta->number), options_, &table);
    if (!s.ok()) return s;
    meta->table = std::shared_ptr<Table>(table.release());
    version->levels[level].push_back(std::move(meta));
  }

  // L0 is kept newest-first so a lookup finds the most recent version first.
  std::sort(version->levels[0].begin(), version->levels[0].end(),
            [](const auto& a, const auto& b) { return a->number > b->number; });
  for (int level = 1; level < kNumLevels; ++level) {
    std::sort(version->levels[level].begin(), version->levels[level].end(),
              [](const auto& a, const auto& b) {
                return CompareInternalKey(a->smallest, b->smallest) < 0;
              });
  }
  current_ = version;
  return Status::OK();
}

// ------------------------------------------------------------------- recovery

Status DB::Recover() {
  std::unique_lock<std::mutex> lock(mu_);

  uint64_t manifest_log_number = 0;
  Status s = LoadManifest(&manifest_log_number);
  if (!s.ok()) return s;

  // Replay every log at or after the one the manifest committed against. There
  // are at most two: the active log, plus one that was still being flushed when
  // we died.
  std::vector<uint64_t> logs;
  if (DIR* dir = ::opendir(path_.c_str())) {
    while (struct dirent* entry = ::readdir(dir)) {
      uint64_t number;
      std::string kind;
      if (!ParseFileName(entry->d_name, &number, &kind)) continue;
      if (kind == "log" && number >= manifest_log_number) logs.push_back(number);
      if (number >= next_file_number_) next_file_number_ = number + 1;
    }
    ::closedir(dir);
  }
  std::sort(logs.begin(), logs.end());

  mem_ = std::make_shared<MemTable>();
  SequenceNumber max_seq = last_sequence_;
  for (const uint64_t number : logs) {
    bool truncated = false;
    s = WalReader::Replay(
        LogPath(number),
        [&](SequenceNumber seq, ValueType type, std::string_view key,
            std::string_view value) {
          mem_->Add(seq, type, key, value);
          if (seq > max_seq) max_seq = seq;
        },
        &truncated);
    if (!s.ok()) return s;
  }
  last_sequence_ = max_seq;

  // Land the replayed data in an SSTable straight away so that the post-open
  // state is identical whether or not we crashed.
  if (!mem_->empty()) {
    imm_ = mem_;
    mem_ = std::make_shared<MemTable>();
    s = FlushImmutableMemTable(lock);
    if (!s.ok()) return s;
  }

  const uint64_t new_log = next_file_number_++;
  log_ = std::make_unique<WalWriter>();
  s = log_->Open(LogPath(new_log));
  if (!s.ok()) return s;
  log_number_ = new_log;

  s = WriteManifest(*current_, OldestLiveLog());
  if (!s.ok()) return s;

  RemoveObsoleteFiles(*current_, OldestLiveLog());
  return Status::OK();
}

// ---------------------------------------------------------------- write path

Status DB::Write(const WriteOptions& opts, ValueType type, std::string_view key,
                 std::string_view value) {
  std::unique_lock<std::mutex> lock(mu_);
  if (!bg_error_.ok()) return bg_error_;

  Status s = MakeRoomForWrite(lock);
  if (!s.ok()) return s;

  const SequenceNumber seq = ++last_sequence_;

  // WAL first, memtable second. If the order were reversed a crash could leave
  // a value readable in memory that was never logged -- and after recovery the
  // acknowledged write would simply be gone.
  s = log_->AddRecord(seq, type, key, value);
  if (!s.ok()) return s;
  wal_dirty_ = true;
  // Record overhead: 8-byte header (crc + length), 8-byte tag, two 4-byte
  // length prefixes.
  stats_.bytes_written_to_wal += key.size() + value.size() + 24;
  stats_.user_bytes_written += key.size() + value.size();

  // A failed fsync must not be swallowed: the caller would take an OK as a
  // durability guarantee the device never gave us.
  s = MaybeSyncWal(opts);
  if (!s.ok()) return s;

  mem_->Add(seq, type, key, value);
  ++stats_.writes;
  return Status::OK();
}

Status DB::MaybeSyncWal(const WriteOptions& opts) {
  const bool force = !opts.use_default_policy && opts.sync;
  switch (force ? SyncPolicy::kEveryWrite : options_.sync_policy) {
    case SyncPolicy::kEveryWrite: {
      // fsync tells the device to move the data out of any volatile cache and
      // does not return until it has. This is the only setting that survives
      // sudden power loss for every acknowledged write.
      const Status s = log_->Sync();
      if (!s.ok()) return s;
      ++stats_.wal_syncs;
      wal_dirty_ = false;
      break;
    }
    case SyncPolicy::kInterval: {
      const auto now = std::chrono::steady_clock::now();
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - last_sync_time_);
      if (static_cast<uint64_t>(elapsed.count()) >= options_.fsync_interval_ms) {
        const Status s = log_->Sync();
        if (!s.ok()) return s;
        ++stats_.wal_syncs;
        wal_dirty_ = false;
        last_sync_time_ = now;
      }
      break;
    }
    case SyncPolicy::kNever:
      // The bytes are in the page cache. A process crash (or SIGKILL) still
      // recovers everything, because the kernel owns them. A kernel panic or
      // power cut loses whatever had not been written back.
      break;
  }
  return Status::OK();
}

Status DB::Put(const WriteOptions& opts, std::string_view key,
               std::string_view value) {
  return Write(opts, ValueType::kValue, key, value);
}

Status DB::Delete(const WriteOptions& opts, std::string_view key) {
  return Write(opts, ValueType::kDeletion, key, std::string_view());
}

Status DB::MakeRoomForWrite(std::unique_lock<std::mutex>& lock) {
  while (true) {
    if (!bg_error_.ok()) return bg_error_;
    if (mem_->ApproximateMemoryUsage() <= options_.memtable_size_bytes) {
      return Status::OK();
    }
    if (imm_ != nullptr) {
      // A flush is already outstanding; writes have to wait rather than let an
      // unbounded number of memtables pile up in memory.
      if (options_.background_compaction) {
        bg_cv_.wait(lock);
        continue;
      }
      const Status s = DrainBackgroundWork(lock);
      if (!s.ok()) return s;
      continue;
    }

    // Rotate: the full memtable becomes immutable and gets its own log. The old
    // log stays on disk until the flush is committed to the manifest.
    const uint64_t new_log = next_file_number_++;
    auto new_writer = std::make_unique<WalWriter>();
    const Status s = new_writer->Open(LogPath(new_log));
    if (!s.ok()) return s;

    log_->Close();
    log_ = std::move(new_writer);
    imm_log_number_ = log_number_;  // imm_'s records live in the old log.
    log_number_ = new_log;
    imm_ = mem_;
    mem_ = std::make_shared<MemTable>();
    MaybeScheduleCompaction();
  }
}

// ----------------------------------------------------------------- read path

Status DB::Get(const ReadOptions& opts, std::string_view key, std::string* value) {
  std::shared_ptr<MemTable> mem, imm;
  std::shared_ptr<const Version> version;
  SequenceNumber seq;
  {
    std::lock_guard<std::mutex> guard(mu_);
    mem = mem_;
    imm = imm_;
    version = current_;
    seq = last_sequence_;
    ++stats_.gets;
  }
  (void)opts;

  // Newest source first: active memtable, the one being flushed, then L0
  // newest-file-first, then one file per deeper level. The first source that
  // knows about the key is authoritative -- including when what it knows is a
  // tombstone.
  Status s;
  if (mem->Get(key, seq, value, &s)) {
    std::lock_guard<std::mutex> guard(mu_);
    ++stats_.get_hits_memtable;
    return s;
  }
  if (imm != nullptr && imm->Get(key, seq, value, &s)) return s;

  size_t probed = 0;
  auto probe = [&](const FileMetaData& f) -> bool {
    if (key < ExtractUserKey(f.smallest) || key > ExtractUserKey(f.largest)) {
      return false;
    }
    ++probed;
    return f.table->Get(key, seq, value, &s);
  };

  bool found = false;
  for (const auto& f : version->levels[0]) {  // Already newest-first.
    if (probe(*f)) {
      found = true;
      break;
    }
  }
  if (!found) {
    for (int level = 1; level < kNumLevels && !found; ++level) {
      const auto& files = version->levels[level];
      if (files.empty()) continue;
      // Files in a level below L0 have disjoint ranges, so at most one can hold
      // the key: binary search straight to it.
      const auto it = std::lower_bound(
          files.begin(), files.end(), key,
          [](const std::shared_ptr<FileMetaData>& f, std::string_view k) {
            return ExtractUserKey(f->largest) < k;
          });
      if (it == files.end()) continue;
      if (probe(**it)) found = true;
    }
  }

  {
    std::lock_guard<std::mutex> guard(mu_);
    stats_.tables_probed += probed;
  }

  if (!found) return Status::NotFound();
  return s;
}

// -------------------------------------------------------------------- flush

Status DB::FlushImmutableMemTable(std::unique_lock<std::mutex>& lock) {
  std::shared_ptr<MemTable> imm = imm_;
  if (imm == nullptr) return Status::OK();

  const uint64_t number = next_file_number_++;
  const std::string table_path = TablePath(number);

  auto meta = std::make_shared<FileMetaData>();
  meta->number = number;

  // Build the file with the lock dropped: writers keep going into the new
  // memtable while this runs, which is the point of having an immutable one.
  lock.unlock();
  Status s;
  {
    TableBuilder builder(options_);
    s = builder.Open(table_path);
    if (s.ok()) {
      MemTable::Iterator it(imm.get());
      for (it.SeekToFirst(); it.Valid(); it.Next()) {
        s = builder.Add(it.internal_key(), it.value());
        if (!s.ok()) break;
      }
    }
    if (s.ok()) s = builder.Finish();
    if (s.ok()) {
      meta->file_size = builder.FileSize();
      meta->smallest = builder.SmallestKey();
      meta->largest = builder.LargestKey();
    } else {
      builder.Abandon();
    }
  }
  std::unique_ptr<Table> table;
  if (s.ok()) s = Table::Open(table_path, options_, &table);
  lock.lock();

  if (!s.ok()) {
    bg_error_ = s;
    bg_cv_.notify_all();
    return s;
  }
  meta->table = std::shared_ptr<Table>(table.release());

  auto version = std::make_shared<Version>(*current_);
  version->levels[0].insert(version->levels[0].begin(), meta);  // Newest first.
  current_ = version;
  imm_ = nullptr;
  imm_log_number_ = 0;

  s = WriteManifest(*version, OldestLiveLog());
  if (!s.ok()) {
    bg_error_ = s;
    return s;
  }
  ++stats_.memtable_flushes;
  stats_.flush_output_bytes += meta->file_size;

  RemoveObsoleteFiles(*version, OldestLiveLog());
  bg_cv_.notify_all();
  return Status::OK();
}

// --------------------------------------------------------------- compaction

int DB::PickCompactionLevel(const Version& version) const {
  // L0 is scored by file count, not bytes: its files overlap, so every extra
  // one adds a block read to every point lookup that misses.
  if (version.levels[0].size() >=
      static_cast<size_t>(options_.l0_compaction_trigger)) {
    return 0;
  }
  int best = -1;
  double best_score = 1.0;
  for (int level = 1; level < kNumLevels - 1; ++level) {
    const double score = static_cast<double>(version.LevelBytes(level)) /
                         static_cast<double>(MaxBytesForLevel(options_, level));
    if (score > best_score) {
      best_score = score;
      best = level;
    }
  }
  return best;
}

Status DB::DoCompaction(std::unique_lock<std::mutex>& lock, int level) {
  auto base = current_;
  std::vector<std::shared_ptr<FileMetaData>> inputs0, inputs1;

  if (level == 0) {
    // L0 files overlap arbitrarily, so they all have to go together.
    inputs0 = base->levels[0];
  } else {
    const auto& files = base->levels[level];
    if (files.empty()) return Status::OK();
    // Round-robin across the level: take the first file starting after where
    // the last compaction of this level ended, wrapping when we run off it.
    std::shared_ptr<FileMetaData> chosen;
    for (const auto& f : files) {
      if (compact_pointer_[level].empty() ||
          CompareInternalKey(f->largest, compact_pointer_[level]) > 0) {
        chosen = f;
        break;
      }
    }
    if (chosen == nullptr) chosen = files.front();
    compact_pointer_[level] = chosen->largest;
    inputs0.push_back(std::move(chosen));
  }
  if (inputs0.empty()) return Status::OK();

  std::string smallest(ExtractUserKey(inputs0[0]->smallest));
  std::string largest(ExtractUserKey(inputs0[0]->largest));
  for (const auto& f : inputs0) {
    const std::string s(ExtractUserKey(f->smallest));
    const std::string l(ExtractUserKey(f->largest));
    if (s < smallest) smallest = s;
    if (l > largest) largest = l;
  }
  for (const auto& f : base->levels[level + 1]) {
    if (RangesOverlap(*f, smallest, largest)) inputs1.push_back(f);
  }

  // Tombstones may only be physically dropped once nothing below can still hold
  // an older version of the key -- otherwise deleting a key would resurrect it.
  bool bottom_most = true;
  for (int l = level + 2; l < kNumLevels; ++l) {
    if (!base->levels[l].empty()) {
      bottom_most = false;
      break;
    }
  }

  uint64_t input_bytes = 0;
  for (const auto& f : inputs0) input_bytes += f->file_size;
  for (const auto& f : inputs1) input_bytes += f->file_size;

  lock.unlock();

  // Children newest-first so that on an exact internal-key tie the newer source
  // wins. inputs0 comes from a shallower level, so it is newer by construction.
  std::vector<std::unique_ptr<Iterator>> children;
  children.reserve(inputs0.size() + inputs1.size());
  for (const auto& f : inputs0) children.push_back(f->table->NewIterator());
  for (const auto& f : inputs1) children.push_back(f->table->NewIterator());
  auto merged = NewMergingIterator(std::move(children));

  std::vector<std::shared_ptr<FileMetaData>> outputs;
  Status s;
  std::unique_ptr<TableBuilder> builder;
  std::shared_ptr<FileMetaData> current_meta;
  std::string last_user_key;
  bool have_last_user_key = false;
  uint64_t output_bytes = 0;

  auto finish_output = [&]() -> Status {
    if (!builder) return Status::OK();
    Status fs = builder->Finish();
    if (!fs.ok()) {
      builder->Abandon();
      return fs;
    }
    current_meta->file_size = builder->FileSize();
    current_meta->smallest = builder->SmallestKey();
    current_meta->largest = builder->LargestKey();
    std::unique_ptr<Table> table;
    fs = Table::Open(TablePath(current_meta->number), options_, &table);
    if (!fs.ok()) return fs;
    current_meta->table = std::shared_ptr<Table>(table.release());
    output_bytes += current_meta->file_size;
    outputs.push_back(current_meta);
    builder.reset();
    current_meta.reset();
    return Status::OK();
  };

  for (merged->SeekToFirst(); merged->Valid(); merged->Next()) {
    const std::string_view ikey = merged->key();
    const std::string_view user_key = ExtractUserKey(ikey);

    // The merge yields each user key's versions newest-first, so everything
    // after the first occurrence is shadowed and can be dropped. This is where
    // an LSM tree actually reclaims the space its overwrites cost.
    const bool is_new_key = !have_last_user_key || user_key != last_user_key;
    if (!is_new_key) continue;
    last_user_key.assign(user_key);
    have_last_user_key = true;

    if (ExtractValueType(ikey) == ValueType::kDeletion && bottom_most) continue;

    if (!builder) {
      lock.lock();
      const uint64_t number = next_file_number_++;
      lock.unlock();
      current_meta = std::make_shared<FileMetaData>();
      current_meta->number = number;
      builder = std::make_unique<TableBuilder>(options_);
      s = builder->Open(TablePath(number));
      if (!s.ok()) break;
    }
    s = builder->Add(ikey, merged->value());
    if (!s.ok()) break;

    if (builder->FileSize() >= options_.target_file_size) {
      s = finish_output();
      if (!s.ok()) break;
    }
  }
  if (s.ok()) s = merged->status();
  if (s.ok()) s = finish_output();

  lock.lock();
  if (!s.ok()) {
    if (builder) builder->Abandon();
    bg_error_ = s;
    bg_cv_.notify_all();
    return s;
  }

  // Install: drop the inputs, add the outputs one level down.
  std::set<uint64_t> removed;
  for (const auto& f : inputs0) removed.insert(f->number);
  for (const auto& f : inputs1) removed.insert(f->number);

  auto version = std::make_shared<Version>(*current_);
  for (int l : {level, level + 1}) {
    auto& files = version->levels[l];
    files.erase(std::remove_if(files.begin(), files.end(),
                               [&](const std::shared_ptr<FileMetaData>& f) {
                                 return removed.count(f->number) > 0;
                               }),
                files.end());
  }
  auto& target = version->levels[level + 1];
  target.insert(target.end(), outputs.begin(), outputs.end());
  std::sort(target.begin(), target.end(), [](const auto& a, const auto& b) {
    return CompareInternalKey(a->smallest, b->smallest) < 0;
  });
  current_ = version;

  s = WriteManifest(*version, OldestLiveLog());
  if (!s.ok()) {
    bg_error_ = s;
    return s;
  }
  ++stats_.compactions;
  stats_.compaction_input_bytes += input_bytes;
  stats_.compaction_output_bytes += output_bytes;

  RemoveObsoleteFiles(*version, OldestLiveLog());
  bg_cv_.notify_all();
  return Status::OK();
}

// ------------------------------------------------------------ background loop

void DB::MaybeScheduleCompaction() { bg_cv_.notify_all(); }

bool DB::HasPendingWork() const {
  return imm_ != nullptr || PickCompactionLevel(*current_) >= 0;
}

Status DB::BackgroundWorkOnce(std::unique_lock<std::mutex>& lock) {
  if (imm_ != nullptr) return FlushImmutableMemTable(lock);
  const int level = PickCompactionLevel(*current_);
  if (level < 0) return Status::OK();
  return DoCompaction(lock, level);
}

// Runs flushes and compactions inline until the tree is back within its level
// budgets. Used when background_compaction is off, and by the shutdown path.
Status DB::DrainBackgroundWork(std::unique_lock<std::mutex>& lock) {
  while (bg_error_.ok() && HasPendingWork()) {
    const Status s = BackgroundWorkOnce(lock);
    if (!s.ok()) return s;
  }
  return bg_error_;
}

void DB::BackgroundLoop() {
  std::unique_lock<std::mutex> lock(mu_);
  while (!shutting_down_) {
    if (!bg_error_.ok() || !HasPendingWork()) {
      bg_cv_.wait(lock);
      continue;
    }
    bg_running_ = true;
    BackgroundWorkOnce(lock);
    bg_running_ = false;
    bg_cv_.notify_all();
  }
}

void DB::WaitForBackgroundWork() {
  std::unique_lock<std::mutex> lock(mu_);
  if (!options_.background_compaction) {
    DrainBackgroundWork(lock);
    return;
  }
  while (bg_error_.ok() && (bg_running_ || HasPendingWork())) {
    bg_cv_.notify_all();
    bg_cv_.wait_for(lock, std::chrono::milliseconds(2));
  }
}

Status DB::FlushMemTable() {
  {
    std::unique_lock<std::mutex> lock(mu_);
    // Drain any flush already in flight before rotating again -- there is only
    // ever one immutable memtable slot.
    while (bg_error_.ok() && imm_ != nullptr) {
      if (!options_.background_compaction) {
        const Status s = DrainBackgroundWork(lock);
        if (!s.ok()) return s;
        break;
      }
      bg_cv_.notify_all();
      bg_cv_.wait_for(lock, std::chrono::milliseconds(2));
    }
    if (!bg_error_.ok()) return bg_error_;
    if (mem_->empty()) return Status::OK();

    const uint64_t new_log = next_file_number_++;
    auto new_writer = std::make_unique<WalWriter>();
    const Status s = new_writer->Open(LogPath(new_log));
    if (!s.ok()) return s;
    log_->Close();
    log_ = std::move(new_writer);
    imm_log_number_ = log_number_;
    log_number_ = new_log;
    imm_ = mem_;
    mem_ = std::make_shared<MemTable>();

    if (!options_.background_compaction) return DrainBackgroundWork(lock);
    bg_cv_.notify_all();
  }
  WaitForBackgroundWork();
  std::lock_guard<std::mutex> guard(mu_);
  return bg_error_;
}

Status DB::CompactAll() {
  const Status s = FlushMemTable();
  if (!s.ok()) return s;
  WaitForBackgroundWork();
  std::lock_guard<std::mutex> guard(mu_);
  return bg_error_;
}

// ----------------------------------------------------------- file bookkeeping

void DB::RemoveObsoleteFiles(const Version& live, uint64_t log_number) {
  std::set<uint64_t> live_tables;
  for (const auto& level : live.levels) {
    for (const auto& f : level) live_tables.insert(f->number);
  }

  DIR* dir = ::opendir(path_.c_str());
  if (dir == nullptr) return;
  std::vector<std::string> to_delete;
  while (struct dirent* entry = ::readdir(dir)) {
    uint64_t number;
    std::string kind;
    if (!ParseFileName(entry->d_name, &number, &kind)) continue;
    if (kind == "sst" && live_tables.count(number) == 0) {
      to_delete.push_back(path_ + "/" + entry->d_name);
    } else if (kind == "log" && number < log_number) {
      // Safe only because the manifest now names `log_number` as the oldest
      // log that recovery needs; everything older has been folded into an
      // SSTable that the manifest already references.
      to_delete.push_back(path_ + "/" + entry->d_name);
    }
  }
  ::closedir(dir);
  for (const std::string& p : to_delete) ::unlink(p.c_str());
}

void DB::SetWalWriteHook(WalWriter::WriteHook hook) {
  std::lock_guard<std::mutex> guard(mu_);
  if (log_) log_->SetWriteHook(std::move(hook));
}

DbStats DB::GetStats() const {
  std::lock_guard<std::mutex> guard(mu_);
  DbStats snapshot = stats_;
  for (const auto& level : current_->levels) {
    for (const auto& f : level) {
      snapshot.sstable_blocks_read += f->table->blocks_read();
      snapshot.bloom_rejections += f->table->bloom_rejections();
    }
  }
  return snapshot;
}

std::string DB::DebugLevelSummary() const {
  std::lock_guard<std::mutex> guard(mu_);
  std::ostringstream out;
  for (int level = 0; level < kNumLevels; ++level) {
    const auto& files = current_->levels[level];
    if (files.empty()) continue;
    out << "L" << level << ": " << files.size() << " files, "
        << (current_->LevelBytes(level) / 1024) << " KiB\n";
  }
  return out.str();
}

// -------------------------------------------------------------------- DBIter

namespace {

// Wraps a MemTable::Iterator in the common Iterator interface, holding a
// reference so the table outlives the cursor.
class MemTableIteratorAdapter : public Iterator {
 public:
  explicit MemTableIteratorAdapter(std::shared_ptr<MemTable> mem)
      : mem_(std::move(mem)), iter_(mem_.get()) {}

  bool Valid() const override { return iter_.Valid(); }
  void SeekToFirst() override { iter_.SeekToFirst(); }
  void Seek(std::string_view target) override { iter_.Seek(target); }
  void Next() override { iter_.Next(); }
  std::string_view key() const override { return iter_.internal_key(); }
  std::string_view value() const override { return iter_.value(); }

 private:
  std::shared_ptr<MemTable> mem_;
  MemTable::Iterator iter_;
};

// Concatenating iterator over one level below L0.
//
// Those levels hold files with disjoint, sorted key ranges, so the level as a
// whole is one sorted run: at any position exactly one file is live. Handing
// the merging iterator a single cursor per level instead of one per file is
// what keeps a scan's cost proportional to the number of levels rather than to
// the number of files -- with a file per child, every Seek would read a data
// block out of every file in the level.
class LevelIterator : public Iterator {
 public:
  explicit LevelIterator(std::vector<std::shared_ptr<FileMetaData>> files)
      : files_(std::move(files)) {}

  bool Valid() const override { return iter_ != nullptr && iter_->Valid(); }

  void SeekToFirst() override {
    index_ = 0;
    OpenCurrentFile();
    if (iter_ != nullptr) iter_->SeekToFirst();
    SkipEmptyFilesForward();
  }

  void Seek(std::string_view target) override {
    // First file whose largest key is >= target: the only one that can contain
    // it, and the right place to start scanning from.
    size_t lo = 0, hi = files_.size();
    while (lo < hi) {
      const size_t mid = lo + (hi - lo) / 2;
      if (CompareInternalKey(files_[mid]->largest, target) < 0) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    index_ = lo;
    OpenCurrentFile();
    if (iter_ != nullptr) iter_->Seek(target);
    SkipEmptyFilesForward();
  }

  void Next() override {
    iter_->Next();
    SkipEmptyFilesForward();
  }

  std::string_view key() const override { return iter_->key(); }
  std::string_view value() const override { return iter_->value(); }
  Status status() const override {
    if (!status_.ok()) return status_;
    return iter_ != nullptr ? iter_->status() : Status::OK();
  }

 private:
  void OpenCurrentFile() {
    iter_ = index_ < files_.size() ? files_[index_]->table->NewIterator() : nullptr;
  }

  // A Seek can land past the end of its file, and a file can be empty; step
  // forward until we have a live entry or run out of files.
  void SkipEmptyFilesForward() {
    while (iter_ != nullptr && !iter_->Valid()) {
      if (!iter_->status().ok()) {
        status_ = iter_->status();
        return;
      }
      ++index_;
      OpenCurrentFile();
      if (iter_ != nullptr) iter_->SeekToFirst();
    }
  }

  std::vector<std::shared_ptr<FileMetaData>> files_;  // Sorted, non-overlapping.
  size_t index_ = 0;
  std::unique_ptr<Iterator> iter_;
  Status status_;
};

// Collapses the internal-key stream into live user keys: takes the newest
// version of each key and hides it entirely if that version is a tombstone.
class DBIter : public Iterator {
 public:
  DBIter(std::unique_ptr<Iterator> inner, SequenceNumber seq,
         std::shared_ptr<const Version> version)
      : inner_(std::move(inner)), seq_(seq), version_(std::move(version)) {}

  bool Valid() const override { return valid_; }

  void SeekToFirst() override {
    inner_->SeekToFirst();
    AdvanceToNextLiveKey();
  }

  void Seek(std::string_view target) override {
    inner_->Seek(LookupKey(target, seq_));
    AdvanceToNextLiveKey();
  }

  void Next() override {
    SkipRestOfCurrentKey();
    AdvanceToNextLiveKey();
  }

  std::string_view key() const override { return key_; }
  std::string_view value() const override { return value_; }
  Status status() const override { return inner_->status(); }

 private:
  void SkipRestOfCurrentKey() {
    while (inner_->Valid() && ExtractUserKey(inner_->key()) == key_) inner_->Next();
  }

  void AdvanceToNextLiveKey() {
    while (inner_->Valid()) {
      const std::string_view ikey = inner_->key();
      if (ExtractSequence(ikey) > seq_) {  // Not visible in this snapshot.
        inner_->Next();
        continue;
      }
      key_.assign(ExtractUserKey(ikey));
      if (ExtractValueType(ikey) == ValueType::kDeletion) {
        SkipRestOfCurrentKey();
        continue;
      }
      const std::string_view v = inner_->value();
      value_.assign(v.data(), v.size());
      valid_ = true;
      return;
    }
    valid_ = false;
  }

  std::unique_ptr<Iterator> inner_;
  SequenceNumber seq_;
  std::shared_ptr<const Version> version_;  // Keeps the referenced tables alive.
  std::string key_;
  std::string value_;
  bool valid_ = false;
};

}  // namespace

std::unique_ptr<Iterator> DB::NewIterator(const ReadOptions& opts) {
  (void)opts;
  std::shared_ptr<MemTable> mem, imm;
  std::shared_ptr<const Version> version;
  SequenceNumber seq;
  {
    std::lock_guard<std::mutex> guard(mu_);
    mem = mem_;
    imm = imm_;
    version = current_;
    seq = last_sequence_;
  }

  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(std::make_unique<MemTableIteratorAdapter>(mem));
  if (imm != nullptr) {
    children.push_back(std::make_unique<MemTableIteratorAdapter>(imm));
  }
  // L0 files overlap each other, so each needs its own cursor. Every deeper
  // level is a single sorted run and gets one concatenating cursor.
  for (const auto& f : version->levels[0]) {
    children.push_back(f->table->NewIterator());
  }
  for (int level = 1; level < kNumLevels; ++level) {
    if (version->levels[level].empty()) continue;
    children.push_back(std::make_unique<LevelIterator>(version->levels[level]));
  }
  return std::make_unique<DBIter>(NewMergingIterator(std::move(children)), seq,
                                  version);
}

}  // namespace lsm
