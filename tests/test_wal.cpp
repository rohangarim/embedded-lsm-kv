#include "lsm/wal.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <string>
#include <tuple>
#include <vector>

#include "test_util.h"

namespace lsm {
namespace {

using Record = std::tuple<SequenceNumber, ValueType, std::string, std::string>;

std::vector<Record> ReplayAll(const std::string& path, bool* truncated = nullptr) {
  std::vector<Record> records;
  const Status s = WalReader::Replay(
      path,
      [&](SequenceNumber seq, ValueType type, std::string_view key,
          std::string_view value) {
        records.emplace_back(seq, type, std::string(key), std::string(value));
      },
      truncated);
  EXPECT_TRUE(s.ok()) << s.ToString();
  return records;
}

void TruncateFile(const std::string& path, off_t bytes_to_remove) {
  struct stat st;
  ASSERT_EQ(::stat(path.c_str(), &st), 0);
  ASSERT_EQ(::truncate(path.c_str(), st.st_size - bytes_to_remove), 0);
}

}  // namespace

TEST(Wal, ReplayOfMissingFileIsEmptyAndNotAnError) {
  testing_support::ScopedTempDir dir("wal_missing");
  const auto records = ReplayAll(dir.File("nope.log"));
  EXPECT_TRUE(records.empty());
}

TEST(Wal, RoundTripsRecords) {
  testing_support::ScopedTempDir dir("wal_roundtrip");
  const std::string path = dir.File("000001.log");

  {
    WalWriter writer;
    ASSERT_TRUE(writer.Open(path).ok());
    ASSERT_TRUE(writer.AddRecord(1, ValueType::kValue, "a", "one").ok());
    ASSERT_TRUE(writer.AddRecord(2, ValueType::kDeletion, "b", "").ok());
    ASSERT_TRUE(writer.AddRecord(3, ValueType::kValue, std::string("\0k", 2),
                                 std::string("\0v\xff", 3))
                    .ok());
    ASSERT_TRUE(writer.Sync().ok());
    ASSERT_TRUE(writer.Close().ok());
  }

  const auto records = ReplayAll(path);
  ASSERT_EQ(records.size(), 3u);
  EXPECT_EQ(std::get<0>(records[0]), 1u);
  EXPECT_EQ(std::get<1>(records[0]), ValueType::kValue);
  EXPECT_EQ(std::get<2>(records[0]), "a");
  EXPECT_EQ(std::get<3>(records[0]), "one");

  EXPECT_EQ(std::get<1>(records[1]), ValueType::kDeletion);
  EXPECT_EQ(std::get<3>(records[1]), "");

  EXPECT_EQ(std::get<2>(records[2]), std::string("\0k", 2));
  EXPECT_EQ(std::get<3>(records[2]), std::string("\0v\xff", 3));
}

TEST(Wal, ReopenAppendsRatherThanTruncates) {
  testing_support::ScopedTempDir dir("wal_append");
  const std::string path = dir.File("000001.log");
  {
    WalWriter writer;
    ASSERT_TRUE(writer.Open(path).ok());
    ASSERT_TRUE(writer.AddRecord(1, ValueType::kValue, "a", "1").ok());
    ASSERT_TRUE(writer.Close().ok());
  }
  {
    WalWriter writer;
    ASSERT_TRUE(writer.Open(path).ok());
    ASSERT_TRUE(writer.AddRecord(2, ValueType::kValue, "b", "2").ok());
    ASSERT_TRUE(writer.Close().ok());
  }
  EXPECT_EQ(ReplayAll(path).size(), 2u);
}

TEST(Wal, LargeRecords) {
  testing_support::ScopedTempDir dir("wal_large");
  const std::string path = dir.File("000001.log");
  const std::string big_value(1 << 20, 'x');
  {
    WalWriter writer;
    ASSERT_TRUE(writer.Open(path).ok());
    ASSERT_TRUE(writer.AddRecord(1, ValueType::kValue, "big", big_value).ok());
    ASSERT_TRUE(writer.Close().ok());
  }
  const auto records = ReplayAll(path);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(std::get<3>(records[0]), big_value);
}

// A process killed mid-write leaves a partial record. Replay must keep every
// complete record before it and silently drop the fragment -- that write was
// never acknowledged to the caller.
TEST(Wal, TruncatedTailIsDroppedAndEarlierRecordsSurvive) {
  testing_support::ScopedTempDir dir("wal_torn");
  const std::string path = dir.File("000001.log");
  {
    WalWriter writer;
    ASSERT_TRUE(writer.Open(path).ok());
    for (int i = 0; i < 50; ++i) {
      ASSERT_TRUE(writer
                      .AddRecord(i + 1, ValueType::kValue, testing_support::Key(i),
                                 testing_support::Value(i))
                      .ok());
    }
    ASSERT_TRUE(writer.Close().ok());
  }

  // Chop off part of the last record, then part of its header.
  for (const off_t cut : {5, 40, 80}) {
    const std::string copy = path + ".cut";
    ASSERT_EQ(std::system(("cp '" + path + "' '" + copy + "'").c_str()), 0);
    TruncateFile(copy, cut);

    bool truncated = false;
    const auto records = ReplayAll(copy, &truncated);
    EXPECT_TRUE(truncated) << "cut=" << cut;
    EXPECT_LT(records.size(), 50u);
    EXPECT_GT(records.size(), 40u);
    for (size_t i = 0; i < records.size(); ++i) {
      EXPECT_EQ(std::get<2>(records[i]), testing_support::Key(static_cast<int>(i)));
      EXPECT_EQ(std::get<3>(records[i]), testing_support::Value(static_cast<int>(i)));
    }
    ::unlink(copy.c_str());
  }
}

TEST(Wal, CorruptedPayloadStopsReplayAtThatRecord) {
  testing_support::ScopedTempDir dir("wal_corrupt");
  const std::string path = dir.File("000001.log");
  {
    WalWriter writer;
    ASSERT_TRUE(writer.Open(path).ok());
    for (int i = 0; i < 10; ++i) {
      ASSERT_TRUE(writer.AddRecord(i + 1, ValueType::kValue, "key", "value").ok());
    }
    ASSERT_TRUE(writer.Close().ok());
  }

  // Flip a bit inside the fifth record's payload. Each record here is
  // 8 (header) + 8 (tag) + 4+3 (key) + 4+5 (value) = 32 bytes.
  const int fd = ::open(path.c_str(), O_RDWR);
  ASSERT_GE(fd, 0);
  char byte;
  const off_t target = 4 * 32 + 20;
  ASSERT_EQ(::pread(fd, &byte, 1, target), 1);
  byte = static_cast<char>(byte ^ 0x40);
  ASSERT_EQ(::pwrite(fd, &byte, 1, target), 1);
  ::close(fd);

  bool truncated = false;
  const auto records = ReplayAll(path, &truncated);
  EXPECT_TRUE(truncated);
  EXPECT_EQ(records.size(), 4u);  // Everything before the damaged record.
}

// The write hook is the seam the crash harness uses; verify a truncating hook
// really does produce a torn tail.
TEST(Wal, WriteHookCanTruncateARecord) {
  testing_support::ScopedTempDir dir("wal_hook");
  const std::string path = dir.File("000001.log");
  {
    WalWriter writer;
    ASSERT_TRUE(writer.Open(path).ok());
    ASSERT_TRUE(writer.AddRecord(1, ValueType::kValue, "a", "keep").ok());
    writer.SetWriteHook([](std::string* record) { record->resize(6); });
    ASSERT_TRUE(writer.AddRecord(2, ValueType::kValue, "b", "lost").ok());
    ASSERT_TRUE(writer.Close().ok());
  }
  bool truncated = false;
  const auto records = ReplayAll(path, &truncated);
  EXPECT_TRUE(truncated);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(std::get<3>(records[0]), "keep");
}

TEST(Wal, SyncOnUnopenedWriterIsAnError) {
  WalWriter writer;
  EXPECT_FALSE(writer.Sync().ok());
  EXPECT_FALSE(writer.AddRecord(1, ValueType::kValue, "a", "b").ok());
}

}  // namespace lsm
