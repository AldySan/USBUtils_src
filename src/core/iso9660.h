// SPDX-License-Identifier: MIT
/*
 * Minimal ISO9660 reader — reconstructed from the original's ISOUnit /
 * jISOImage / ISO9660* units and ParseSystemCnf @0x3A0F10:
 *   - locate SYSTEM.CNF in the root directory (LBA, size)
 *   - byte offset = LBA * 2048
 *   - BOOT2 = cdrom0:\SLUS_202.65;1  -> serial "SLUS_202.65"
 */
#pragma once

#include <string>
#include <cstdint>
#include <vector>

namespace usbutils {

struct IsoInfo {
    bool        valid = false;
    uint32_t    volumeSize = 0;      ///< in 2048-byte sectors
    std::string volumeId;            ///< primary volume descriptor ID
    std::string systemCnf;           ///< raw SYSTEM.CNF content (up to 255 B)
    std::string serial;              ///< e.g. "SLUS_202.65"
    std::string bootElf;             ///< full BOOT2 value, trimmed of ";1"
};

/// Parse the ISO header + SYSTEM.CNF of `path` (a .iso file).
/// Works on plain ISO9660 images (PS2 discs are ISO9660 + sometimes UDF
/// hybrid; the serial is always reachable via the ISO9660 path table).
IsoInfo readIsoInfo(const std::string& path);

/// Read one 2048-byte sector from a file. Returns false on EOF/error.
bool readSector(const std::string& path, uint32_t lba, uint8_t* out2048);

/// Canonicalize a serial: strip ";1" / trailing ".1", pad with '0' to 11
/// (normalization observed at 0x3A1072..0x3A109C).
std::string normalizeSerial(const std::string& raw);

} // namespace usbutils
