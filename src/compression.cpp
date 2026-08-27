#include "lsm/compression.h"

#include <cstring>
#include <vector>

#include "lsm/coding.h"

#ifdef LSM_HAVE_ZLIB
#include <zlib.h>
#endif

namespace lsm {
namespace {

// Only adopt a compressed block if it saves at least an eighth. Below that the
// decompression cost on every read is not repaid, and the block is stored raw.
constexpr double kMinimumSavings = 0.125;

#ifdef LSM_HAVE_ZLIB
// Raw deflate, no zlib or gzip wrapper: the block already has its own length
// and checksum, so a second framing would be pure overhead.
constexpr int kWindowBits = -15;
constexpr int kLevel = 6;

bool ZlibCompress(std::string_view input, std::string* output) {
  z_stream stream{};
  if (::deflateInit2(&stream, kLevel, Z_DEFLATED, kWindowBits, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
    return false;
  }
  output->resize(::deflateBound(&stream, static_cast<uLong>(input.size())));

  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
  stream.avail_in = static_cast<uInt>(input.size());
  stream.next_out = reinterpret_cast<Bytef*>(&(*output)[0]);
  stream.avail_out = static_cast<uInt>(output->size());

  const int rc = ::deflate(&stream, Z_FINISH);
  const uLong produced = stream.total_out;
  ::deflateEnd(&stream);
  if (rc != Z_STREAM_END) return false;
  output->resize(produced);
  return true;
}

Status ZlibUncompress(std::string_view input, std::string* output) {
  z_stream stream{};
  if (::inflateInit2(&stream, kWindowBits) != Z_OK) {
    return Status::Corruption("could not start decompression");
  }
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
  stream.avail_in = static_cast<uInt>(input.size());

  // Blocks are bounded by block_size_bytes, but a caller can configure that
  // upward, so grow rather than assume.
  size_t capacity = input.size() * 4 + 1024;
  output->resize(capacity);
  size_t produced = 0;
  while (true) {
    stream.next_out = reinterpret_cast<Bytef*>(&(*output)[produced]);
    stream.avail_out = static_cast<uInt>(capacity - produced);
    const int rc = ::inflate(&stream, Z_NO_FLUSH);
    produced = capacity - stream.avail_out;
    if (rc == Z_STREAM_END) break;
    if (rc != Z_OK) {
      ::inflateEnd(&stream);
      return Status::Corruption("corrupt compressed block");
    }
    if (stream.avail_out == 0) {
      capacity *= 2;
      output->resize(capacity);
      continue;
    }
    if (stream.avail_in == 0) {
      ::inflateEnd(&stream);
      return Status::Corruption("truncated compressed block");
    }
  }
  ::inflateEnd(&stream);
  output->resize(produced);
  return Status::OK();
}
#endif  // LSM_HAVE_ZLIB

// ---------------------------------------------------------------- kFastLz
//
// A small LZ77 coder. zlib gives a better ratio but decompresses at a few
// hundred MB/s, and with a block cache in front of it every miss pays that; the
// measured cost was more than half of read throughput. This trades ratio for a
// decompressor that is essentially two memcpy loops.
//
// Format: a varint32 of the uncompressed length, then a run of sequences.
//
//   sequence: [token:1][extended literal length...][literal bytes]
//             [match offset:2 LE][extended match length...]
//
// The token packs two nibbles: literal count in the high four bits, match
// length minus four in the low four. A nibble of 15 means "add the following
// bytes until one is not 255". The final sequence stops after its literals --
// there is no trailing match -- which is how the decoder knows it is done.
//
// Matches are found with a hash table over four-byte sequences. It keeps one
// candidate per hash, so a collision just means a missed match, never a wrong
// one: correctness never depends on the table, only the ratio does.
namespace fastlz {

constexpr int kHashBits = 16;
constexpr size_t kHashSize = 1u << kHashBits;
constexpr uint32_t kMinMatch = 4;
// Offsets are 16-bit, so a match can only reach this far back.
constexpr size_t kMaxOffset = 65535;
// The tail is emitted as literals: a match needs four bytes to compare and two
// more to be worth encoding, so there is nothing to gain down here.
constexpr size_t kTailLiterals = 12;

inline uint32_t Load32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

inline uint32_t Hash(uint32_t sequence) {
  return (sequence * 2654435761u) >> (32 - kHashBits);
}

void EmitLength(std::string* out, size_t remaining) {
  while (remaining >= 255) {
    out->push_back(static_cast<char>(255));
    remaining -= 255;
  }
  out->push_back(static_cast<char>(remaining));
}

void Compress(std::string_view input, std::string* output) {
  output->clear();
  PutVarint32(output, static_cast<uint32_t>(input.size()));

  const auto* base = reinterpret_cast<const uint8_t*>(input.data());
  const size_t n = input.size();

  // Emits everything from `anchor` as a literal-only sequence, which is also
  // how every stream ends.
  auto emit_final_literals = [&](size_t anchor) {
    const size_t literals = n - anchor;
    output->push_back(
        static_cast<char>((literals >= 15 ? 15 : literals) << 4));
    if (literals >= 15) EmitLength(output, literals - 15);
    if (literals > 0) output->append(input.data() + anchor, literals);
  };

  if (n < kMinMatch + kTailLiterals) {  // Too short for a match to pay off.
    emit_final_literals(0);
    return;
  }

  std::vector<uint32_t> table(kHashSize, 0);

  size_t anchor = 0;  // Start of the pending literal run.
  size_t ip = 1;      // Position 0 can never be a match target.
  const size_t limit = n - kTailLiterals;

  while (ip < limit) {
    const uint32_t sequence = Load32(base + ip);
    const uint32_t h = Hash(sequence);
    const size_t candidate = table[h];
    table[h] = static_cast<uint32_t>(ip);

    // A candidate of 0 means "unset"; position 0 is excluded above so this is
    // unambiguous.
    if (candidate == 0 || ip - candidate > kMaxOffset ||
        Load32(base + candidate) != sequence) {
      ++ip;
      continue;
    }

    // Extend the match forward as far as the input allows.
    size_t match = kMinMatch;
    while (ip + match < n && base[candidate + match] == base[ip + match]) ++match;

    const size_t literals = ip - anchor;
    const size_t match_extra = match - kMinMatch;
    const uint8_t token =
        static_cast<uint8_t>(((literals >= 15 ? 15 : literals) << 4) |
                             (match_extra >= 15 ? 15 : match_extra));
    output->push_back(static_cast<char>(token));
    if (literals >= 15) EmitLength(output, literals - 15);
    if (literals > 0) output->append(input.data() + anchor, literals);

    const uint16_t offset = static_cast<uint16_t>(ip - candidate);
    output->push_back(static_cast<char>(offset & 0xff));
    output->push_back(static_cast<char>(offset >> 8));
    if (match_extra >= 15) EmitLength(output, match_extra - 15);

    ip += match;
    anchor = ip;
  }

  emit_final_literals(anchor);
}

// Every bound is checked. The block already carries a CRC, but a decompressor
// that can be walked off the end of a buffer by malformed input is a liability
// regardless of what is supposed to guard it.
Status Uncompress(std::string_view input, std::string* output) {
  const char* p = input.data();
  const char* const end = p + input.size();

  uint32_t expected = 0;
  p = GetVarint32Ptr(p, end, &expected);
  if (p == nullptr) return Status::Corruption("fastlz: bad length header");

  output->clear();
  output->reserve(expected);

  auto read_extended = [&](size_t* value) -> bool {
    while (true) {
      if (p >= end) return false;
      const uint8_t byte = static_cast<uint8_t>(*p++);
      *value += byte;
      if (byte != 255) return true;
    }
  };

  while (p < end) {
    const uint8_t token = static_cast<uint8_t>(*p++);
    size_t literals = token >> 4;
    if (literals == 15 && !read_extended(&literals)) {
      return Status::Corruption("fastlz: truncated literal length");
    }
    if (static_cast<size_t>(end - p) < literals) {
      return Status::Corruption("fastlz: literal run runs past the block");
    }
    output->append(p, literals);
    p += literals;

    if (p == end) break;  // Final sequence: literals only.
    if (end - p < 2) return Status::Corruption("fastlz: truncated match offset");
    const uint16_t offset = static_cast<uint16_t>(
        static_cast<uint8_t>(p[0]) | (static_cast<uint8_t>(p[1]) << 8));
    p += 2;
    if (offset == 0 || offset > output->size()) {
      return Status::Corruption("fastlz: match offset outside the output");
    }

    size_t match = token & 15;
    if (match == 15 && !read_extended(&match)) {
      return Status::Corruption("fastlz: truncated match length");
    }
    match += kMinMatch;

    // Byte at a time, because a match may overlap itself -- offset 1 with
    // length 30 is a run of the same byte, and that is the point.
    size_t from = output->size() - offset;
    for (size_t i = 0; i < match; ++i) output->push_back((*output)[from + i]);
  }

  if (output->size() != expected) {
    return Status::Corruption("fastlz: decompressed to the wrong length");
  }
  return Status::OK();
}

}  // namespace fastlz

}  // namespace

bool CompressionSupported(CompressionType type) {
  switch (type) {
    case CompressionType::kNoCompression:
      return true;
    case CompressionType::kZlib:
#ifdef LSM_HAVE_ZLIB
      return true;
#else
      return false;
#endif
    case CompressionType::kFastLz:
      return true;  // Built in; no external codec to be missing.
  }
  return false;
}

const char* CompressionName(CompressionType type) {
  switch (type) {
    case CompressionType::kNoCompression: return "none";
    case CompressionType::kZlib: return "zlib";
    case CompressionType::kFastLz: return "fastlz";
  }
  return "unknown";
}

bool CompressBlock(CompressionType requested, std::string_view input,
                   std::string* output, CompressionType* type) {
  *type = CompressionType::kNoCompression;
  if (requested == CompressionType::kNoCompression || input.empty()) return false;

#ifdef LSM_HAVE_ZLIB
  if (requested == CompressionType::kZlib) {
    if (!ZlibCompress(input, output)) return false;
    const double ratio = static_cast<double>(output->size()) /
                         static_cast<double>(input.size());
    if (ratio > 1.0 - kMinimumSavings) return false;  // Not worth it; store raw.
    *type = CompressionType::kZlib;
    return true;
  }
#endif
  if (requested == CompressionType::kFastLz) {
    fastlz::Compress(input, output);
    const double ratio =
        static_cast<double>(output->size()) / static_cast<double>(input.size());
    if (ratio > 1.0 - kMinimumSavings) return false;  // Not worth it; store raw.
    *type = CompressionType::kFastLz;
    return true;
  }
  return false;
}

Status UncompressBlock(CompressionType type, std::string_view input,
                       std::string* output) {
  switch (type) {
    case CompressionType::kNoCompression:
      output->assign(input.data(), input.size());
      return Status::OK();
    case CompressionType::kZlib:
#ifdef LSM_HAVE_ZLIB
      return ZlibUncompress(input, output);
#else
      return Status::NotSupported(
          "block is zlib-compressed but this build has no zlib");
#endif
    case CompressionType::kFastLz:
      return fastlz::Uncompress(input, output);
  }
  return Status::Corruption("unknown block compression type");
}

}  // namespace lsm
