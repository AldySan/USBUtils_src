// SPDX-License-Identifier: MIT
/*
 * USBUtils — open reimplementation of USBUtil v2.2 (PS2 USBExtreme manager)
 * Copyright (c) 2025 AldySan
 * Based on the reverse-engineering of "USBUtil v2.2 rev1.0" by ISEKO (2009).
 *
 * gameops — the remaining "Utils" menu features of the original USBUtil,
 * ported from the TForm1 menus (docs/dfm/TFORM1.dfm.txt):
 *
 *   - Find file in games     : search every installed game's ISO tree
 *                              (original "Find file in games" +
 *                              "SEARCH by size" dialogs)
 *   - Copy/Move              : copy or move a whole game to another drive
 *                              (original "Copy/Move" + "ADD to memory" /
 *                              "Copy History" — the memory list is the
 *                              selection; the engine is the same copy)
 *   - General Report         : text report — file system state, game table
 *                              with sizes, structure evaluation, broken
 *                              serial codes ("------ REPORT-" captions)
 *   - Correct code 666.66    : fix records whose serial field holds the
 *                              broken marker "666.66" (written by some
 *                              third-party tools): re-derive the real
 *                              serial from the game's own SYSTEM.CNF and
 *                              rename record + chunk files. Disassembly
 *                              0x39B774..0x39B85C: the original scans the
 *                              game list, slices 11 chars of the code
 *                              column and compares them with "666.66"
 *                              (string @0x39B854); failures map to
 *                              "No se pudo renombrar codigo 666.66"
 *                              ("could not rename code 666.66").
 *   - Explore/Extract/Inject : per-game file list, single-file extract
 *                              and in-place same-size file injection
 *                              (original "FILE: Explore/Configure",
 *                              "EXTRACT...", "INJECT and match").
 */
#pragma once

#include "ulcfg.h"
#include "iso9660.h"
#include "gamefile.h"

#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace usbutils {

// ---------------------------------------------------------------------------
// Find file in games
// ---------------------------------------------------------------------------

/// One search hit: the game it was found in plus the ISO entry.
struct FileHit {
    size_t      gameIndex = 0;
    std::string gameName;
    IsoEntry    entry;              ///< path / name / lba / size
};

/// Case-insensitive wildcard match ('*' any run, '?' single char) against
/// a file name or full path. An empty pattern matches everything.
bool matchPattern(const std::string& nameOrPath, const std::string& pattern);

/// Search all games in `cfg` for files matching `pattern` (wildcards,
/// matched against both the full path and the base name) and optionally an
/// exact byte size (0 = any). Only file entries are considered.
std::vector<FileHit> findFileInGames(const std::string& dir, const UlCfg& cfg,
                                     const std::string& pattern,
                                     uint64_t sizeFilter = 0);

// ---------------------------------------------------------------------------
// Explore / extract / inject (per-game file operations)
// ---------------------------------------------------------------------------

/// List every file stored inside an installed game (ISO9660 tree walk over
/// the chunk stream). `err` is set when the game cannot be opened as an
/// ISO9660 image.
std::vector<IsoEntry> listGameFiles(const std::string& dir,
                                    const std::string& idHex,
                                    const std::string& serial,
                                    uint32_t parts,
                                    std::string* err = nullptr);

/// Extract one in-game file to `outPath` (8 MiB buffered copy from the
/// chunk stream). Returns false on a short read or write error — partial
/// output files are removed.
bool extractGameFile(const std::string& dir, const std::string& idHex,
                     const std::string& serial, uint32_t parts,
                     const IsoEntry& entry, const std::string& outPath,
                     const ProgressFn& progress = nullptr,
                     const std::function<bool()>& cancelled = nullptr);

struct InjectResult {
    bool        ok = false;
    std::string error;
    std::string path;              ///< in-game path that was replaced
    uint64_t    bytes = 0;
};

/// Replace an in-game file with `localFile` IN PLACE. The replacement must
/// be exactly the same size as the original so every LBA stays valid — the
/// original's "INJECT and match" accepted exact matches only. `isoPath` is
/// matched case-insensitively; '/' and '\' separators are both accepted,
/// with or without a leading separator.
InjectResult injectGameFile(const std::string& dir, const std::string& idHex,
                            const std::string& serial, uint32_t parts,
                            const std::string& isoPath,
                            const std::string& localFile,
                            const ProgressFn& progress = nullptr);

