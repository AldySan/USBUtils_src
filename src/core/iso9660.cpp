// SPDX-License-Identifier: MIT
#include "iso9660.h"
#include "port.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

namespace usbutils {

namespace {

constexpr uint32_t kSectorSize = 2048;

#pragma pack(push, 1)
struct IsoDirEntry {           // ISO9660 directory record — ECMA-119
    uint8_t  len;              // 0      record length
    uint8_t  extLen;           // 1      extended attribute length
    uint32_t lbaLE, lbaBE;     // 2, 6   extent location
    uint32_t sizeLE, sizeBE;   // 10, 14 data length (32-bit!)
    uint8_t  date[7];          // 18..24 recording date/time
    uint8_t  flags;            // 25     file flags
    uint8_t  unitSize;         // 26     file unit size
    uint8_t  gapSize;          // 27     interleave gap size
    uint16_t volSeqLE, volSeqBE; // 28, 30 volume sequence number
    uint8_t  nameLen;          // 32     file identifier length
    // 33..: file identifier
};
#pragma pack(pop)

struct FileHandle {
    FILE* f = nullptr;
    ~FileHandle() { if (f) std::fclose(f); }
};

/// Directory extent to visit, with its accumulated path prefix.
struct DirVisit {
    uint32_t    lba;
    uint64_t    size;
    std::string path;           ///< "" for root, "MOVIES" for a subdir
};

/// Safety caps: a corrupt/hostile image must not hang or OOM the walker.
constexpr uint64_t kMaxDirBytes  = 8ull * 1024 * 1024;   // 8 MiB per directory
constexpr size_t   kMaxDirs      = 4096;

std::string toLower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        out += static_cast<char>((c >= 'A' && c <= 'Z') ? c + 32 : c);
    return out;
}

/// Strip the ISO9660 version suffix (";1") and any trailing pad byte.
std::string cleanIsoName(const uint8_t* rec, size_t recOff) {
    const IsoDirEntry* e = reinterpret_cast<const IsoDirEntry*>(rec + recOff);
    std::string name(reinterpret_cast<const char*>(rec + recOff + 33), e->nameLen);
    if (!name.empty() && name.back() == '\0') name.pop_back();   // pad byte
    const size_t semi = name.find(';');
    if (semi != std::string::npos) name.resize(semi);
    return name;
}

} // namespace

// ---------------------------------------------------------------------------
// walkIso
// ---------------------------------------------------------------------------

