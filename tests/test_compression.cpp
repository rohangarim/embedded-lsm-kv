#include "lsm/compression.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <memory>
#include <random>
#include <string>

#include "lsm/db.h"
#include "lsm/sstable.h"
#include "test_util.h"

namespace lsm {
namespace {

using testing_support::Key;
using testing_support::ScopedTempDir;
using testing_support::Value;

// Text-like: compresses well, as a real value payload usually would.
std::string Compressible(size_t n) {
  const std::string unit = "the quick brown fox jumps over the lazy dog; ";
  std::string out;
  while (out.size() < n) out += unit;
  out.resize(n);
  return out;
}

// Random bytes: the case where compression must recognise it is not helping.
std::string Incompressible(size_t n, uint32_t seed) {
  std::mt19937 rng(seed);
  std::string out(n, '\0');
  for (size_t i = 0; i < n; ++i) out[i] = static_cast<char>(rng());
  return out;
}

Options CompressedOptions(CompressionType type) {
  Options options;
  options.compression = type;
  options.memtable_size_bytes = 64 * 1024;
  options.block_size_bytes = 4096;
  options.max_bytes_for_level_base = 512 * 1024;
  options.target_file_size = 256 * 1024;
  options.l0_compaction_trigger = 3;
  options.sync_policy = SyncPolicy::kNever;
  options.background_compaction = false;
  return options;
}

std::unique_ptr<DB> OpenDB(const std::string& path, const Options& options) {
  std::unique_ptr<DB> db;
  const Status s = DB::Open(options, path, &db);
  EXPECT_TRUE(s.ok()) << s.ToString();
  return db;
}

uint64_t TreeBytes(DB* db) {
  const DbStats stats = db->GetStats();
  return stats.flush_output_bytes + stats.compaction_output_bytes;
}

}  // namespace

TEST(Compression, NoCompressionRoundTrips) {
  std::string out;
  CompressionType type;
  const std::string input = Compressible(4096);
  EXPECT_FALSE(
      CompressBlock(CompressionType::kNoCompression, input, &out, &type));
  EXPECT_EQ(type, CompressionType::kNoCompression);

  std::string plain;
  ASSERT_TRUE(UncompressBlock(CompressionType::kNoCompression, input, &plain).ok());
  EXPECT_EQ(plain, input);
}

TEST(Compression, ZlibRoundTripsAcrossSizes) {
  if (!CompressionSupported(CompressionType::kZlib)) GTEST_SKIP() << "no zlib";

  for (const size_t n : {1u, 2u, 100u, 4095u, 4096u, 65536u, 300000u}) {
    const std::string input = Compressible(n);
    std::string packed;
    CompressionType type;
    if (!CompressBlock(CompressionType::kZlib, input, &packed, &type)) continue;
    ASSERT_EQ(type, CompressionType::kZlib) << n;

    std::string plain;
    ASSERT_TRUE(UncompressBlock(type, packed, &plain).ok()) << n;
    EXPECT_EQ(plain, input) << n;
  }
}

TEST(Compression, BinaryDataRoundTrips) {
  if (!CompressionSupported(CompressionType::kZlib)) GTEST_SKIP() << "no zlib";
  // Repetitive binary, including embedded NULs, so it compresses but is not text.
  std::string input;
  for (int i = 0; i < 2000; ++i) input += std::string("\0\1\2\0\xff", 5);

  std::string packed;
  CompressionType type;
  ASSERT_TRUE(CompressBlock(CompressionType::kZlib, input, &packed, &type));
  std::string plain;
  ASSERT_TRUE(UncompressBlock(type, packed, &plain).ok());
  EXPECT_EQ(plain, input);
}

// Compressing random bytes makes them bigger. The writer has to notice and
// store the block raw instead, or incompressible data would cost space.
TEST(Compression, DeclinesWhenItWouldNotHelp) {
  if (!CompressionSupported(CompressionType::kZlib)) GTEST_SKIP() << "no zlib";
  const std::string input = Incompressible(8192, 99);
  std::string packed;
  CompressionType type;
  EXPECT_FALSE(CompressBlock(CompressionType::kZlib, input, &packed, &type));
  EXPECT_EQ(type, CompressionType::kNoCompression);
}

TEST(Compression, RejectsGarbageInsteadOfReturningIt) {
  if (!CompressionSupported(CompressionType::kZlib)) GTEST_SKIP() << "no zlib";
  const std::string junk = Incompressible(512, 7);
  std::string plain;
  EXPECT_FALSE(UncompressBlock(CompressionType::kZlib, junk, &plain).ok());
}

TEST(Compression, RejectsAnUnknownType) {
  std::string plain;
  const auto bogus = static_cast<CompressionType>(200);
  EXPECT_FALSE(UncompressBlock(bogus, "whatever", &plain).ok());
  EXPECT_FALSE(CompressionSupported(bogus));
}

// ------------------------------------------------------------- DB integration

TEST(Compression, ShrinksTheTreeAndReadsBackIdentically) {
  if (!CompressionSupported(CompressionType::kZlib)) GTEST_SKIP() << "no zlib";

  constexpr int kKeys = 20000;
  // ASSERT_* returns void, so this lambda uses EXPECT_* throughout.
  auto run = [&](CompressionType type) -> uint64_t {
    ScopedTempDir dir(type == CompressionType::kZlib ? "cmp_on" : "cmp_off");
    auto db = OpenDB(dir.File("db"), CompressedOptions(type));
    for (int i = 0; i < kKeys; ++i) {
      EXPECT_TRUE(db->Put(WriteOptions(), Key(i), Compressible(200)).ok());
    }
    EXPECT_TRUE(db->CompactAll().ok());

    // Everything must read back byte-identical regardless of encoding.
    std::string value;
    for (int i = 0; i < kKeys; i += 3) {
      EXPECT_TRUE(db->Get(ReadOptions(), Key(i), &value).ok()) << i;
      EXPECT_EQ(value, Compressible(200)) << i;
    }
    return TreeBytes(db.get());
  };

  const uint64_t raw = run(CompressionType::kNoCompression);
  const uint64_t packed = run(CompressionType::kZlib);
  std::printf("[compression] tree bytes: %llu raw -> %llu zlib (%.1f%% smaller)\n",
              (unsigned long long)raw, (unsigned long long)packed,
              100.0 * (1.0 - static_cast<double>(packed) /
                                 static_cast<double>(raw)));
  EXPECT_LT(packed, raw / 2) << "text-like values should compress well";
}

TEST(Compression, IncompressibleDataDoesNotInflateTheTree) {
  if (!CompressionSupported(CompressionType::kZlib)) GTEST_SKIP() << "no zlib";

  constexpr int kKeys = 8000;
  auto run = [&](CompressionType type) -> uint64_t {
    ScopedTempDir dir("cmp_random");
    auto db = OpenDB(dir.File("db"), CompressedOptions(type));
    for (int i = 0; i < kKeys; ++i) {
      EXPECT_TRUE(
          db->Put(WriteOptions(), Key(i), Incompressible(200, i)).ok());
    }
    EXPECT_TRUE(db->CompactAll().ok());
    std::string value;
    for (int i = 0; i < kKeys; i += 5) {
      EXPECT_TRUE(db->Get(ReadOptions(), Key(i), &value).ok()) << i;
      EXPECT_EQ(value, Incompressible(200, i)) << i;
    }
    return TreeBytes(db.get());
  };

  const uint64_t raw = run(CompressionType::kNoCompression);
  const uint64_t packed = run(CompressionType::kZlib);
  // Blocks fall back to raw, so the only cost is the attempt -- not bytes.
  EXPECT_LE(packed, raw + raw / 100)
      << "raw=" << raw << " packed=" << packed;
}

TEST(Compression, SurvivesReopen) {
  if (!CompressionSupported(CompressionType::kZlib)) GTEST_SKIP() << "no zlib";
  ScopedTempDir dir("cmp_reopen");
  const std::string path = dir.File("db");
  constexpr int kKeys = 5000;
  {
    auto db = OpenDB(path, CompressedOptions(CompressionType::kZlib));
    for (int i = 0; i < kKeys; ++i) {
      ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Compressible(150)).ok());
    }
    ASSERT_TRUE(db->CompactAll().ok());
  }
  auto db = OpenDB(path, CompressedOptions(CompressionType::kZlib));
  std::string value;
  for (int i = 0; i < kKeys; ++i) {
    ASSERT_TRUE(db->Get(ReadOptions(), Key(i), &value).ok()) << i;
    EXPECT_EQ(value, Compressible(150)) << i;
  }
}

