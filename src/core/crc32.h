// SPDX-License-Identifier: MIT
/*
 * Standard CRC-32/ISO-HDLC (zlib-compatible) for integrity checks.
 *
 * Distinct from ulCrc32 (core/ulid.h): that one reconstructs the original
 * USBUtil's game-id hash (polynomial 0x04C11DB7, MSB-first, init 0, no
 * final xor) and must never change. This one is the conventional
 * checksum (polynomial 0xEDB88320 reflected, init and xorout 0xFFFFFFFF)
 * used by the USRRIP03 sidecar, with zlib crc32() chaining semantics:
 *
 *     uint32_t c = crc32(0, data1, n1);   // fresh state, first chunk
 *     c = crc32(c, data2, n2);            // feed the rest in order
 *
 * Known vectors: crc32(0, "", 0) == 0x00000000,
 *                crc32(0, "123456789", 9) == 0xCBF43926.
 */
#pragma once

#include <cstdint>
#include <cstddef>

namespace usbutils {

/// Continue (or start, when `crc` is 0) a CRC-32 over `len` bytes.
uint32_t crc32(uint32_t crc, const void* data, size_t len);

} // namespace usbutils
