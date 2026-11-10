// SPDX-License-Identifier: MIT
/*
 * ISO shrink — the scene's "real" ripping, done safely.
 *
 * The media rip (videorip.h) blanks files IN PLACE: the game keeps working,
 * but the drive space stays used. Back in the USBExtreme days people made
 * games actually SMALLER by rebuilding the ISO: relinking duplicate files
 * (intro movies in five languages stored five times), collapsing dummy
 * padding files into one shared zero extent and cutting the sectors no file
 * uses (publisher padding, trailing slack). This engine does exactly that,
 * in place, inside the game's chunk stream:
 *
 *   1. analyze — walk the ISO9660 tree, hash every file, find exact
 *      duplicates, all-zero extents and unreferenced sectors;
 *   2. rewrite — move the surviving data to the front of the image (each
 *      extent's directory record is re-pointed, path tables and the volume
 *      descriptor are patched), then cut the stream and drop surplus
 *      chunk files (ul.cfg's parts field is kept in sync);
 *   3. verify — re-walk the finished image and re-hash every file; the
 *      shrink only reports success when every byte still matches.
 *
 * Every file's recorded size is preserved and every file stays reachable
 * under its original name, so games and loaders that enumerate files keep
 * working — unlike the per-game "ripkits" of the 2000s nothing is deleted
 * from the directory tree.
 *
 * Refused (not silently mangled): Joliet supplementary volumes, UDF hybrid
 * descriptors, Rock Ridge / SUSP system-use data, interleaved or extended-
 * attribute records, non-sequential multi-extent files, games with a
 * PS2Load injection (it stores appended data past the volume) and games
 * with rip backups (their sidecar offsets would go stale — restore first,
 * or pass --discard-backups).
 *
 * The rewrite phase runs to completion once started — a half-moved image
 * is not a valid one. Do not unplug the drive while it runs.
 */
#pragma once

#include "gamefile.h"      // ProgressFn

#include <string>
#include <cstdint>
#include <functional>

namespace usbutils {

/// What a shrink could reclaim from one game (all fields in bytes).
struct ShrinkInfo {
    bool        ok = false;
    std::string error;
    bool        hasRipBackups = false;   ///< .rip/.arip sidecar present
    bool        ps2LoadInjected = false; ///< PS2Load marker found
    uint64_t    currentBytes = 0;        ///< stream size now
    uint64_t    duplicateBytes = 0;      ///< extra copies of identical files
    uint64_t    zeroBytes = 0;           ///< zero extents beyond one shared copy
    uint64_t    gapBytes = 0;            ///< padding, slack, unused sectors
    uint64_t    finalBytes = 0;          ///< projected size after the shrink
    size_t      duplicateFiles = 0;
    size_t      zeroFiles = 0;

    uint64_t savings() const {
        return currentBytes > finalBytes ? currentBytes - finalBytes : 0;
    }
};

/// Read-only analysis of one installed game. Hashing walks every file, so
/// this reports progress like any long engine operation.
ShrinkInfo analyzeShrink(const std::string& dir, const std::string& idHex,
                         const std::string& serial, uint32_t parts,
                         const ProgressFn& progress = nullptr,
                         const std::function<bool()>& cancelled = nullptr);

struct ShrinkResult {
    bool        ok = false;
    std::string error;
    uint64_t    oldBytes = 0;
    uint64_t    newBytes = 0;
    uint32_t    oldParts = 0;
    uint32_t    newParts = 0;
    bool        verified = false;        ///< post-rewrite hash check passed
};

/// Apply the shrink to one installed game. Refuses while rip sidecars
/// exist unless `discardBackups` is set (their stored offsets would be
/// stale after the rewrite — the media stay blanked either way). Rewrites
/// the chunk stream in place, rewrites ul.cfg's parts field and deletes
/// surplus chunk files. `cancelled` is honored during analysis only.
ShrinkResult shrinkGame(const std::string& dir, const std::string& idHex,
                        const std::string& serial, uint32_t parts,
                        bool discardBackups,
                        const ProgressFn& progress = nullptr,
                        const std::function<bool()>& cancelled = nullptr);

} // namespace usbutils