// A file written with compression is readable by a database configured without
// it: the type travels with each block, not in the options.
TEST(Compression, SettingOnlyAffectsNewWrites) {
  if (!CompressionSupported(CompressionType::kZlib)) GTEST_SKIP() << "no zlib";
  ScopedTempDir dir("cmp_mixed");
  const std::string path = dir.File("db");
  constexpr int kKeys = 4000;
  {
    auto db = OpenDB(path, CompressedOptions(CompressionType::kZlib));
    for (int i = 0; i < kKeys; ++i) {
      ASSERT_TRUE(db->Put(WriteOptions(), Key(i), Compressible(150)).ok());
    }
    ASSERT_TRUE(db->CompactAll().ok());
  }
  auto db = OpenDB(path, CompressedOptions(CompressionType::kNoCompression));
  std::string value;
  for (int i = 0; i < kKeys; i += 3) {
    ASSERT_TRUE(db->Get(ReadOptions(), Key(i), &value).ok()) << i;
    EXPECT_EQ(value, Compressible(150)) << i;
  }
  // New writes land uncompressed alongside the old compressed blocks.
  ASSERT_TRUE(db->Put(WriteOptions(), "fresh", Compressible(150)).ok());
  ASSERT_TRUE(db->CompactAll().ok());
  ASSERT_TRUE(db->Get(ReadOptions(), "fresh", &value).ok());
  EXPECT_EQ(value, Compressible(150));
}

