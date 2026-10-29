// SPDX-License-Identifier: MIT
/*
 * Minimal ISO9660 reader — reconstructed from the original's ISOUnit /
 * jISOImage / ISO9660* units and ParseSystemCnf @0x3A0F10:
 *   - locate SYSTEM.CNF in the root directory (LBA, size)
 *   - byte offset = LBA * 2048
 *   - BOOT2 = cdrom0:\SLUS_202.65;1  -> serial "SLUS_202.65"
 *
 * walkIso() is a generic directory-tree walker that works over any
 * random-access byte source (a plain ISO file, or a game's chunk stream
 * via GameStream) — it powers serial extraction, the video ripper's
 * scanner and the exact IOPRP locator used by DNAS patching.
 */
#pragma once

#include <string>
#include <cstdint>
#include <vector>
#include <functional>

namespace usbutils {

struct IsoInfo {
    bool        valid = false;
    uint32_t    volumeSize = 0;      ///< in 2048-byte sectors
    std::string volumeId;            ///< primary volume descriptor ID
    std::string systemCnf;           ///< raw SYSTEM.CNF content (up to 255 B)
    std::string serial;              ///< e.g. "SLUS_202.65"
    std::string bootElf;             ///< full BOOT2 value, trimmed of ";1"
};

/// Random-access byte source: read `n` bytes at absolute `offset`.
/// Returns false on error/short read.
using IsoReadFn = std::function<bool(uint64_t offset, void* dst, size_t n)>;

/// One ISO9660 directory-record entry (file or directory).
struct IsoEntry {
    std::string path;                ///< full path, '\\'-separated, ";N" stripped
    std::string name;                ///< base name (no directory part)
    uint32_t    lba = 0;             ///< extent start, in 2048-byte sectors
    uint64_t    size = 0;            ///< extent size in bytes
    bool        isDir = false;       ///< directory-record flag bit 1
};

/// Walk the whole ISO9660 directory tree depth-first, starting at the
/// root record of the primary volume descriptor. `onEntry` is called for
/// every file AND directory (except "." and ".."); returning false from
/// `onEntry` stops the walk early (not an error). Returns false only on
/// a fatal condition (unreadable PVD / not ISO9660).
bool walkIso(const IsoReadFn& read,
             const std::function<bool(const IsoEntry&)>& onEntry);

/// Parse the ISO header + SYSTEM.CNF of `path` (a .iso file).
/// Works on plain ISO9660 images (PS2 discs are ISO9660 + sometimes UDF
/// hybrid; the serial is always reachable via the ISO9660 path table).
IsoInfo readIsoInfo(const std::string& path);

/// Read just the Primary Volume Descriptor through a generic reader —
/// used for raw CD/DVD device probing (the drive is opened by the caller;
/// sector 16 is read to obtain volume id + data size in 2048-byte sectors).
/// Returns false when the medium is not ISO9660.
bool readPvd(const IsoReadFn& read, std::string* volumeId,
             uint32_t* volumeSectors);

/// Canonicalize a serial: strip ";1" / trailing ".1", pad with '0' to 11
/// (normalization observed at 0x3A1072..0x3A109C).
std::string normalizeSerial(const std::string& raw);

/// Extract the boot ELF file name from a SYSTEM.CNF "BOOT2 = ..." line
/// (ParseSystemCnf @0x3A0F10). Handles "cdrom0:\SLUS_202.65;1" and bare
/// "SLUS_202.65" forms. Empty when no BOOT2 line exists.
std::string bootElfFromCnf(const std::string& cnf);

} // namespace usbutils