bool walkIso(const IsoReadFn& read,
             const std::function<bool(const IsoEntry&)>& onEntry) {
    uint8_t pvd[kSectorSize];
    if (!read(16ull * kSectorSize, pvd, sizeof(pvd))) return false;
    if (std::memcmp(pvd + 1, "CD001", 5) != 0) return false;   // not ISO9660

    const IsoDirEntry* root =
        reinterpret_cast<const IsoDirEntry*>(pvd + 156);

    std::vector<DirVisit> stack;
    std::vector<uint32_t> visited;                 // cycle guard (corrupt images)
    stack.push_back({root->lbaLE, root->sizeLE, ""});

    while (!stack.empty()) {
        const DirVisit cur = stack.back();
        stack.pop_back();

        if (visited.size() >= kMaxDirs) return true;   // pathological — stop safely
        visited.push_back(cur.lba);

        uint64_t dirLen = cur.size;
        if (dirLen == 0 || dirLen > kMaxDirBytes) dirLen = kMaxDirBytes;
        std::vector<uint8_t> dir(static_cast<size_t>(dirLen));
        if (!read(static_cast<uint64_t>(cur.lba) * kSectorSize, dir.data(), dir.size()))
            continue;                               // unreadable dir — skip subtree

        for (size_t off = 0; off + 1 < dir.size();) {
            const uint8_t recLen = dir[off];
            if (recLen == 0) {                      // pad to sector boundary
                off = (off / kSectorSize + 1) * kSectorSize;
                continue;
            }
            if (off + recLen > dir.size()) break;   // truncated record
            const IsoDirEntry* e =
                reinterpret_cast<const IsoDirEntry*>(dir.data() + off);
            const size_t recOff = off;              // (used by cleanIsoName)
            off += recLen;

            // Sanity: the 33-byte header and the whole identifier must fit
            // inside the record. Malformed/hostile images otherwise make
            // cleanIsoName() read past the buffer (garbage or wild names).
            if (recLen < 34 || size_t(e->nameLen) + 33 > recLen) continue;

            const std::string name = cleanIsoName(dir.data(), recOff);

            // "." and ".." — identifier byte is 0x00 / 0x01
            if (name.empty() ||
                (e->nameLen == 1 && dir[recOff + 33] < 2))
                continue;

            IsoEntry en;
            en.name  = name;
            en.path  = cur.path.empty() ? name : cur.path + "\\" + name;
            en.lba   = e->lbaLE;
            en.size  = e->sizeLE;
            en.isDir = (e->flags & 0x02) != 0;

            if (en.isDir) {
                bool seen = false;                  // self-referencing dir guard
                for (uint32_t v : visited) if (v == en.lba) { seen = true; break; }
                if (!seen && en.lba != cur.lba)
                    stack.push_back({en.lba, en.size, en.path});
            }
            if (!onEntry(en)) return true;          // caller requested early stop
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// readIsoInfo
// ---------------------------------------------------------------------------

std::string normalizeSerial(const std::string& raw) {
    std::string s = raw;
    // strip ISO9660 version suffix ";1" and a trailing ".1"
    if (s.size() > 2 && s.compare(s.size() - 2, 2, ";1") == 0)
        s = s.substr(0, s.size() - 2);
    // Original checked char 11 for '.' or '1' and replaced it with '0'
    // (0x3A1072..0x3A109C): pad short serials to 11 chars with '0'.
    if (s.size() < 11) {
        while (s.size() < 11) s += '0';
    } else if (s.size() > 11) {
        s = s.substr(0, 11);
    }
    return s;
}

/// Extract the boot ELF file name from a SYSTEM.CNF "BOOT2 = ..." line.
/// Handles both "cdrom0:\SLUS_202.65;1" and bare "SLUS_202.65" forms.
/// (Exported for gameops.cpp — the "Correct code 666.66" fixer needs to
/// re-derive a game's serial from its chunk stream.)
std::string bootElfFromCnf(const std::string& cnf) {
    const size_t b = cnf.find("BOOT2");
    if (b == std::string::npos) return {};
    const size_t eq = cnf.find('=', b);
    if (eq == std::string::npos) return {};

    // value runs to end of line
    size_t end = eq + 1;
    while (end < cnf.size() && cnf[end] != '\r' && cnf[end] != '\n') ++end;
    std::string val = cnf.substr(eq + 1, end - eq - 1);

    // trim blanks, strip ";N" version suffix
    size_t s = 0, t = val.size();
    while (s < t && (val[s] == ' ' || val[s] == '\t')) ++s;
    while (t > s && (val[t-1] == ' ' || val[t-1] == '\t' || val[t-1] == ';'))
        --t;
    val = val.substr(s, t - s);

    // basename: after the last '\' or ':'
    size_t cut = val.rfind('\\');
    const size_t colon = val.rfind(':');
    if (colon != std::string::npos && (cut == std::string::npos || colon > cut))
        cut = colon;
    if (cut != std::string::npos) val = val.substr(cut + 1);
    return val;
}

bool readPvd(const IsoReadFn& read, std::string* volumeId,
             uint32_t* volumeSectors) {
    uint8_t pvd[kSectorSize];
    if (!read(16ull * kSectorSize, pvd, sizeof(pvd))) return false;
    if (std::memcmp(pvd + 1, "CD001", 5) != 0) return false;
    if (volumeId) {
        volumeId->assign(reinterpret_cast<const char*>(pvd + 40), 32);
        while (!volumeId->empty() &&
               (volumeId->back() == ' ' || volumeId->back() == '\0'))
            volumeId->pop_back();
    }
    if (volumeSectors) {
        *volumeSectors = static_cast<uint32_t>(
            pvd[80] | (pvd[81] << 8) | (pvd[82] << 16) |
            (static_cast<uint32_t>(pvd[83]) << 24));
    }
    return true;
}

IsoInfo readIsoInfo(const std::string& path) {
    IsoInfo info;
    FileHandle fh;
    fh.f = std::fopen(path.c_str(), "rb");
    if (!fh.f) return info;

    // ---- Primary Volume Descriptor (sector 16) --------------------------
    uint8_t pvd[kSectorSize];
    if (!usbutils::seek64(fh.f, 16ll * kSectorSize) ||
        std::fread(pvd, 1, kSectorSize, fh.f) != kSectorSize)
        return info;
    if (std::memcmp(pvd + 1, "CD001", 5) != 0) return info;   // not ISO9660

    info.volumeSize = static_cast<uint32_t>(
        pvd[80] | (pvd[81] << 8) | (pvd[82] << 16) |
        (static_cast<uint32_t>(pvd[83]) << 24));
    info.volumeId.assign(reinterpret_cast<char*>(pvd + 40), 32);
    while (!info.volumeId.empty() &&
           (info.volumeId.back() == ' ' || info.volumeId.back() == '\0'))
        info.volumeId.pop_back();

    // ---- Walk the directory tree looking for SYSTEM.CNF -----------------
    // (the original only inspected the root; real PS2 discs always keep it
    // there, but accepting any depth is strictly more forgiving)
    uint32_t sysLba = 0;
    uint64_t sysSize = 0;
    FILE* file = fh.f;                                 // captured by the read fn
    const IsoReadFn read = [file](uint64_t off, void* dst, size_t n) {
        return usbutils::seek64(file, static_cast<int64_t>(off))
            && std::fread(dst, 1, n, file) == n;
    };
    walkIso(read, [&](const IsoEntry& e) {
        if (!e.isDir && toLower(e.name) == "system.cnf") {
            sysLba = e.lba;
            sysSize = e.size;
            return false;                              // found — stop the walk
        }
        return true;
    });

    if (sysLba == 0) return info;                      // "not a PS2 disc"

    // ---- Read SYSTEM.CNF (capped at 255 like the original) --------------
    if (sysSize > 255) sysSize = 255;
    std::string cnf(static_cast<size_t>(sysSize), '\0');
    if (sysSize == 0 ||
        !read(static_cast<uint64_t>(sysLba) * kSectorSize, &cnf[0], cnf.size()))
        return info;
    info.systemCnf = cnf;

    info.bootElf = bootElfFromCnf(cnf);
    info.serial = normalizeSerial(info.bootElf);
    info.valid = !info.serial.empty();
    return info;
}

} // namespace usbutils
