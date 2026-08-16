# lsmtree

An embedded log-structured merge-tree key-value store in C++17. Writes go to a
write-ahead log and an in-memory skip list; the skip list is flushed to
immutable, block-structured SSTables; leveled compaction merges them back down
and reclaims the space that overwrites and deletes leave behind.

No dependencies beyond the standard library and POSIX. GoogleTest is fetched by
CMake for the test build only.

```cpp
#include "lsm/db.h"

lsm::Options options;
options.sync_policy = lsm::SyncPolicy::kEveryWrite;

std::unique_ptr<lsm::DB> db;
lsm::DB::Open(options, "/var/data/mydb", &db);

db->Put(lsm::WriteOptions(), "user:1", "alice");

std::string value;
lsm::Status s = db->Get(lsm::ReadOptions(), "user:1", &value);
if (s.ok()) { /* value == "alice" */ }

for (auto it = db->NewIterator(lsm::ReadOptions()); it->SeekToFirst(), it->Valid();
     it->Next()) {
  // it->key(), it->value() -- live keys only, in sorted order
}
```

## Building

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
```

Sanitizer builds (mutually exclusive):

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DLSM_ASAN=ON && cmake --build build-asan
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DLSM_TSAN=ON && cmake --build build-tsan
```

## How a write becomes durable

```
Put(key, value)
  |
  |-- 1. append to the write-ahead log       <- sequential; fsync per policy
  |-- 2. insert into the skip-list memtable  <- in memory, sorted
  '-- return OK

memtable exceeds memtable_size_bytes
  |
  |-- becomes immutable, a fresh memtable and log take over
  '-- background thread writes it out as one L0 SSTable, then commits the
      manifest

L0 reaches l0_compaction_trigger files, or a level exceeds its byte budget
  |
  '-- compaction merges the overlapping files one level down, dropping
      shadowed versions and (at the bottom) tombstones
```

The WAL append happens *before* the memtable insert. Reversed, a crash could
leave a value that a reader had already seen but that was never logged — after
recovery an acknowledged write would simply be gone.

## Components

| Component | Files | Notes |
|---|---|---|
| Skip-list memtable | [`include/lsm/skiplist.h`](include/lsm/skiplist.h), [`memtable.h`](include/lsm/memtable.h) | Single writer, lock-free readers |
| Arena | [`include/lsm/arena.h`](include/lsm/arena.h) | Bump allocator; a memtable is freed whole |
| Write-ahead log | [`include/lsm/wal.h`](include/lsm/wal.h), [`src/wal.cpp`](src/wal.cpp) | CRC-32C per record, three fsync policies |
| SSTable | [`include/lsm/sstable.h`](include/lsm/sstable.h), [`src/sstable.cpp`](src/sstable.cpp) | Data blocks + sparse index + bloom + footer |
| Bloom filter | [`include/lsm/bloom.h`](include/lsm/bloom.h), [`src/bloom.cpp`](src/bloom.cpp) | Configurable bits/key, double hashing |
| Block cache | [`include/lsm/cache.h`](include/lsm/cache.h), [`src/cache.cpp`](src/cache.cpp) | Sharded LRU over decoded data blocks |
| Levels, compaction, recovery | [`include/lsm/db.h`](include/lsm/db.h), [`src/db.cpp`](src/db.cpp) | 7 levels, 10× growth, MVCC on file metadata |

### Internal keys

Every stored key carries an 8-byte trailer of `(sequence << 8 | type)`. Keys
sort by user key ascending, then by sequence **descending**, so the newest
version of a key is the first one a forward scan meets. That single ordering
decision is what makes point lookups, snapshot reads, and compaction's
shadowed-version filter all fall out of the same comparator.

A `Delete` writes a tombstone rather than removing anything — the key may still
live in a deeper SSTable, and only compaction can drop both together.

### SSTable layout

```
+------------------+
| data block 0     |  [key_len][internal key][value_len][value] ... + crc32c
| data block 1     |
| ...              |
+------------------+
| bloom block      |  bit array over user keys + probe count + crc32c
+------------------+
| index block      |  one [last key][offset][size] entry per data block + crc32c
+------------------+
| footer (40 B)    |  index handle, bloom handle, magic
+------------------+
```

