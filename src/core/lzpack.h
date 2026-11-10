// SPDX-License-Identifier: MIT
/*
 * Block-LZSS codec for the media-rip sidecar backup (USRRIP02 payload).
 *
 * The core library has no external dependencies, so this is a small
 * in-house codec instead of zlib. It only has to do two things well:
 * shrink PS2 ADPCM audio (long zero runs, repeated predictor frames) and
 * never make MPEG-2 video (already entropy-coded) grow. A block that
 * does not get smaller is stored verbatim, so incompressible payloads
 * cost a few bytes per MiB of overhead instead of a 12% LZSS blowup.
 *
 * Block stream (little-endian):
 *   uint32 hdr   bit 31 = 1 -> LZSS data (uint32 compLen follows),
 *                bit 31 = 0 -> stored data; bits 0-30 = raw length
 *                of the block (1..kLzBlockSize)
 *   data         compLen LZSS bytes, or `rawLen` stored bytes
 *
 * LZSS stream inside a compressed block:
 *   one control byte, then up to 8 units (LSB first, 1 = literal):
 *     literal -> 1 byte
 *     match   -> uint16 back offset (1..65535, inside the same block)
 *                uint8  (length - kLzMinMatch)
 *   Matches never cross block boundaries. The decoder works directly in
 *   the output buffer and rejects any offset reaching before the block
 *   start, so a corrupt sidecar fails loudly instead of writing garbage.
 */
#pragma once

#include <cstdint>
#include <vector>

namespace usbutils {

constexpr uint32_t kLzBlockSize = 1u << 20;   ///< raw bytes per block (1 MiB)
constexpr uint16_t kLzMaxOffset = 65535;
constexpr uint16_t kLzMinMatch  = 4;
constexpr uint16_t kLzMaxMatch  = kLzMinMatch + 255;

/// LZSS-encode `rawLen` bytes and append the stream to `out`. Returns
/// false without touching `out` when the encoding would not be smaller
/// than the input — the caller stores that block verbatim.
bool lzEncodeBlock(const uint8_t* src, size_t rawLen,
                   std::vector<uint8_t>& out);

/// Decode `srcLen` LZSS bytes into `dst`, filling exactly `rawLen` bytes.
/// Returns false on malformed input (bad offset or length, short stream).
bool lzDecodeBlock(const uint8_t* src, size_t srcLen,
                   uint8_t* dst, size_t rawLen);

} // namespace usbutils
