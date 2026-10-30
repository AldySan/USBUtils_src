// SPDX-License-Identifier: MIT
#include "videorip.h"
#include "gamestream.h"
#include "iso9660.h"
#include "port.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

namespace usbutils {

namespace {

constexpr char kSidecarMagic[8] = {'U','S','R','R','I','P','0','1'};

struct SidecarEntry {
    uint64_t offset;
    uint64_t size;
};

struct Sidecar {
    std::vector<SidecarEntry> entries;
    std::string path;          ///< "" when no sidecar exists
};

std::string toLower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        out += static_cast<char>((c >= 'A' && c <= 'Z') ? c + 32 : c);
    return out;
}

/// The original detected '.PSS' (extension) and '.PSS.' (embedded marker,
/// e.g. multi-part containers like "MOVIE.PSS.01"). Both are matched
/// case-insensitively on the version-stripped name.
bool isVideoName(const std::string& isoPath) {
    const std::string low = toLower(isoPath);
    const size_t slash = low.rfind('\\');
    const std::string base =
        low.substr(slash == std::string::npos ? 0 : slash + 1);
    if (base.size() >= 4 && base.compare(base.size() - 4, 4, ".pss") == 0)
        return true;
    return base.find(".pss.") != std::string::npos;
}

#pragma pack(push, 1)
struct SidecarHeader {
    char     magic[8];
    uint32_t count;
};
struct SidecarEntryRec {
    uint64_t offset;
    uint64_t size;
};
#pragma pack(pop)
static_assert(sizeof(SidecarHeader)   == 12, "sidecar header layout");
static_assert(sizeof(SidecarEntryRec) == 16, "sidecar entry layout");

bool loadSidecar(const std::string& path, Sidecar& out) {
    out.entries.clear();
    out.path.clear();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return true;                       // no sidecar — fine

    SidecarHeader hdr{};
    const bool ok = std::fread(&hdr, sizeof(hdr), 1, f) == 1
        && std::memcmp(hdr.magic, kSidecarMagic, sizeof(kSidecarMagic)) == 0;
    if (ok) {
        // Clamp the entry count by what the file can actually hold: the
        // header count is untrusted (a corrupt/hostile .rip would otherwise
        // size the vector at count*16 bytes — up to 64 GiB — before any
        // read happens).
        uint64_t maxEntries = 0;
        if (usbutils::seek64(f, 0, SEEK_END)) {
            const int64_t fsz = usbutils::tell64(f);
            if (fsz >= static_cast<int64_t>(sizeof(SidecarHeader)))
                maxEntries = static_cast<uint64_t>(fsz - sizeof(SidecarHeader))
                             / sizeof(SidecarEntryRec);
            if (usbutils::seek64(f, static_cast<int64_t>(sizeof(SidecarHeader)))) {
                // no-op reposition; keeps the read loop below meaningful
            }
        }
        if (hdr.count > maxEntries) hdr.count = static_cast<uint32_t>(maxEntries);
        out.entries.resize(hdr.count);
        for (uint32_t i = 0; i < hdr.count; ++i) {
            SidecarEntryRec rec{};
            if (std::fread(&rec, sizeof(rec), 1, f) != 1) {
                out.entries.resize(i);
                break;
            }
            out.entries[i] = {rec.offset, rec.size};
        }
        out.path = path;
    }
    std::fclose(f);
    return ok;                                 // false only on corrupt sidecar
}

/// Write the new sidecar: old entries + `newEntries` in the index, the
/// old data section copied VERBATIM (the game stream already holds zeros
/// for those ranges — re-reading it would destroy the backup!), then the
/// new ranges streamed out of the still-original game chunks.
bool saveSidecar(const std::string& path, const Sidecar& oldSc,
                 const std::vector<SidecarEntry>& newEntries,
                 GameStream& stream) {
    const std::string tmp = path + ".new";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;

    SidecarHeader hdr{};
    std::memcpy(hdr.magic, kSidecarMagic, sizeof(kSidecarMagic));
    hdr.count = static_cast<uint32_t>(oldSc.entries.size() + newEntries.size());
    bool ok = std::fwrite(&hdr, sizeof(hdr), 1, f) == 1;
    for (const auto& e : oldSc.entries) {
        SidecarEntryRec rec{e.offset, e.size};
        if (!ok || std::fwrite(&rec, sizeof(rec), 1, f) != 1) ok = false;
    }
    for (const auto& e : newEntries) {
        SidecarEntryRec rec{e.offset, e.size};
        if (!ok || std::fwrite(&rec, sizeof(rec), 1, f) != 1) ok = false;
    }

    // ---- data section ---------------------------------------------------
    // 1) previously backed-up ranges are copied verbatim from the old sidecar
    if (ok && !oldSc.path.empty()) {
        FILE* old = std::fopen(oldSc.path.c_str(), "rb");
        if (!old ||
            !usbutils::seek64(old, static_cast<int64_t>(
                sizeof(SidecarHeader) + oldSc.entries.size() * sizeof(SidecarEntryRec)))) {
            ok = false;
            if (old) std::fclose(old);
        } else {
            std::vector<uint8_t> buf(4 * 1024 * 1024);
            size_t n;
            while (ok && (n = std::fread(buf.data(), 1, buf.size(), old)) > 0)
                if (std::fwrite(buf.data(), 1, n, f) != n) ok = false;
            std::fclose(old);
        }
    }
    // 2) newly selected ranges are streamed out of the game chunks —
    //    they are still original bytes at this point (blanking happens later)
    std::vector<uint8_t> buf(4 * 1024 * 1024);
    for (const auto& e : newEntries) {
        uint64_t left = e.size;
        while (ok && left > 0) {
            const size_t want = static_cast<size_t>(
                std::min<uint64_t>(buf.size(), left));
            const size_t got = stream.read(e.offset + (e.size - left),
                                           buf.data(), want);
            if (got != want ||
                std::fwrite(buf.data(), 1, got, f) != got) {
                ok = false;
                break;
            }
            left -= got;
        }
    }
    std::fclose(f);

    if (!ok) {
        std::remove(tmp.c_str());
        return false;
    }
    std::remove(path.c_str());                 // rename-over does not work on Windows
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

/// A video is "blanked without backup" when its first sector is all zeros
/// (real PSS streams start with an MPEG pack header 00 00 01 BA).
bool looksBlanked(GameStream& stream, const VideoEntry& v) {
    if (v.size < 2048) return false;
    uint8_t head[16] = {};
    if (stream.read(v.offset, head, sizeof(head)) != sizeof(head))
        return false;
    for (uint8_t b : head)
        if (b != 0) return false;
    return true;
}

} // namespace

std::string videoBackupFileName(const std::string& idHex,
                                const std::string& serial) {
    return chunkFileName(idHex, serial, 0) + ".rip";
}

uint64_t videoBackupSize(const std::string& dir, const std::string& idHex,
                         const std::string& serial) {
    const std::string p = joinPath(dir, videoBackupFileName(idHex, serial));
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return 0;
    const bool ok = usbutils::seek64(f, 0, SEEK_END);
    const int64_t sz = ok ? usbutils::tell64(f) : -1;
    std::fclose(f);
    return sz < 0 ? 0 : static_cast<uint64_t>(sz);
}

// ---------------------------------------------------------------------------
// scan
// ---------------------------------------------------------------------------

std::vector<VideoEntry> scanVideos(const std::string& dir,
                                   const std::string& idHex,
                                   const std::string& serial,
                                   uint32_t parts,
                                   std::string* error) {
    std::vector<VideoEntry> out;
    if (error) error->clear();

    GameStream stream;
    if (!stream.open(dir, idHex, serial, parts) || stream.size() == 0) {
        if (error) *error = "cannot open game chunks in: " + dir;
        return out;
    }

    const IsoReadFn read = [&stream](uint64_t off, void* dst, size_t n) {
        return stream.read(off, dst, n) == n;
    };
    if (!walkIso(read, [&](const IsoEntry& e) {
            if (!e.isDir && isVideoName(e.path))
                out.push_back(VideoEntry{e.path,
                                         static_cast<uint64_t>(e.lba) * 2048ull,
                                         e.size, false, false});
            return true;
        })) {
        if (error) *error = "not a valid ISO9660 image (game data corrupt?)";
        out.clear();
        return out;
    }

    // merge sidecar state + zero-detection
    Sidecar sc;
    loadSidecar(joinPath(dir, videoBackupFileName(idHex, serial)), sc);
    for (VideoEntry& v : out) {
        for (const auto& e : sc.entries) {
            if (e.offset == v.offset && e.size == v.size) {
                v.hasBackup = true;
                v.blanked = true;
                break;
            }
        }
        if (!v.blanked) v.blanked = looksBlanked(stream, v);
    }
    return out;
}

// ---------------------------------------------------------------------------
// rip
// ---------------------------------------------------------------------------

RipResult ripVideos(const std::string& dir, const std::string& idHex,
                    const std::string& serial, uint32_t parts,
                    const std::vector<VideoEntry>& selection,
                    bool makeBackup,
                    const ProgressFn& progress,
                    const std::function<bool()>& cancelled) {
    RipResult res;

    GameStream stream;
    if (!stream.open(dir, idHex, serial, parts) || stream.size() == 0) {
        res.error = "cannot open game chunks in: " + dir;
        return res;
    }

    // ---- 1. build the entry list (old entries + newly selected) --------
    const std::string scPath = joinPath(dir, videoBackupFileName(idHex, serial));
    Sidecar sc;
    if (makeBackup && !loadSidecar(scPath, sc)) {
        res.error = "existing backup file is corrupt: " + scPath;
        return res;
    }

    std::vector<SidecarEntry> newEntries;
    uint64_t newBytes = 0;
    for (const VideoEntry& v : selection) {
        if (v.blanked) continue;                    // already ripped — skip
        if (v.offset + v.size > stream.size()) continue;   // out of range
        newEntries.push_back({v.offset, v.size});
        newBytes += v.size;
    }
    if (newEntries.empty()) {
        res.ok = true;                             // nothing new to rip
        return res;
    }

    // ---- 2. write the sidecar BEFORE touching the game ------------------
    if (makeBackup) {
        if (!saveSidecar(scPath, sc, newEntries, stream)) {
            res.error = "cannot write backup file: " + scPath;
            return res;
        }
        res.backupBytes = newBytes;
    }
    // (destructive rip: an existing backup is left untouched on purpose,
    //  so previously backed-up videos remain restorable)

    // ---- 3. blank the newly selected videos -----------------------------
    std::vector<uint8_t> zeros(4 * 1024 * 1024, 0);
    uint64_t done = 0;
    const uint64_t total = newBytes;

    for (const VideoEntry& v : selection) {
        if (v.blanked) continue;
        if (v.offset + v.size > stream.size()) continue;
        uint64_t left = v.size;
        while (left > 0) {
            if (cancelled && cancelled()) {
                res.ok = false;
                res.error = "cancelled";
                return res;
            }
            const size_t want = static_cast<size_t>(
                std::min<uint64_t>(zeros.size(), left));
            if (!stream.write(v.offset + (v.size - left), zeros.data(), want)) {
                res.error = "write error while blanking " + v.path;
                return res;
            }
            left     -= want;
            done     += want;
            res.blankedBytes += want;
            if (progress) progress(done, total);
        }
        ++res.rippedCount;
    }

    res.ok = true;
    return res;
}

// ---------------------------------------------------------------------------
// restore
// ---------------------------------------------------------------------------

RipResult restoreVideos(const std::string& dir, const std::string& idHex,
                        const std::string& serial, uint32_t parts,
                        const ProgressFn& progress,
                        const std::function<bool()>& cancelled) {
    RipResult res;

    const std::string scPath = joinPath(dir, videoBackupFileName(idHex, serial));

    Sidecar sc;
    if (!loadSidecar(scPath, sc) || sc.entries.empty()) {
        res.error = sc.path.empty() ? "no backup file for this game"
                                    : "backup file is corrupt: " + scPath;
        return res;
    }

    GameStream stream;
    if (!stream.open(dir, idHex, serial, parts) || stream.size() == 0) {
        res.error = "cannot open game chunks in: " + dir;
        return res;
    }

    FILE* f = std::fopen(scPath.c_str(), "rb");
    if (!f) {
        res.error = "cannot read backup file: " + scPath;
        return res;
    }
    if (!usbutils::seek64(f,
            static_cast<int64_t>(sizeof(SidecarHeader) +
                                 sc.entries.size() * sizeof(SidecarEntryRec)))) {
        std::fclose(f);
        res.error = "backup file is truncated";
        return res;
    }

    uint64_t total = 0;
    for (const auto& e : sc.entries) total += e.size;

    std::vector<uint8_t> buf(4 * 1024 * 1024);
    uint64_t done = 0;
    bool ok = true;
    for (const auto& e : sc.entries) {
        uint64_t left = e.size;
        while (left > 0) {
            if (cancelled && cancelled()) { ok = false; res.error = "cancelled"; break; }
            const size_t want = static_cast<size_t>(
                std::min<uint64_t>(buf.size(), left));
            const size_t got = std::fread(buf.data(), 1, want, f);
            if (got != want) { ok = false; res.error = "backup file is truncated"; break; }
            if (!stream.write(e.offset + (e.size - left), buf.data(), got)) {
                ok = false;
                res.error = "write error while restoring";
                break;
            }
            left -= got;
            done += got;
            res.blankedBytes += got;         // bytes restored
            if (progress) progress(done, total);
        }
        if (ok) ++res.rippedCount;
        else break;
    }
    std::fclose(f);

    if (!ok) return res;

    std::remove(scPath.c_str());             // backup fully consumed
    res.ok = true;
    return res;
}

} // namespace usbutils
