// SPDX-License-Identifier: MIT
/*
 * Recovery — rebuild ul.cfg records for orphan "ul.*.00" chunk files.
 * Mirrors the original "Recover games" feature (ScanULPattern @0x3A2B77
 * plus rcn serial/name database lookups).
 */
#pragma once

#include "ulcfg.h"
#include "gamedb.h"

#include <string>
#include <vector>

namespace usbutils {

struct OrphanGame {
    std::string idHex;
    std::string serial;
    std::string fileName;      ///< first chunk path
    uint32_t    partsFound = 0;///< consecutive chunks present
    uint64_t    totalBytes = 0;
    std::string suggestedName; ///< from rcn/ljs databases when known
};

/// Scan dir for orphan first-chunks not present in ul.cfg.
std::vector<OrphanGame> findOrphans(const std::string& dir);

/// Build a ul.cfg record for an orphan (name required — from rcn db,
/// the game db, or a user-provided fallback like the serial).
/// Media is derived from the total size (700 MB rule).
UlCfgRecord makeRecoveredRecord(const OrphanGame& orphan,
                                const std::string& name);

} // namespace usbutils
