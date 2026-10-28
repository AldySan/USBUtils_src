// SPDX-License-Identifier: MIT
/*
 * Game id computation — reconstructed from the original binary:
 *   InitCRCTable    @0x398738  (poly 0x04C11DB7, MSB-first, init 0)
 *   ComputeGameId   @0x398784  (CRC32 over Trim(name)+NUL, IntToHex(crc,8))
 */
#pragma once

#include <string>
#include <cstdint>

namespace usbutils {

/// Non-standard CRC-32: polynomial 0x04C11DB7, MSB-first, init 0,
/// no reflection, no final xor. Table generated exactly like 0x398738.
uint32_t ulCrc32(const void* data, size_t len);

/// Original behaviour: CRC over Trim(name) plus one trailing NUL byte,
/// rendered as 8 uppercase hex digits.
std::string gameIdHex(const std::string& gameName);

} // namespace usbutils
