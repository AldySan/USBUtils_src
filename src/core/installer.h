// SPDX-License-Identifier: MIT
/*
 * Game installer — ISO to USBExtreme chunks.
 * Reconstructed from SplitISO @0x3A112F + CreateGameRecord @0x39DCB0:
 *   parts = ceil(size / 1 GiB); write "ul.<ID>.<SERIAL>.NN" chunks;
 *   build the 64-byte ul.cfg record; append/insert into ul.cfg.
 */
#pragma once

#include "ulcfg.h"
#include "iso9660.h"
#include "gamefile.h"

#include <string>
#include <vector>
#include <functional>

namespace usbutils {

struct InstallOptions {
    std::string gameName;        ///< display name (user-editable)
    bool        useIsoSerial = true;   ///< take serial from SYSTEM.CNF
    std::string serialOverride;        ///< used when SYSTEM.CNF missing
    bool        forceMedia    = false;
    uint8_t     mediaOverride = kMediaDvd;
};

struct InstallResult {
    bool        ok = false;
    std::string error;
    std::string idHex;
    std::string serial;
    uint32_t    parts = 0;
    uint64_t    totalBytes = 0;
    std::vector<std::string> writtenFiles;
};

/// Install an ISO onto `dir` (USB root). Reports progress via callback.
InstallResult installIso(const std::string& isoPath, const std::string& dir,
                         const InstallOptions& opt,
                         const ProgressFn& progress = nullptr,
                         const std::function<bool()>& cancelled = nullptr);

/// Rebuild an ISO image from installed chunks (reverse operation,
/// mirrors the "Create ISO" flow at 0x3A1367 which reads chunks and
/// writes a single image with the 1 GiB split undone).
bool exportIso(const std::string& dir, const UlCfgRecord& rec,
               const std::string& isoPath,
               const ProgressFn& progress = nullptr,
               const std::function<bool()>& cancelled = nullptr);

} // namespace usbutils