TEST(Compression, CorruptedCompressedBlockIsDetected) {
  if (!CompressionSupported(CompressionType::kZlib)) GTEST_SKIP() << "no zlib";
  ScopedTempDir dir("cmp_corrupt");
  Options options;
  options.compression = CompressionType::kZlib;
  options.block_size_bytes = 4096;
  const std::string path = dir.File("000001.sst");

  {
    TableBuilder builder(options);
    ASSERT_TRUE(builder.Open(path).ok());
    for (int i = 0; i < 2000; ++i) {
      ASSERT_TRUE(builder
                      .Add(EncodeInternalKey(Key(i), i + 1, ValueType::kValue),
                           Compressible(120))
                      .ok());
    }
    ASSERT_TRUE(builder.Finish().ok());
  }

  const int fd = ::open(path.c_str(), O_RDWR);
  ASSERT_GE(fd, 0);
  char byte;
  ASSERT_EQ(::pread(fd, &byte, 1, 64), 1);
  byte = static_cast<char>(byte ^ 0xff);
  ASSERT_EQ(::pwrite(fd, &byte, 1, 64), 1);
  ::close(fd);

  std::unique_ptr<Table> table;
  ASSERT_TRUE(Table::Open(path, options, &table).ok());
  std::string value;
  Status status;
  // Either the CRC rejects it or the bloom filter never gets there. What must
  // not happen is a wrong value coming back.
  if (table->Get(Key(0), kMaxSequenceNumber, &value, &status)) {
    if (status.ok()) EXPECT_EQ(value, Compressible(120));
  }
}

}  // namespace lsm
