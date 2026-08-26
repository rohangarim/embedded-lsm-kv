#include "lsm/compression.h"

#include <cstring>

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
  }
  return false;
}

const char* CompressionName(CompressionType type) {
  switch (type) {
    case CompressionType::kNoCompression: return "none";
    case CompressionType::kZlib: return "zlib";
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
  }
  return Status::Corruption("unknown block compression type");
}

}  // namespace lsm
