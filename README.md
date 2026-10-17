# lsmtree

An embedded key-value store in C++17, built to understand how storage engines
actually work. It's a library you link into a program — you call `Put` and
`Get`, and it guarantees that if the machine dies a millisecond after `Put`
returns, the data is still there when you restart. Roughly what sits underneath
SQLite, LevelDB, or Cassandra.

No dependencies beyond the standard library and POSIX. GoogleTest is fetched by
CMake for tests only; zlib is used for compression if CMake finds it, and
everything works without it.

```cpp
#include "lsm/db.h"

std::unique_ptr<lsm::DB> db;
lsm::DB::Open(lsm::Options(), "/var/data/mydb", &db);

db->Put(lsm::WriteOptions(), "user:1", "alice");

std::string value;
if (db->Get(lsm::ReadOptions(), "user:1", &value).ok()) { /* "alice" */ }

// Several writes, applied atomically: after a crash, all of it or none.
lsm::WriteBatch batch;
batch.Put("user:2", "bob");
batch.Delete("user:1");
db->Write(lsm::WriteOptions(), batch);

// Sorted scan over live keys.
for (auto it = db->NewIterator(lsm::ReadOptions()); it->SeekToFirst(), it->Valid();
     it->Next()) {
  // it->key(), it->value()
}
```

## Building

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure   # 195 tests
```

## The idea

Disks hate random writes. Storing data as a tree on disk means an update in the
middle costs a seek and a full page rewrite — for a 100-byte value. An LSM tree
refuses to write into the middle of anything:

```
Put(key, value)
  |-- append one record to the write-ahead log   <- sequential, the fastest thing a disk does
  |-- insert into an in-memory sorted skip list
  '-- return

memtable fills up
  '-- write it out as one immutable SSTable, start over

files pile up and reads slow down
  '-- a background thread merges them into fewer, bigger files
```

That's the whole trade: writes get fast, reads get slower, and background
merging (compaction) is the rent you pay. Compaction is where most of the
difficulty lives — it's also what reclaims space from overwrites and finalises
deletes.

Two details that everything else falls out of:

**Every key carries an 8-byte trailer** of `(sequence << 8 | type)`. Keys sort
by user key ascending, then sequence *descending*, so the newest version of a
key is the first one a forward scan meets. Point lookups, snapshot reads, and
compaction's shadowed-version filter all drop out of that single ordering.

**A delete doesn't delete.** It writes a tombstone, because the real value may
still sit in an older file. Only compaction can drop both together.

## What's in it

| | |
|---|---|
| Skip-list memtable | Single writer, lock-free readers — insertion publishes pointers, never rotates |
| Write-ahead log | CRC-32C per record, three fsync policies, group commit |
| SSTables | Data blocks + sparse index + Bloom filter + footer |
| Data blocks | Prefix compression with restart points |
| Bloom filters | 1.23% false positives measured at 10 bits/key |
| Block cache | Sharded LRU over decoded blocks |
| Compaction | Leveled, 7 levels at 10× growth |
| Snapshots | Pinned read sequence; also gates what compaction may discard |
| Manifest | Append-only edit log, ~115 bytes per commit regardless of database size |
| Compression | Optional, off by default — built-in LZ77 or zlib |

## Performance

Benchmarked against LevelDB — Google's production engine and the reference
implementation of this design — on identical YCSB-style workloads, matched
settings, back to back on an idle machine. Checksum verification is **on** here
while LevelDB's default leaves it off, so this is the harder configuration.

| Workload | lsmtree | LevelDB | |
|---|---:|---:|---|
| load (100% insert) | 432 065 | 499 547 | 0.87x |
| A (50% read / 50% update) | **464 655** | 222 640 | **2.09x** |
| B (95% read) | **862 548** | 710 916 | **1.21x** |
| C (100% read) | 1 149 186 | 1 210 596 | 0.95x |
| D (read latest) | 1 157 849 | 1 294 580 | 0.89x |
| E (scan) | 150 267 | 241 630 | 0.62x |
| F (read-modify-write) | **392 766** | 243 388 | **1.61x** |

Ops/sec, 500 000 keys, 100-byte values. On-disk size is 57 MiB for both.

One caveat on workload A: LevelDB's number for it swings between runs (222–285K
across mine), and this table caught it on a low one. Against its best A the
ratio is closer to 1.6x than 2.1x.

Scans are the one real gap. Positioning a cursor in every level is inherent to a
range scan in an LSM tree, and profiling says the remaining difference is spread
across per-level work rather than any single thing worth fixing.

## Crash testing

The part I'd point at first. A harness spawns a writer, kills it with `SIGKILL`
at a random moment, restarts, and checks two things: every write the engine
returned `OK` for is readable, and the recovered keys form a contiguous prefix —
no holes, and nothing past the last complete record. It runs a few hundred
rounds across all three fsync policies, and also tears WAL records deliberately.

```
300/300 rounds passed; 298 killed by signal mid-operation
RESULT: PASS -- every acknowledged write survived every crash
```

Plus 195 unit and integration tests, clean under AddressSanitizer and
ThreadSanitizer, all running in CI.

## Three things worth reading the code for

**A durability bug the harness found.** Compaction wrote the manifest naming the
*active* log. If a memtable rotation happened during the compaction's unlocked
window, cleanup would delete a log that still held acknowledged writes. 34 of
300 rounds lost exactly one memtable's worth of data. Fixed by naming the oldest
log recovery still needs.

**Reads were slow for a reason I guessed wrong twice.** Point-read p95 sat at a
flat ~11 µs. It wasn't allocation and it wasn't lock contention — I fixed both
and nothing moved. It was checksum verification: a byte-at-a-time CRC over every
4 KiB block. Switching to the CPU's CRC32C instruction took reads from 264K to
1.2M ops/sec, with verification still on.

**Scans were 0.07x, and the iterator was innocent.** Counting dead entries
stepped over per live row returned gave 41. The engine was hoarding versions:
compaction collapsed each key to its newest, but flushing a memtable dumped
every version verbatim, and a 4 MiB memtable under skewed updates holds
thousands of versions of its hottest keys. Applying compaction's own rule to the
flush took scans from 19K to 151K ops/sec.

The pattern in all three: measure the specific quantity rather than reason about
the plausible cause.

## Known gaps

- **Scans at 0.62x.** Diagnosed, not a single fixable thing.
- **Writers serialize.** Group commit shares the fsync, but they still queue
  behind the leader rather than proceeding in parallel.
- **Compression costs a third of read throughput**, so it stays off by default.
  A production LZ4 or Snappy would be a drop-in behind the existing interface.

## Layout

```
include/lsm/   public headers
src/           implementation
tests/         GoogleTest suite
bench/         YCSB-style benchmark, and the LevelDB adapter
tools/         crash harness and its worker process
```