Blocks exist because storage is addressed in pages: a point lookup should
transfer one ~4 KiB chunk, not the whole file and not a byte at a time. The
index is *sparse* — one entry per block, not per key — so it stays small enough
to hold resident, and a lookup is a binary search in memory plus exactly one
block read. Within a block, a linear scan beats a binary search: it is a single
sequential pass over bytes already in L1 cache.

### Read path

Newest source first: active memtable → immutable memtable → L0 files
newest-first → one file per deeper level. The first source that knows about the
key is authoritative, *including* when what it knows is a tombstone. L0 files
overlap arbitrarily so all of them may need probing; every deeper level is a
single sorted run, so a binary search picks the at most one file that can hold
the key.

Data blocks go through a sharded LRU cache of already-decoded blocks. Blocks are
handed out as `shared_ptr`, so a cursor mid-scan keeps its block alive even if
the cache evicts it underneath — that is what makes eviction safe without any
reference-counting protocol at the call site. Compaction erases the cache
entries of the files it retires, so dead blocks stop holding capacity the new
outputs need.

### Durability and recovery

`SyncPolicy` is the durability/throughput knob:

| Policy | Survives process crash | Survives power loss | Cost |
|---|---|---|---|
| `kEveryWrite` | yes | yes | one fsync per write |
| `kInterval` | yes | writes older than the interval | one fsync per interval |
| `kNever` | yes | no | none |

`kNever` still survives a `SIGKILL` because the bytes are in the page cache and
the kernel owns them; it does not survive a kernel panic or a power cut. On
macOS the sync path uses `F_FULLFSYNC`, since plain `fsync()` there only pushes
data to the drive, which may still hold it in a volatile write cache.

Recovery replays every log from the one the manifest names onward. A process
killed mid-write leaves a short or bad-CRC record at the tail; the reader treats
the first such record as the end of the log. That is correct because a torn
record is by construction the one that was still being written, and was never
acknowledged.

The manifest is the commit point. It is written to a temp file, fsynced, and
`rename()`d — atomic, so a crash leaves either the old file list or the new one,
never a half-written one. SSTables are fsynced before the manifest is allowed to
reference them, and the directory is fsynced after the rename (a rename is not
durable until the *directory* entry is).

## Testing

```bash
ctest --test-dir build --output-on-failure   # 92 tests
./build/lsm_crash_harness --rounds 300 --writes 60000
./build/lsm_bench --keys 500000 --ops 500000
```

### Fault injection

[`tools/crash_harness.cpp`](tools/crash_harness.cpp) spawns a writer process,
kills it at a random point, then reopens the database and checks two properties:

1. **Durability** — every write the engine returned `OK` for is readable after
   recovery. The writer publishes its acknowledged count into a `MAP_SHARED`
   page, which the kernel keeps after `SIGKILL`.
2. **Atomicity** — the recovered keys are exactly a contiguous prefix. A hole
   would mean a record was skipped; a key past the last complete record would
   mean a torn fragment was accepted as real.

Two crash shapes are covered: an asynchronous `SIGKILL` that can land anywhere
including mid-`write()` or mid-`fsync()`, and a deliberately torn WAL record
(the write hook truncates the encoded record and the process exits before
acknowledging it). Both run across all three sync policies.

Latest campaign — 300 randomized rounds, ~486 000 acknowledged writes, 294 of
300 processes killed by signal mid-operation:

```
300/300 rounds passed; 294 killed by signal mid-operation
total acknowledged writes: 486398, total recovered: 486565
RESULT: PASS -- every acknowledged write survived every crash
```

This harness found a real durability bug. Compaction was writing the manifest
with the *active* log number; if a memtable rotation had happened while the
compaction was running with the lock released, the manifest would name a log
newer than the one the pending immutable memtable was written to — and the
cleanup pass would then delete a log that still held acknowledged writes. 34 of
300 rounds lost exactly one memtable's worth of data. The fix
([`OldestLiveLog()`](src/db.cpp)) is to name the oldest log recovery still
needs, which is the immutable memtable's log while a flush is outstanding.

### Sanitizers

