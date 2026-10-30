// SPDX-License-Identifier: MIT
/*
 * PS2Load injection engine — reconstructed from the original binary
 * (see ps2load.h for the format summary and docs/formats/ps2load.md
 * for the full derivation). Everything operates on the game's chunk
 * stream through GameStream, exactly like the other engines.
 */
#include "ps2load.h"

#include "gamestream.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>

namespace usbutils {

namespace {

constexpr uint32_t kSectorSize = 2048;
/// Size stored in the SCEE_DD marker directory record (0x158 @0x3BAE58).
constexpr uint32_t kMarkerDirSize = 0x158;
/// Room the three injected records need (original check @0x3BAE1A).
constexpr uint32_t kRecordSlack = 0x90;

struct DirRec {
    uint32_t    lba = 0;
    uint64_t    size = 0;
    uint8_t     flags = 0;
    std::string name;      ///< raw ISO9660 name, ";N" stripped
    size_t      offset = 0;///< byte offset of the record inside the dir area
    size_t      length = 0;
};

uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (uint32_t(p[1]) << 8)
         | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

/// Strip the ISO9660 version suffix (";1") from a record name — the
/// original compares Copy(name, 1, N) with the fixed names (0x3B9E41).
std::string stripVersion(const std::string& raw) {
    const auto semi = raw.find(';');
    return semi == std::string::npos ? raw : raw.substr(0, semi);
}

/// Walk the directory records of `dirArea` (rootDirSectors * 2048 bytes).
/// Handles the standard sector-boundary padding rule: a record never
/// straddles a sector boundary, the gap is zero-filled.
std::vector<DirRec> parseDirArea(const std::vector<uint8_t>& dirArea) {
    std::vector<DirRec> out;
    const size_t size = dirArea.size();
    size_t off = 0;
    while (off < size) {
        const uint8_t len = dirArea[off];
        if (len == 0) {
            // rest of this sector is padding — continue at the next one
            const size_t next = (off / kSectorSize + 1) * kSectorSize;
            if (next <= off) break;
            off = next;
            continue;
        }
        if (off + len > size || len < 33) break;

        DirRec r;
        r.lba    = rd32(dirArea.data() + off + 2);
        r.size   = rd32(dirArea.data() + off + 10);
        r.flags  = dirArea[off + 25];
        const uint8_t nameLen = dirArea[off + 32];
        if (33 + nameLen > len) break;                 // corrupt record
        r.name   = stripVersion(std::string(
            reinterpret_cast<const char*>(dirArea.data() + off + 33),
            nameLen));
        r.offset = off;
        r.length = len;
        out.push_back(std::move(r));
        off += len;
    }
    return out;
}

/// Read the PVD (sector 16) and report the root directory extent.
bool readRootDir(GameStream& gs, uint32_t& lba, uint32_t& sectors) {
    uint8_t pvd[kSectorSize];
    if (gs.read(16ull * kSectorSize, pvd, sizeof pvd) != sizeof pvd)
        return false;
    if (std::memcmp(pvd + 1, "CD001", 5) != 0)
        return false;                                   // not ISO9660
    const uint32_t rootSize = rd32(pvd + 166);
    // Hostile-image guard (same 8 MiB cap as walkIso): a lied-about root
    // size would otherwise size the dirArea vector at up to 4 GiB before
    // any read fails. Real PS2 root directories are a few sectors.
    if (rootSize == 0 || rootSize > 8u * 1024 * 1024) return false;
    lba      = rd32(pvd + 158);
    sectors  = static_cast<uint32_t>((rootSize + kSectorSize - 1) / kSectorSize);
    return lba != 0 && sectors != 0;
}

/// The original's boot-name mangling (0x3BAB38): "SCUS_973.99" with the
/// first five characters dropped, the extension re-appended and the
/// "PS2L_" prefix added -> "PS2L_973.99.99".
std::string buildPs2lName(const std::string& bootName) {
    std::string stem = bootName, ext;
    if (const auto dot = bootName.find_last_of('.'); dot != std::string::npos) {
        stem = bootName.substr(0, dot);
        ext  = bootName.substr(dot);
    }
    std::string core;
    if (bootName.size() >= 8)      core = bootName.substr(5);
    else if (bootName.size() >= 3) core = bootName.substr(0, 3);
    else                           core = bootName;
    return "PS2L_" + core + ext;
}

/// Extract the boot file name from a SYSTEM.CNF text
/// ("BOOT2 = cdrom0:\SCUS_973.99;1" -> "SCUS_973.99").
std::string parseBootElf(const std::string& cnf) {
    const auto boot = cnf.find("BOOT2");
    if (boot == std::string::npos) return {};
    const auto colon = cnf.find(':', boot);
    if (colon == std::string::npos) return {};
    const auto semi = cnf.find(';', colon);
    const auto lineEnd = cnf.find_first_of("\r\n", colon);
    const auto nameEnd = std::min(semi == std::string::npos ? cnf.size() : semi,
                                  lineEnd == std::string::npos ? cnf.size() : lineEnd);
    std::string path = cnf.substr(colon + 1, nameEnd - colon - 1);
    // trim whitespace
    const auto first = path.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    const auto last = path.find_last_not_of(" \t");
    path = path.substr(first, last - first + 1);
    const auto slash = path.find_last_of('\\');
    if (slash != std::string::npos) path = path.substr(slash + 1);
    return path;
}

/// Load a whole file into memory (the loader ELF is ~213 KiB).
bool loadFile(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::rewind(f);
    if (sz <= 0) { std::fclose(f); return false; }
    out.resize(static_cast<size_t>(sz));
    const bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

/// Length one directory record will occupy (ISO9660: even-sized).
size_t recordLen(const std::string& name) {
    size_t len = 33 + name.size();
    return len % 2 ? len + 1 : len;
}

/// Build one ISO9660 directory record (the original's 33-byte template
/// @0x3BAE44 with true lengths instead of the fixed 0x30).
size_t putRecord(std::vector<uint8_t>& dir, size_t off, uint32_t lba,
                 uint32_t size, uint8_t flags, const std::string& name) {
    size_t len = 33 + name.size();
    if (len % 2) ++len;                                 // ISO9660: even
    if (off + len > dir.size()) return 0;
    uint8_t rec[64] = {};
    rec[0]  = static_cast<uint8_t>(len);
    std::memcpy(rec + 2,  &lba, 4);
    rec[6]  = static_cast<uint8_t>(lba >> 24);          // big-endian extent
    rec[7]  = static_cast<uint8_t>(lba >> 16);
    rec[8]  = static_cast<uint8_t>(lba >> 8);
    rec[9]  = static_cast<uint8_t>(lba);
    std::memcpy(rec + 10, &size, 4);
    rec[14] = static_cast<uint8_t>(size >> 24);         // big-endian size
    rec[15] = static_cast<uint8_t>(size >> 16);
    rec[16] = static_cast<uint8_t>(size >> 8);
    rec[17] = static_cast<uint8_t>(size);
    rec[18] = flags;
    rec[25] = static_cast<uint8_t>(name.size());
    std::memcpy(rec + 28, "\x01\x00", 2);               // volume seq LE
    rec[30] = 0x00; rec[31] = 0x01;                     // volume seq BE
    rec[32] = static_cast<uint8_t>(name.size());
    std::memcpy(rec + 33, name.data(), name.size());
    std::memset(dir.data() + off, 0, len);
    std::memcpy(dir.data() + off, rec, len);
    return len;
}

/// Shared state scan: opens the stream, reads the root dir area and
/// collects the records the injector cares about.
struct Scan {
    GameStream           gs;
    uint32_t             rootLba = 0, rootSectors = 0;
    std::vector<uint8_t> dirArea;
    std::vector<DirRec>  recs;
    const DirRec*        systemCnf = nullptr;
    const DirRec*        msystemCnf = nullptr;
    const DirRec*        marker = nullptr;
    size_t               recordsEnd = 0;               ///< where free space starts
    std::string          error;

    bool run(const std::string& dir, const std::string& idHex,
             const std::string& serial, uint32_t parts) {
        if (!gs.open(dir, idHex, serial, parts)) {
            error = "cannot open game chunks";
            return false;
        }
        if (!readRootDir(gs, rootLba, rootSectors)) {
            error = "not an ISO9660 game";
            return false;
        }
        dirArea.resize(static_cast<size_t>(rootSectors) * kSectorSize);
        if (gs.read(uint64_t(rootLba) * kSectorSize, dirArea.data(),
                    dirArea.size()) != dirArea.size()) {
            error = "cannot read root directory";
            return false;
        }
        recs = parseDirArea(dirArea);
        recordsEnd = 0;
        for (const auto& r : recs) {
            if (r.name == "SYSTEM.CNF")  systemCnf  = &r;
            if (r.name == "MSYSTEM.CNF") msystemCnf = &r;
            if (r.name == "SCEE_DD" && (r.flags & 0x02)) marker = &r;
            recordsEnd = r.offset + r.length;
        }
        return true;
    }
};

Ps2LoadState classify(const Scan& sc) {
    if (!sc.marker) return Ps2LoadState::NotInjected;
    return sc.marker->lba < sc.rootLba ? Ps2LoadState::InjectedByTool
                                       : Ps2LoadState::InjectedManually;
}

} // namespace

Ps2LoadInfo queryPs2Load(const std::string& dir, const std::string& idHex,
                         const std::string& serial, uint32_t parts) {
    Ps2LoadInfo info;
    Scan sc;
    if (!sc.run(dir, idHex, serial, parts)) {
        info.error = sc.error;
        return info;
    }
    info.state          = classify(sc);
    info.rootDirLba     = sc.rootLba;
    info.rootDirSectors = sc.rootSectors;
    if (sc.marker) info.markerLba = sc.marker->lba;
    if (sc.systemCnf) {
        info.systemCnfLba  = sc.systemCnf->lba;
        info.systemCnfSize = sc.systemCnf->size;
        // read the CNF text (capped at one sector)
        std::string cnf(std::min<uint64_t>(sc.systemCnf->size, kSectorSize),
                        '\0');
        if (sc.gs.read(uint64_t(sc.systemCnf->lba) * kSectorSize,
                       cnf.data(), cnf.size()) == cnf.size()) {
            info.bootElf = parseBootElf(cnf);
            // the injected boot name comes from the record list instead
            if (info.state == Ps2LoadState::NotInjected)
                info.ps2lName = buildPs2lName(info.bootElf);
            else
                info.ps2lName = parseBootElf(cnf);   // "PS2L_..."
        }
    }
    return info;
}

Ps2LoadResult injectPs2Load(const std::string& dir, const std::string& idHex,
                            const std::string& serial, uint32_t parts,
                            const std::string& elfPath,
                            const ProgressFn& progress) {
    Ps2LoadResult res;
    Scan sc;
    if (!sc.run(dir, idHex, serial, parts)) {
        res.error = sc.error;
        return res;
    }

    switch (classify(sc)) {
    case Ps2LoadState::InjectedByTool:
        res.error = "PS2Load was already injected into this game";
        return res;                                     // @0x3BB370
    case Ps2LoadState::InjectedManually:
        res.error = "the game contains a manually added PS2Load; "
                    "remove it with the tool it was made with";
        return res;                                     // @0x3BAA44
    default: break;
    }
    if (!sc.systemCnf || sc.systemCnf->size == 0) {
        res.error = "SYSTEM.CNF is missing or empty";  // @0x3BB398
        return res;
    }

    // --- gather everything before touching a single byte ---------------
    std::vector<uint8_t> elf;
    if (!loadFile(elfPath, elf)) {
        res.error = "cannot read the loader: " + elfPath;
        return res;
    }

    // Real games are whole ISO images split into chunks, so the stream
    // end is always a multiple of 2048. Refusing anything else keeps the
    // removal able to restore the exact original file size.
    if (sc.gs.size() % kSectorSize) {
        res.error = "game stream does not end on a sector boundary "
                    "(damaged or non-standard install)";
        return res;
    }

    const uint64_t cnfLen = std::min<uint64_t>(sc.systemCnf->size, kSectorSize);
    std::string cnf(static_cast<size_t>(cnfLen), '\0');
    if (sc.gs.read(uint64_t(sc.systemCnf->lba) * kSectorSize,
                   cnf.data(), cnf.size()) != cnf.size()) {
        res.error = "cannot read SYSTEM.CNF";
        return res;
    }
    const std::string bootName = parseBootElf(cnf);
    if (bootName.empty()) {
        res.error = "no BOOT2 entry in SYSTEM.CNF — not a PS2 disc";
        return res;                                     // @0x9A59C
    }
    const std::string ps2l = buildPs2lName(bootName);
    if (ps2l.size() > 28) {                             // record name limit
        res.error = "boot file name too long for PS2Load injection";
        return res;
    }

    // room for the three records in the root directory area
    const size_t need =
        recordLen("SCEE_DD") + recordLen("MSYSTEM.CNF;1")
      + recordLen(ps2l + ";1");
    if (sc.recordsEnd + need > sc.dirArea.size()) {
        res.error = "no room left in the root directory for PS2Load "
                    "(needs " + std::to_string(need) + " bytes)";
        return res;                                     // @0x3BB3CC
    }

    // marker LBA: the last all-zero sector before the root directory
    // (the original scans [0..rootDirLba], helper 0x3B9CF0 / 0x3BA4EC).
    uint32_t markerLba = 0;
    bool haveMarker = false;
    {
        std::vector<uint8_t> sec(kSectorSize);
        for (uint32_t s = sc.rootLba; s-- > 0;) {
            if (sc.gs.read(uint64_t(s) * kSectorSize, sec.data(), sec.size())
                    == sec.size()
                && std::all_of(sec.begin(), sec.end(),
                               [](uint8_t b) { return b == 0; })) {
                markerLba = s;
                haveMarker = true;
                break;
            }
        }
    }
    if (!haveMarker) {
        res.error = "no spare zero sector found before the root directory";
        return res;
    }

    // --- 1) append the MSYSTEM.CNF backup: a full snapshot of the
    //     SYSTEM.CNF sector. The rewritten CNF can be a few bytes longer
    //     than the original (the PS2L_ name is longer), so the backup
    //     must cover the whole sector for a bit-exact restore.
    const uint64_t streamLimit = uint64_t(parts) * kChunkSize;
    std::vector<uint8_t> backup(kSectorSize);
    if (sc.gs.read(uint64_t(sc.systemCnf->lba) * kSectorSize,
                   backup.data(), backup.size()) != backup.size()) {
        res.error = "cannot read the SYSTEM.CNF sector";
        return res;
    }
    const uint64_t cnfLbaBase = sc.gs.size();
    if (sc.gs.append(backup.data(), backup.size(), streamLimit) == ~0ull) {
        res.error = "not enough space left in the last chunk";
        return res;
    }
    const uint32_t cnfLba = uint32_t(cnfLbaBase / kSectorSize);

    // --- 2) append PS2LOAD.ELF -----------------------------------------
    if (progress) progress(0, elf.size());
    if (sc.gs.append(elf.data(), elf.size(), streamLimit) == ~0ull) {
        res.error = "not enough space left in the last chunk for the loader";
        return res;
    }
    const uint32_t elfLba =
        uint32_t((cnfLbaBase + backup.size()) / kSectorSize);

    // --- 3) append the three root-directory records --------------------
    size_t off = sc.recordsEnd;
    off += putRecord(sc.dirArea, off, markerLba, kMarkerDirSize, 0x02,
                     "SCEE_DD");
    off += putRecord(sc.dirArea, off, cnfLba, kSectorSize, 0x00,
                     "MSYSTEM.CNF;1");
    off += putRecord(sc.dirArea, off, elfLba,
                     static_cast<uint32_t>(elf.size()), 0x00, ps2l + ";1");
    if (!sc.gs.write(uint64_t(sc.rootLba) * kSectorSize, sc.dirArea.data(),
                     sc.dirArea.size())) {
        res.error = "cannot rewrite the root directory";
        return res;
    }

    // --- 4) rewrite SYSTEM.CNF in place: BOOT2 -> the PS2L_ record -----
    std::string modified = cnf;
    const auto namePos = modified.find(bootName);
    if (namePos == std::string::npos) {
        // parseBootElf trimmed a path the text stores differently; bail
        // out cleanly (records already written point at valid data, the
        // CNF just still boots the original ELF).
        res.ok = true;
        res.ps2lName = ps2l;
        res.error = "SYSTEM.CNF not rewritten (boot name not found verbatim)";
        return res;
    }
    modified.replace(namePos, bootName.size(), ps2l);
    // never write past the CNF's own sector (the PS2L_ name is longer
    // than the original boot name; real CNFs sit in a mostly empty
    // sector, so the cap only guards pathological images)
    if (modified.size() > kSectorSize) modified.resize(kSectorSize);
    if (!sc.gs.write(uint64_t(sc.systemCnf->lba) * kSectorSize,
                     modified.data(), modified.size())) {
        res.error = "cannot rewrite SYSTEM.CNF";
        return res;
    }

    if (progress) progress(elf.size(), elf.size());
    res.ok = true;
    res.ps2lName = ps2l;
    res.appendedBytes = backup.size() + elf.size();
    return res;
}

Ps2LoadResult removePs2Load(const std::string& dir, const std::string& idHex,
                            const std::string& serial, uint32_t parts,
                            const ProgressFn& progress) {
    Ps2LoadResult res;
    Scan sc;
    if (!sc.run(dir, idHex, serial, parts)) {
        res.error = sc.error;
        return res;
    }
    if (classify(sc) != Ps2LoadState::InjectedByTool) {
        res.error = "this game does not contain a tool-made PS2Load "
                    "injection";                         // @0x3B9F0C
        return res;
    }
    if (!sc.msystemCnf || !sc.systemCnf) {
        res.error = "MSYSTEM.CNF backup record not found";
        return res;                                     // @0x3B8820
    }

    // --- restore SYSTEM.CNF from the sector snapshot --------------------
    // The backup is a full 2048-byte snapshot of the CNF's sector, which
    // also repairs the few bytes past the original CNF length that the
    // rewritten (longer) boot name may have touched.
    const uint64_t len = std::min<uint64_t>(sc.msystemCnf->size, kSectorSize);
    std::string backup(static_cast<size_t>(len), '\0');
    if (sc.gs.read(uint64_t(sc.msystemCnf->lba) * kSectorSize,
                   backup.data(), backup.size()) != backup.size()) {
        res.error = "cannot read the MSYSTEM.CNF backup";
        return res;
    }
    if (!sc.gs.write(uint64_t(sc.systemCnf->lba) * kSectorSize,
                     backup.data(), backup.size())) {
        res.error = "cannot restore SYSTEM.CNF";
        return res;
    }
    if (progress) progress(1, 2);

    // --- blank the injected records (marker offset to the area end) ----
    std::memset(sc.dirArea.data() + sc.marker->offset, 0,
                sc.dirArea.size() - sc.marker->offset);
    if (!sc.gs.write(uint64_t(sc.rootLba) * kSectorSize, sc.dirArea.data(),
                     sc.dirArea.size())) {
        res.error = "cannot rewrite the root directory";
        return res;
    }
    if (progress) progress(2, 2);

    // --- trim the appended bytes off the last chunk --------------------
    // The backup was written at (cnfLba * 2048) == the pre-inject EOF.
    const uint64_t preInjectEof = uint64_t(sc.msystemCnf->lba) * kSectorSize;
    if (preInjectEof < sc.gs.size())
        sc.gs.truncate(preInjectEof);                   // best effort

    res.ok = true;
    return res;
}

} // namespace usbutils