// ---------------------------------------------------------------------------
// Copy / move games (original "Copy/Move")
// ---------------------------------------------------------------------------

struct CopyResult {
    bool        ok = false;
    std::string error;
    uint64_t    copiedBytes = 0;
    int         filesWritten = 0;
};

/// Copy one game (record + all chunk files) from `srcDir` to `destDir`.
/// With `deleteSource` this becomes a move: the source chunks, the record
/// and a video-rip sidecar are removed after a verified copy. Chunks are
/// streamed 8 MiB at a time; the video-rip sidecar ("<first chunk>.rip")
/// is carried over when present so ripped games stay reversible. A game
/// with the same id + serial already on the destination is an error.
/// `srcCfg` is modified only for moves (the record is removed).
CopyResult copyGame(const std::string& srcDir, UlCfg& srcCfg, size_t index,
                    const std::string& destDir, bool deleteSource,
                    const ProgressFn& progress = nullptr,
                    const std::function<bool()>& cancelled = nullptr);

// ---------------------------------------------------------------------------
// Correct code 666.66
// ---------------------------------------------------------------------------

struct SerialFix {
    size_t      index = 0;
    std::string gameName;
    std::string oldSerial;         ///< usually "666.66" (the broken marker)
    std::string newSerial;         ///< empty when the real serial could not
                                   ///< be re-derived (the original's
                                   ///< "No se pudo renombrar codigo 666.66")
    bool        applied = false;
};

/// Find records whose serial is the broken "666.66" marker (or empty) and
/// re-derive the real serial from the game's own SYSTEM.CNF. With `apply`,
/// rewrite the record and rename the chunk files (ul.<ID>.<old>.NN ->
/// ul.<ID>.<new>.NN). Returns one entry per broken record; an empty vector
/// means no broken codes were found.
std::vector<SerialFix> fixBrokenSerials(const std::string& dir, UlCfg& cfg,
                                        bool apply);

// ---------------------------------------------------------------------------
// Sorting (original "Sorting" button: "Sort the list of games in memory
// and disk")
// ---------------------------------------------------------------------------

/// Sort the game list alphabetically and save ul.cfg. Comparison is
/// case-insensitive on the display name (ties keep their relative disk
/// order — a stable sort, matching the original's in-memory list sort).
/// Records that are invalid/trailing garbage sink to the end so they never
/// hide real games. Returns false when the sorted ul.cfg could not be
/// written; `movedOut` (optional) reports how many records changed index.
bool sortGames(const std::string& dir, size_t* movedOut = nullptr);

// ---------------------------------------------------------------------------
// Restore space (original "Rest. Spc." / "Disk Status INFORMATION" panel)
// ---------------------------------------------------------------------------

/// One entry per file on the drive that belongs to the USBExtreme game
/// area (name starting with "ul.", excluding ul.cfg itself).
struct DriveSpaceReport {
    size_t      games = 0;          ///< records in ul.cfg
    size_t      gameFiles = 0;      ///< chunk files present that records use
    uint64_t    gameBytes = 0;      ///< bytes in those files
    size_t      obsoleteFiles = 0;  ///< "ul.*" files no record uses
    uint64_t    obsoleteBytes = 0;  ///< "space not occupied" (reclaimable)
    size_t      zeroSizeFiles = 0;  ///< "ul.*" files with size 0 (broken)
    uint64_t    freeBytes = 0;      ///< "current free space" on the volume
    std::vector<std::string> obsolete;  ///< obsolete file names (report/confirm)
};

/// Scan the drive like the original's INFORMATION panel: files in games,
/// obsolete files, space not occupied, current free space and files with
/// size at zero. Video-rip sidecars ("<first chunk>.rip") of existing
/// games count as game files, so ripped games are never reported as junk.
DriveSpaceReport scanDriveSpace(const std::string& dir);

/// "Proceed to restore space": delete the obsolete files found by the
/// scan. Returns the number of files actually removed (0 when none were
/// present or deletions failed — check scanDriveSpace afterwards).
int restoreSpace(const std::string& dir, const DriveSpaceReport& rep);

// ---------------------------------------------------------------------------
// General report
// ---------------------------------------------------------------------------

/// Build the original's "General Report" as plain text: drive/file system
/// state, the game table with sizes and media, per-game structure
/// evaluation, video state and broken serial codes.
std::string generateReport(const std::string& dir, const UlCfg& cfg);

} // namespace usbutils