CI runs the full suite under AddressSanitizer + UndefinedBehaviorSanitizer and
under ThreadSanitizer, plus Debug and RelWithDebInfo builds. TSan is the one
that matters here: it covers the lock-free skip-list read path and the version
handoff between compaction and in-flight readers.

## Benchmarks

Hardware: Apple M-series, macOS 26.3, APFS on SSD. 500 000 keys, 100-byte
values, single thread, `sync=never`, no compression on either engine. LevelDB
1.23 from Homebrew, configured with a matching 4 MiB write buffer, 4 KiB blocks
and a 10-bits-per-key Bloom filter.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLSM_BENCH_LEVELDB=ON
cmake --build build
./build/lsm_bench --engine lsmtree --keys 500000 --ops 500000
./build/lsm_bench --engine leveldb --keys 500000 --ops 500000
```

Throughput, operations per second:

| Workload | Mix | lsmtree | LevelDB | Ratio |
|---|---|---:|---:|---:|
| load | 100% insert | 328 954 | 502 996 | 0.65x |
| A | 50% read / 50% update | 233 817 | 280 481 | 0.83x |
| B | 95% read / 5% update | 273 890 | 719 733 | 0.38x |
| C | 100% read | 266 032 | 1 337 607 | 0.20x |
| D | 95% read latest / 5% insert | 377 117 | 1 277 812 | 0.30x |
| E | 95% scan (50 rows) / 5% insert | 12 271 | 242 951 | 0.05x |
| F | 50% read / 50% read-modify-write | 176 693 | 251 451 | 0.70x |

Latency, microseconds (lsmtree):

| Workload | p50 | p95 | p99 | p99.9 |
|---|---:|---:|---:|---:|
| load | 1.67 | 3.00 | 4.67 | 16.54 |
| A | 1.79 | 12.29 | 16.04 | 60.75 |
| B | 0.58 | 11.33 | 13.42 | 25.83 |
| C | 0.54 | 11.04 | 13.42 | 21.54 |
| D | 0.33 | 10.83 | 12.46 | 20.08 |
| E | 57.54 | 221.29 | 267.42 | 891.04 |
| F | 2.17 | 13.62 | 15.54 | 26.29 |

Engine counters for the lsmtree run: 40 memtable flushes, 28 compactions,
509 MiB read into compaction against 444 MiB written out, **5.95x write
amplification**, and a **93.8% block cache hit rate** (33.4M hits, 2.2M misses)
over the 1 074 802 writes issued.

### What the block cache bought

Adding the cache is the single largest change measured so far. Same hardware,
same workloads, same level shapes:

| Workload | Before | After | Change |
|---|---:|---:|---:|
| B (95% read) | 209 565 | 273 890 | +31% |
| C (100% read) | 193 454 | 266 032 | +38% |
| D (read latest) | 293 905 | 377 117 | +28% |
| F (read-modify-write) | 156 129 | 176 693 | +13% |
| **E (scan)** | **1 489** | **12 271** | **+724%** |

Blocks actually fetched from the filesystem across the whole run fell from
986 249 to 201 758. Scans gain the most because they were paying the worst
price: stepping over the shadowed versions of a Zipfian-hot key could touch a
hundred blocks for one logical row, and every one of those was a fresh `pread`
plus a re-parse.

Workload A is flat to slightly down, which is the honest result — it is half
writes, and the cache adds a hashed, mutex-guarded lookup to a path that was
already mostly hitting the memtable.

### Why it is still slower

**Writes (0.65x on load).** Mostly per-record framing. This engine writes a
fixed 4-byte length prefix where LevelDB uses varints and prefix-compresses keys
within a block, so identical data occupies more bytes in both the WAL and the
SSTable. LevelDB also groups concurrent writers into one log append; here every
writer takes the mutex on its own.

**Point reads (0.20–0.38x).** No longer a cache-miss problem — the hit rate is
93.8%. What remains is per-lookup constant factor, and there are three known
contributors, in rough order of expected size:

1. `Table::Get` builds its lookup key with a heap-allocating `std::string`, so
   every point read costs a malloc. LevelDB assembles short keys in a stack
   buffer.
2. Data blocks have no restart points, so finding a key inside a block is a
   linear walk that re-decodes every length prefix from the start of the block.
   LevelDB binary-searches to the nearest restart point first.
3. The cache lookup itself takes a per-shard mutex on the hot path.

**Scans (0.05x).** Same three causes, amplified by how many entries a scan
touches. The per-file-cursor problem that used to dominate here is fixed — the
merge now takes one concatenating cursor per level below L0 rather than one per
file — and the cache removed the I/O. What is left is the constant factor above,
paid once per entry stepped over rather than once per operation.

### Bloom filter false-positive rates

Measured over 10 000 inserted keys and 20 000 absent probes
([`tests/test_bloom.cpp`](tests/test_bloom.cpp)):

| Bits per key | Probes (k) | Measured FP rate | Theoretical `0.62^(m/n)` |
|---:|---:|---:|---:|
| 6 | 4 | 6.26% | 5.9% |
| 10 | 6 | 0.89% | 0.9% |
| 16 | 11 | 0.02% | 0.05% |

At the default 10 bits per key a lookup for an absent key reads zero data blocks
in the single-table test — the filter rejects it outright.


## Known gaps

These are deliberate, not oversights:

- **A malloc per point lookup.** `Table::Get` builds its lookup key into a
  `std::string`. This is the most obviously fixable item on the list and is
  next.
- **No restart points or prefix compression in data blocks.** Keys are stored
  whole and located by a linear walk from the start of the block. LevelDB's
  restart intervals give both a binary search within the block and a smaller
  file.
- **No compression.** Both engines are benchmarked with compression off so the
  comparison is about structure rather than about zlib.
- **Whole-DB write lock.** One mutex serializes writers. Real engines batch
  concurrent writes into a group commit and take one fsync for the group.
- **No explicit snapshots.** Reads use the sequence number current at their
  start, which gives a consistent view, but there is no handle to pin one open
  across calls.
- **Manifest is rewritten in full**, rather than appended to as a log of edits.
  Simple and atomic, but O(number of files) per compaction.

## Design questions

**Why an LSM tree instead of a B-tree?** A B-tree update is a read-modify-write
of a page at a random offset: a seek, a read, and a write of a full page for a
100-byte value. An LSM tree turns every write into an append to a log plus an
in-memory insert, and pays for it later in large sequential merges. It trades
read amplification for write amplification, and buys sequential I/O for random.

**What is read amplification, and how does compaction trade against write
amplification?** Read amplification is how many stored entries you must examine
per logical read; write amplification is how many bytes hit the disk per byte
the user wrote. Compacting more aggressively keeps levels tidy and lookups
short, but each merge rewrites data that was already durable. Compacting less
leaves more overlapping files, so lookups probe more of them. The measured
figures for both are in the benchmark output above.

**Why a skip list over a balanced BST?** Insertion only ever publishes forward
pointers; it never rotates. A single writer can insert while any number of
readers traverse with no lock on the read path, because each `next` pointer is
an atomic whose release-store pairs with the reader's acquire-load — a reader
either sees a fully-built node or does not see it at all. A balanced tree has to
rewire a subtree to rebalance, which readers cannot safely walk through without
locking.

**What does fsync actually guarantee?** That the data and metadata for the file
have reached stable storage before it returns. It does *not* cover the
directory entry — a newly created or renamed file needs the directory fsynced
too — and on macOS plain `fsync()` only reaches the drive, not the platter, so
`F_FULLFSYNC` is required for real power-loss durability. Skipping it entirely
means acknowledged writes live only in the page cache: safe against a process
crash, lost in a power cut.

**How does the Bloom filter's false-positive rate change with bits per key?**
Roughly `0.62^(bits per key)` at the optimal probe count `k = (m/n) ln 2`.
Measured by [`tests/test_bloom.cpp`](tests/test_bloom.cpp), which prints the
observed rate at 6, 10, and 16 bits per key and fails if it drifts above the
expected envelope.

## Layout

```
include/lsm/     public headers
src/             implementation
tests/           GoogleTest suite
bench/           YCSB-style benchmark and the engine adapter
tools/           fault-injection harness and its worker process
.github/         CI
```
