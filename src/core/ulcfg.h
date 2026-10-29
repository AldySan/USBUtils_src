// SPDX-License-Identifier: MIT
/*
 * USBUtils — open reimplementation of USBUtil v2.2 (PS2 USBExtreme manager)
 * Copyright (c) 2025 AldySan
 * Based on the reverse-engineering of "USBUtil v2.2 rev1.0" by ISEKO (2009).
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace usbutils {

#pragma pack(push, 1)
/*
 * ul.cfg record — 64 bytes.
 * Layout derived from BuildRecord @0x39B518 of the original binary.
 */
struct UlCfgRecord {
    static constexpr size_t kSize = 64;
    static constexpr uint8_t kMediaCd  = 0x12;  ///< 18
    static constexpr uint8_t kMediaDvd = 0x14;  ///< 20

    char     name[32];    ///< 0x00 game display name (NUL padded)
    char     magic[3];    ///< 0x20 "ul."
    char     serial[11];  ///< 0x23 e.g. "SLUS_202.65" (NUL padded)
    uint8_t  pad0;        ///< 0x2E
    uint8_t  parts;       ///< 0x2F number of chunk files
    uint8_t  media;       ///< 0x30 kMediaCd / kMediaDvd
    uint8_t  pad1[4];     ///< 0x31
    uint8_t  flag;        ///< 0x35 = 8 in USBUtil-written records
    uint8_t  pad2[10];    ///< 0x36

    UlCfgRecord() { std::memset(this, 0, sizeof(*this));
                    std::memcpy(magic, "ul.", 3); flag = 8; }

    void setName(const std::string& n);
    void setSerial(const std::string& s);
    std::string nameStr() const;
    std::string serialStr() const;
    bool valid() const;
};
#pragma pack(pop)
static_assert(sizeof(UlCfgRecord) == UlCfgRecord::kSize, "record must be 64 bytes");

/// Whole ul.cfg: list of records + the folder they came from.
struct UlCfg {
    std::string dir;               ///< root of the USB device
    std::vector<UlCfgRecord> records;

    /// Load "<dir>/ul.cfg". Returns false when the file cannot be opened.
    /// A file whose size is not a multiple of 64 is tolerated (trailing
    /// partial records are kept with `valid()==false`, as the original did).
    bool load(const std::string& dir);

    /// Write records back to "<dir>/ul.cfg" atomically (write .new, then rename).
    bool save() const;

    /// Append one record (create file when missing).
    bool append(const UlCfgRecord& rec);

    /// Insert at position (like the original's rewrite-with-insert).
    bool insert(const UlCfgRecord& rec, size_t pos);

    /// Remove record and delete its chunk files. Returns deleted file count.
    int removeGame(size_t index);

    /// Rename a game: rewrite ul.cfg + rename all chunk files.
    bool renameGame(size_t index, const std::string& newName);

    /// Fix a record's serial ("Correct code 666.66"): rewrite ul.cfg,
    /// rename the chunk files (ul.<ID>.<old>.NN -> ul.<ID>.<new>.NN) and
    /// carry the video-rip sidecar along. Game name and id stay the same.
    bool renameSerial(size_t index, const std::string& newSerial);

    /// Find record index whose chunk files match idHex+serial, or SIZE_MAX.
    size_t findByIdSerial(const std::string& idHex, const std::string& serial) const;
};

} // namespace usbutils
