#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "lsm/status.h"

namespace lsm {

// How a data block is stored on disk. The type is written next to each block
// rather than in the footer, so a file can hold a mix -- which it does, because
// a block that does not compress well is stored raw.
enum class CompressionType : uint8_t {
  kNoCompression = 0,
  kZlib = 1,
  // A small LZ77 codec built into the library. Compresses less than zlib but
  // decompresses several times faster, which is the trade that matters when
  // every block read pays for it. See src/compression.cpp for the format.
  kFastLz = 2,
};

// Compresses `input`. Returns false when the result would not be worth using,
// leaving it to the caller to store the block raw; `*type` reports what was
// actually produced.
bool CompressBlock(CompressionType requested, std::string_view input,
                   std::string* output, CompressionType* type);

// Reverses CompressBlock. Fails rather than guessing if this build has no
// codec for `type` -- a file written elsewhere must not be silently misread.
Status UncompressBlock(CompressionType type, std::string_view input,
                       std::string* output);

// Whether this build can handle `type` at all.
bool CompressionSupported(CompressionType type);

const char* CompressionName(CompressionType type);

}  // namespace lsm
