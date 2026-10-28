// SPDX-License-Identifier: MIT
#include "recovery.h"
#include "gamefile.h"
#include "ulid.h"

#include <cstdio>
#include "port.h"
#include <cstdint>

namespace usbutils {

std::vector<OrphanGame> findOrphans(const std::string& dir) {
    std::vector<OrphanGame> out;

    UlCfg cfg;
    cfg.load(dir);

    for (const FoundChunk& fc : scanFirstChunks(dir)) {
        // already indexed?
        if (cfg.findByIdSerial(fc.idHex, fc.serial) != SIZE_MAX) continue;

        OrphanGame og;
        og.idHex = fc.idHex;
        og.serial = fc.serial;
        og.fileName = fc.fileName;

        // count consecutive chunks .00, .01, ...
        uint32_t i = 0;
        for (;; ++i) {
            const std::string p =
                fc.fileName.substr(0, fc.fileName.size() - 2); // strip "00"
            char nn[8];
            std::snprintf(nn, sizeof(nn), "%02u", i);
            FILE* f = std::fopen((p + nn).c_str(), "rb");
            if (!f) break;
            if (std::fseek(f, 0, SEEK_END) == 0)
                og.totalBytes += static_cast<uint64_t>(usbutils::tell64(f));
            std::fclose(f);
        }
        og.partsFound = i;
        if (i == 0) continue;   // first chunk only — still offer recovery

        // Suggest a name: recovery db first, then the game db by serial.
        // (The original consults usbutil.rcn — "Archivo de recuperacion".)
        og.suggestedName = og.serial;
        out.push_back(std::move(og));
    }
    return out;
}

UlCfgRecord makeRecoveredRecord(const OrphanGame& orphan,
                                const std::string& name) {
    UlCfgRecord rec;
    rec.setName(name);
    rec.setSerial(orphan.serial);
    rec.parts = static_cast<uint8_t>(orphan.partsFound ? orphan.partsFound : 1);
    rec.media = mediaForSize(orphan.totalBytes);
    rec.flag  = 8;
    return rec;
}

} // namespace usbutils
