#pragma once
// Tiny liblzma (xz) buffer wrappers for the RAR dungeon transport. lzma compresses the RAW
// (un-gzipped) serialized dungeon ~4x smaller than the game's gzip. The 8-byte little-endian
// raw size is prepended so decompression knows the output length. Empty string == failure.
#include <string>
#include <cstdint>
#include <lzma.h>

inline std::string rarLzmaCompress(const std::string& in, uint32_t preset = 6) {
  size_t bound = lzma_stream_buffer_bound(in.size());
  std::string out(8 + bound, '\0');
  uint64_t rawSize = in.size();
  for (int i = 0; i < 8; ++i)
    out[i] = (char) ((rawSize >> (8 * i)) & 0xff);
  size_t outPos = 0;
  lzma_ret r = lzma_easy_buffer_encode(preset, LZMA_CHECK_CRC32, nullptr,
      (const uint8_t*) in.data(), in.size(),
      (uint8_t*) &out[8], &outPos, bound);
  if (r != LZMA_OK)
    return "";
  out.resize(8 + outPos);
  return out;
}

inline std::string rarLzmaDecompress(const std::string& in) {
  if (in.size() < 8)
    return "";
  uint64_t rawSize = 0;
  for (int i = 0; i < 8; ++i)
    rawSize |= ((uint64_t) (unsigned char) in[i]) << (8 * i);
  std::string out(rawSize, '\0');
  uint64_t memlimit = UINT64_MAX;
  size_t inPos = 8, outPos = 0;
  lzma_ret r = lzma_stream_buffer_decode(&memlimit, 0, nullptr,
      (const uint8_t*) in.data(), &inPos, in.size(),
      (uint8_t*) (rawSize ? &out[0] : nullptr), &outPos, rawSize);
  if (r != LZMA_OK)
    return "";
  out.resize(outPos);
  return out;
}
