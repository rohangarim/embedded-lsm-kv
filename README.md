# lsmtree

A small embedded key-value store in C++17. You link it into a program and call
`Put` and `Get`; if the machine dies right after `Put` returns, the data is
still there on restart. It's the same idea as LevelDB or what sits under
Cassandra, written from scratch so I'd understand how it really works.

No dependencies beyond the standard library and POSIX (GoogleTest is fetched
for tests, zlib is optional).

```cpp
#include "lsm/db.h"

std::unique_ptr<lsm::DB> db;
lsm::DB::Open(lsm::Options(), "/var/data/mydb", &db);

db->Put(lsm::WriteOptions(), "user:1", "alice");

std::string value;
db->Get(lsm::ReadOptions(), "user:1", &value);

lsm::WriteBatch batch;           // all of it or none of it, even across a crash
batch.Put("user:2", "bob");
batch.Delete("user:1");
db->Write(lsm::WriteOptions(), batch);

auto it = db->NewIterator(lsm::ReadOptions());   // sorted, forward or backward
for (it->SeekToFirst(); it->Valid(); it->Next()) { /* it->key(), it->value() */ }
```

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build && ctest --test-dir build   # 197 tests
```

## How it works

Random writes are slow on disk, so an LSM tree never writes into the middle of
anything. A `Put` appends to a log and goes into an in-memory skip list. When
that fills up it's written out as one immutable sorted file, and a background
thread keeps merging files so reads don't have to check too many of them.
Deletes write a tombstone, since the old value may still sit in an older file.

What's in the box: a skip-list memtable, a write-ahead log with CRC checks and
group commit, SSTables with prefix-compressed blocks and Bloom filters, a
sharded block cache, leveled compaction, snapshots, an append-only manifest,
optional compression, and write backpressure when compaction falls behind.

## How well it works

Measured on an Apple M4, 500K keys, 100-byte values, buffered writes
(`sync=never`), checksums on. Raw output is in `results/` and every figure is
checked in [`VERIFIED_NUMBERS.md`](VERIFIED_NUMBERS.md).

| Workload | ops/sec |
|---|---:|
| load (inserts) | ~430K |
| 50% read / 50% update | ~460K |
| 95% read | ~860K |
| 100% read | ~1.15M |
| scan | ~150K |

Against LevelDB on the same workloads it wins on the mixed ones, is about even
on reads, and loses on pure inserts and scans (scans are around 0.6x). LevelDB's
numbers swing a lot between runs, so I won't quote an exact ratio.

- **Bloom filters:** 1.23% false positives at 10 bits/key, measured over a
  million lookups of keys that aren't there.
- **Crash testing:** a harness kills the writer with `SIGKILL` at random moments
  and checks that every acknowledged write survives and nothing is half-written.
  300 of 300 rounds passed. That proves process-crash safety, not power loss.
- Clean under AddressSanitizer, UBSan and ThreadSanitizer, run in CI.

## Things the tests taught me

- **A durability bug the crash harness found:** compaction recorded the wrong
  log in the manifest, so cleanup could delete a log that still held
  acknowledged writes.
- **Slow reads weren't what I thought:** I fixed allocation and lock contention
  and nothing moved. It was checksumming every 4 KiB block a byte at a time;
  using the CPU's CRC instruction took reads from 264K to 1.2M ops/sec.
- **Scans were 0.07x:** flushing a memtable kept every old version of a hot key,
  so a scan stepped over ~41 dead entries per row. Collapsing versions at flush
  time like compaction does took scans from 19K to 151K.

## Not done

- Writers queue behind a single leader (group commit shares the fsync, but
  doesn't parallelize).
- Scans are still slower than LevelDB's.
- Compression is off by default because it costs about a third of read speed.

```
include/lsm/  public headers      tests/  GoogleTest suite
src/          implementation      bench/  YCSB-style benchmark + LevelDB adapter
results/      raw benchmark output tools/  crash harness
```
