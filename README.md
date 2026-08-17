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
ctest --test-dir build --output-on-failure   # 103 tests
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

Both engines run back-to-back on an otherwise idle machine, with matched 4 MiB
write buffers, 4 KiB blocks, 10-bits-per-key Bloom filters and 8 MiB block
caches. Checksum verification is **on** for lsmtree; LevelDB's default
(`verify_checksums = false`) is left alone, so this is the harder configuration
for us.

Throughput, operations per second:

| Workload | Mix | lsmtree | LevelDB | Ratio |
|---|---|---:|---:|---:|
| load | 100% insert | 381 895 | 489 438 | 0.78x |
| A | 50% read / 50% update | **437 989** | 280 197 | **1.56x** |
| B | 95% read / 5% update | **764 816** | 704 109 | **1.09x** |
| C | 100% read | 996 459 | 1 293 279 | 0.77x |
| D | 95% read latest / 5% insert | 1 136 292 | 1 263 159 | 0.90x |
| E | 95% scan (50 rows) / 5% insert | 19 030 | 243 579 | 0.08x |
| F | 50% read / 50% read-modify-write | **348 869** | 238 502 | **1.46x** |

Latency, microseconds (lsmtree):

| Workload | p50 | p95 | p99 | p99.9 |
|---|---:|---:|---:|---:|
| load | 1.38 | 2.75 | 4.58 | 12.54 |
| A | 1.50 | 3.29 | 4.62 | 13.75 |
| B | 0.50 | 2.92 | 3.62 | 8.25 |
| C | 0.46 | 2.58 | 3.08 | 4.67 |
| D | 0.33 | 2.58 | 3.21 | 4.67 |
| E | 17.00 | 219.79 | 238.29 | 309.29 |
| F | 1.96 | 5.08 | 6.75 | 14.29 |

### How it got here

Three changes, each measured, on the read path. Same hardware and workloads
throughout:

| Workload | Baseline | + block cache | + fast CRC-32C | LevelDB |
|---|---:|---:|---:|---:|
| A | 245 559 | 233 817 | **437 989** | 280 197 |
| B | 209 565 | 273 890 | **764 816** | 704 109 |
| C | 193 454 | 266 032 | **996 459** | 1 293 279 |
| D | 293 905 | 377 117 | **1 136 292** | 1 263 159 |
| E | 1 489 | 12 271 | **19 030** | 243 579 |
| F | 156 129 | 176 693 | **348 869** | 238 502 |

**The merging iterator** was the first fix, before the table above. `NewIterator`
gave the merge one cursor per *file*, so every `Seek` read a block out of every
file in every level. One concatenating cursor per level below L0 took scans from
1 141 to ~20 000 ops/sec on a freshly-loaded database.

**The block cache** cut blocks fetched from the filesystem across a full run from
986 249 to 201 758, at a 93.8% hit rate. Scans gained most (+724%) because they
were paying worst: stepping over the shadowed versions of a Zipfian-hot key could
touch a hundred blocks for one logical row.

**CRC-32C** turned out to be the whole remaining read gap, which was not obvious
until it was measured. Point-read p95 sat at a suspiciously flat ~11 µs across
every read-heavy workload regardless of what else changed — not a cache-miss
profile, and unaffected by removing a malloc per lookup or two mutex acquisitions
per `Get`. Running with verification disabled took workload C from 264 k to
1 295 k ops/sec, which identified it: a byte-at-a-time table CRC over every
4 KiB block, roughly a nanosecond per byte, was ~9 µs of that p95. LevelDB does
not verify block checksums by default, so it never paid this at all.

The fix was to make verification cheap rather than to turn it off. `Extend` now
uses the CPU's CRC32C instruction (ARMv8 CRC or SSE4.2) with slicing-by-8 tables
as the portable fallback, and a test asserts the two paths agree bit-for-bit
across a range of lengths — a mismatch between builds would make already-written
files unreadable.

Two smaller changes are in the same range: point lookups now build their
internal key in a stack buffer rather than a `std::string` (one fewer malloc per
read), and the read-path counters are atomics, so a `Get` takes the DB mutex once
instead of three times. Each was worth a few percent on its own — worth keeping,
but neither was the bottleneck, and the measurements say so.

### Tail latency, and what hid it

The benchmark originally reported up to p99.9 and no further. That turned out to
hide the single worst problem in the engine. On a run where workload E averaged
296 µs per operation, p50 was 20 µs and p99.9 was 310 µs — figures that cannot
produce that mean. The time was all beyond p99.9, where nothing was looking.

Adding a max column made it obvious: single operations were taking 12–100 ms,
and workload C — the only workload that performs no writes, and therefore
triggers no flush or compaction — had a max of 18 µs. Readers were blocking on
background work.

Two causes, both fixed:

- **The manifest commit ran with the DB lock held.** `WriteManifest` does an
  `F_FULLFSYNC`, a `rename`, and a directory fsync; `RemoveObsoleteFiles` does an
  `opendir` and a series of `unlink`s. Every reader takes that same lock. The
  commit now installs the version under the lock, then releases it and does the
  filesystem work outside, serialized against other commits by a dedicated flag
  so two of them cannot write the manifest out of order or let one commit's
  cleanup delete files the other just added. Everything `WriteManifest` needs is
  passed in by value, so it touches no state another thread can be writing.
- **`BlockCache::EraseFile` walked every entry in every shard**, holding each
  shard lock, once per file a compaction retired — also under the DB lock. It is
  no longer on that path at all: retired blocks are keyed by file number, can
  never be looked up again, and age out through LRU without anyone waiting.

Measured back-to-back on the same machine, workload C went from 874 599 to
1 149 782 ops/sec and D from 828 798 to 1 234 567.

A caveat on the numbers in this section: they were taken on a machine that had
been running benchmarks continuously for hours, and absolute throughput had
drifted down roughly 30% for *both* engines by then. The before/after pairs are
back-to-back on that same state so the comparison holds, but they are not
comparable to the table above, which was taken on an idle machine. Multi-second
maxima also persist and are I/O stalls at the OS level rather than lock
contention — they show up in the `load` workload, which has no readers to block.

### Why scans are still 0.08x

Workload E is the one remaining large gap, and the cause is understood rather
than mysterious. `MergingIterator::FindSmallest` is a linear scan over its
children: with L0 files plus one cursor per deeper level, every `Next()` costs
~20 comparisons. That is fine for compaction, which merges a handful of files,
but a range scan after heavy update churn has to step over every shadowed
version of each hot key to reach the next live one — so the linear scan is paid
hundreds of times per logical row. A binary heap makes each step O(log n)
instead, and that is the next thing to build.

The measurement that isolates it: E runs at ~228 000 ops/sec when the workload
sequence is `load, C, E`, and ~19 000 in the full `load, A, B, C, D, F, E`
sequence, at identical level shapes. The difference is entirely how many
shadowed versions the scan has to walk.

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

- **Scan setup costs ~2.4 µs** before a single row is read — an iterator per L0
  file plus one per level, each seeking a block. At the default 50-row scan that
  is about a third of the total.
- **No restart points or prefix compression in data blocks.** Keys are stored
  whole and located by a linear walk from the start of the block. LevelDB's
  restart intervals give both a binary search within the block and a smaller
  file.
- **`ReadOptions::verify_checksums` is ignored.** Verification is controlled by
  the database-wide `Options` only. Now that verification is cheap this matters
  much less, but the per-read option should either work or not exist.
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
