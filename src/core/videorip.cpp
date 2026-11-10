// SPDX-License-Identifier: MIT
#include "videorip.h"
#include "gamestream.h"
#include "iso9660.h"
#include "crc32.h"
#include "lzpack.h"
#include "port.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

namespace usbutils {

namespace {

constexpr char kMagicV1[8] = {'U','S','R','R','I','P','0','1'};
constexpr char kMagicV2[8] = {'U','S','R','R','I','P','0','2'};
constexpr char kMagicV3[8] = {'U','S','R','R','I','P','0','3'};

struct SidecarEntry {
    uint64_t offset;
    uint64_t size;
    uint32_t crc;              ///< v3 only; 0 in v1/v2 (unverifiable)
};

struct Sidecar {
    std::vector<SidecarEntry> entries;
    std::string path;          ///< "" when no sidecar exists
    uint64_t payloadRaw = 0;   ///< uncompressed payload bytes (sum of sizes)
    uint64_t dataStart = 0;    ///< file offset where the payload begins
    bool v2 = false;           ///< block-compressed payload (USRRIP02/03)
    bool v3 = false;           ///< per-entry CRC32 present (USRRIP03)
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

/// PS2 audio: Sony ADPCM files (.VAG), VAB sample banks (.VAB/.VH/.VB),
/// ADPCM streams (.ADS/.ASS/.SS2) and interleaved music (.MIB/.MIH).
/// Extension match on the base name, case-insensitive.
bool isAudioName(const std::string& isoPath) {
    static const char* const kExts[] = {
        ".vag", ".vab", ".vh", ".vb",
        ".ads", ".ass", ".ss2",
        ".mib", ".mih"
    };
    const std::string low = toLower(isoPath);
    const size_t slash = low.rfind('\\');
    const std::string base =
        low.substr(slash == std::string::npos ? 0 : slash + 1);
    const size_t dot = base.rfind('.');
    if (dot == std::string::npos) return false;
    for (const char* e : kExts)
        if (base.compare(dot, std::strlen(e), e) == 0) return true;
    return false;
}

/// Everything that differs between the video and the audio rip: which
/// names match, and which sidecar file keeps the backup.
struct MediaDef {
    bool (*isMediaName)(const std::string&);
    const char* sidecarSuffix;
};
constexpr MediaDef kVideo{&isVideoName, ".rip"};
constexpr MediaDef kAudio{&isAudioName, ".arip"};

std::string backupPath(const std::string& dir, const MediaDef& def,
                       const std::string& idHex, const std::string& serial) {
    return joinPath(dir, chunkFileName(idHex, serial, 0) + def.sidecarSuffix);
}

uint64_t fileSizeBytes(const std::string& p) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return 0;
    const bool ok = usbutils::seek64(f, 0, SEEK_END);
    const int64_t sz = ok ? usbutils::tell64(f) : -1;
    std::fclose(f);
    return sz < 0 ? 0 : static_cast<uint64_t>(sz);
}

#pragma pack(push, 1)
struct SidecarHeaderV1 {
    char     magic[8];
    uint32_t count;
};
struct SidecarHeaderV2 {
    char     magic[8];
    uint32_t count;
    uint64_t payloadRaw;
};
struct SidecarEntryRec {
    uint64_t offset;
    uint64_t size;
};
struct SidecarEntryRecV3 {
    uint64_t offset;
    uint64_t size;
    uint32_t crc;
};
#pragma pack(pop)
static_assert(sizeof(SidecarHeaderV1) == 12, "sidecar v1 header layout");
static_assert(sizeof(SidecarHeaderV2) == 20, "sidecar v2/v3 header layout");
static_assert(sizeof(SidecarEntryRec) == 16, "sidecar v1/v2 entry layout");
static_assert(sizeof(SidecarEntryRecV3) == 20, "sidecar v3 entry layout");

bool loadSidecar(const std::string& path, Sidecar& out) {
    out.entries.clear();
    out.path.clear();
    out.payloadRaw = 0;
    out.dataStart = 0;
    out.v2 = false;
    out.v3 = false;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return true;                       // no sidecar — fine

    SidecarHeaderV1 hdr{};
    bool ok = std::fread(&hdr, sizeof(hdr), 1, f) == 1;
    uint32_t count = 0;
    uint64_t payloadRaw = 0;
    size_t hdrSize = 0;
    size_t recSize = sizeof(SidecarEntryRec);
    bool blocked = false;                      // block-compressed payload
    bool v3 = false;                           // per-entry CRC32 present
    if (ok && std::memcmp(hdr.magic, kMagicV1, sizeof(kMagicV1)) == 0) {
        count = hdr.count;
        hdrSize = sizeof(SidecarHeaderV1);
    } else if (ok && (std::memcmp(hdr.magic, kMagicV2, sizeof(kMagicV2)) == 0
                   || std::memcmp(hdr.magic, kMagicV3, sizeof(kMagicV3)) == 0)) {
        v3 = std::memcmp(hdr.magic, kMagicV3, sizeof(kMagicV3)) == 0;
        SidecarHeaderV2 h2{};
        ok = usbutils::seek64(f, 0, SEEK_SET)
             && std::fread(&h2, sizeof(h2), 1, f) == 1;
        if (ok) {
            count = h2.count;
            payloadRaw = h2.payloadRaw;
            hdrSize = sizeof(SidecarHeaderV2);
            recSize = v3 ? sizeof(SidecarEntryRecV3) : sizeof(SidecarEntryRec);
            blocked = true;
        }
    } else {
        ok = false;
    }

    if (ok) {
        // Clamp the entry count by what the file can actually hold: the
        // header count is untrusted (a corrupt/hostile .rip would otherwise
        // size the vector at count*20 bytes — up to 80 GiB — before any
        // read happens).
        uint64_t maxEntries = 0;
        if (usbutils::seek64(f, 0, SEEK_END)) {
            const int64_t fsz = usbutils::tell64(f);
            if (fsz >= static_cast<int64_t>(hdrSize))
                maxEntries = static_cast<uint64_t>(fsz - hdrSize) / recSize;
            if (!usbutils::seek64(f, static_cast<int64_t>(hdrSize)))
                ok = false;
        } else {
            ok = false;
        }
        if (ok) {
            if (count > maxEntries) count = static_cast<uint32_t>(maxEntries);
            out.entries.resize(count);
            for (uint32_t i = 0; i < count; ++i) {
                if (v3) {
                    SidecarEntryRecV3 rec{};
                    if (std::fread(&rec, sizeof(rec), 1, f) != 1) {
                        out.entries.resize(i);
                        break;
                    }
                    out.entries[i] = {rec.offset, rec.size, rec.crc};
                } else {
                    SidecarEntryRec rec{};
                    if (std::fread(&rec, sizeof(rec), 1, f) != 1) {
                        out.entries.resize(i);
                        break;
                    }
                    out.entries[i] = {rec.offset, rec.size, 0};
                }
            }
            uint64_t sum = 0;
            for (const auto& e : out.entries) sum += e.size;
            if (blocked && payloadRaw != sum)
                ok = false;      // claimed payload size disagrees — corrupt
            if (ok) {
                if (!blocked) payloadRaw = sum;
                out.payloadRaw = payloadRaw;
                out.dataStart = hdrSize + out.entries.size() * recSize;
                out.v2 = blocked;
                out.v3 = v3;
                out.path = path;
            }
        }
    }
    std::fclose(f);
    return ok;                                 // false only on corrupt sidecar
}

/// CRC seed for one entry: the checksum runs over the entry's own record
/// bytes (little-endian offset, then size) before its payload, so a
/// corrupted index fails verification exactly like corrupted data would.
uint32_t entryCrcSeed(uint64_t offset, uint64_t size) {
    uint8_t rec[16];
    for (int b = 0; b < 8; ++b)
        rec[b] = static_cast<uint8_t>(offset >> (8 * b));
    for (int b = 0; b < 8; ++b)
        rec[8 + b] = static_cast<uint8_t>(size >> (8 * b));
    return usbutils::crc32(0, rec, sizeof(rec));
}

/// Best-effort ISO path of the media file starting at `offset` — names
/// the failing entry in CRC errors (a blank offset is reported instead
/// when the index itself is what got corrupted).
std::string mediaNameAt(GameStream& stream, uint64_t offset) {
    std::string name;
    const IsoReadFn read = [&stream](uint64_t off, void* dst, size_t n) {
        return stream.read(off, dst, n) == n;
    };
    walkIso(read, [&name, offset](const IsoEntry& e) {
        if (!e.isDir && static_cast<uint64_t>(e.lba) * 2048ull == offset) {
            name = e.path;
            return false;                  // found — stop the walk
        }
        return true;
    });
    return name;
}

/// Sequential reader over a sidecar's payload: verbatim bytes for USRRIP01,
/// block-decoded (stored or LZSS) for USRRIP02/03. Shared by restore (its
/// verify pass and its write pass) and by saveSidecar
/// (re-reading an existing backup when more media are ripped later).
class PayloadReader {
public:
    explicit PayloadReader(const Sidecar& sc)
        : v2_(sc.v2), rawLeft_(sc.payloadRaw) {
        if (sc.path.empty()) return;           // no old sidecar — nothing to read
        f_ = std::fopen(sc.path.c_str(), "rb");
        if (f_ && !usbutils::seek64(f_, static_cast<int64_t>(sc.dataStart))) {
            std::fclose(f_);
            f_ = nullptr;
        }
    }
    ~PayloadReader() { if (f_) std::fclose(f_); }
    PayloadReader(const PayloadReader&) = delete;
    PayloadReader& operator=(const PayloadReader&) = delete;

    bool valid() const { return f_ != nullptr; }

    /// Fill exactly `n` raw payload bytes; false on truncation/corruption.
    bool read(uint8_t* dst, size_t n) {
        if (!f_ || failed_ || n > rawLeft_) { if (n) failed_ = true; return false; }
        size_t done = 0;
        while (done < n) {
            if (!v2_) {
                const size_t got = std::fread(dst + done, 1, n - done, f_);
                if (got == 0) { failed_ = true; return false; }
                done += got;
            } else {
                if (blkPos_ == blk_.size() && !nextBlock()) return false;
                const size_t take = std::min(n - done, blk_.size() - blkPos_);
                std::memcpy(dst + done, blk_.data() + blkPos_, take);
                blkPos_ += take;
                done += take;
            }
        }
        rawLeft_ -= n;
        return true;
    }

private:
    bool nextBlock() {
        uint32_t bh = 0;
        if (std::fread(&bh, sizeof(bh), 1, f_) != 1)
            { failed_ = true; return false; }
        const uint32_t rawLen = bh & 0x7FFFFFFFu;
        if (rawLen == 0 || rawLen > kLzBlockSize || rawLen > rawLeft_)
            { failed_ = true; return false; }
        blk_.resize(rawLen);
        blkPos_ = 0;
        if (bh & 0x80000000u) {
            uint32_t compLen = 0;
            if (std::fread(&compLen, sizeof(compLen), 1, f_) != 1 ||
                compLen > rawLen + rawLen / 8 + 64)
                { failed_ = true; return false; }
            comp_.resize(compLen);
            if (std::fread(comp_.data(), 1, compLen, f_) != compLen ||
                !lzDecodeBlock(comp_.data(), compLen, blk_.data(), rawLen))
                { failed_ = true; return false; }
        } else {
            if (std::fread(blk_.data(), 1, rawLen, f_) != rawLen)
                { failed_ = true; return false; }
        }
        return true;
    }

    FILE* f_ = nullptr;
    bool failed_ = false;
    bool v2_ = false;
    uint64_t rawLeft_ = 0;
    std::vector<uint8_t> blk_, comp_;
    size_t blkPos_ = 0;
};

/// Write the new sidecar (USRRIP03, block-compressed, per-entry CRC32):
/// old entries + `newEntries` in the index, then the payload — the old
/// sidecar's data re-read through PayloadReader (the game stream already
/// holds zeros for those ranges — re-reading it would destroy the backup!)
/// and the new ranges streamed out of the still-original game chunks —
/// packed into 1 MiB blocks that fall back to verbatim storage when LZSS
/// cannot shrink them (MPEG-2 video barely compresses).
///
/// The per-entry CRC32 (seeded from the record bytes, then fed the payload
/// as it streams through — old v1/v2 entries included, which upgrades them
/// to verifiable v3 entries) is only known after the payload has been
/// written, so the fields are written as placeholders and patched back
/// afterwards; the file is renamed into place only once it is complete.
/// A v3 old sidecar is re-verified on the way through: when a stored CRC
/// no longer matches its bytes, the rip is refused (`error` says so)
/// instead of blessing the corrupted data with freshly computed
/// checksums.
bool saveSidecar(const std::string& path, const Sidecar& oldSc,
                 const std::vector<SidecarEntry>& newEntries,
                 GameStream& stream, std::string* error = nullptr) {
    const std::string tmp = path + ".new";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;

    uint64_t oldRaw = 0, newRaw = 0;
    for (const auto& e : oldSc.entries) oldRaw += e.size;
    for (const auto& e : newEntries) newRaw += e.size;

    // Output entry order == payload order: old entries first, then the new
    // ones. `all` drives the CRC feed (a payload read may span entries).
    std::vector<SidecarEntry> all(oldSc.entries);
    all.insert(all.end(), newEntries.begin(), newEntries.end());
    std::vector<uint32_t> crcs(all.size());
    for (size_t i = 0; i < all.size(); ++i)
        crcs[i] = entryCrcSeed(all[i].offset, all[i].size);
    size_t crcCur = 0;                        // entry the feed is inside
    uint64_t crcOff = 0;                      // bytes of it already fed
    auto crcFeed = [&](const uint8_t* p, size_t n) {
        while (n > 0 && crcCur < all.size()) {
            const uint64_t remaining = all[crcCur].size - crcOff;
            if (remaining == 0) {             // zero-size entry: no payload
                ++crcCur;
                continue;
            }
            const size_t take = static_cast<size_t>(
                std::min<uint64_t>(n, remaining));
            crcs[crcCur] = usbutils::crc32(crcs[crcCur], p, take);
            p += take;
            n -= take;
            crcOff += take;
            if (crcOff == all[crcCur].size) {
                ++crcCur;
                crcOff = 0;
            }
        }
    };

    SidecarHeaderV2 hdr{};
    std::memcpy(hdr.magic, kMagicV3, sizeof(kMagicV3));
    hdr.count = static_cast<uint32_t>(oldSc.entries.size() + newEntries.size());
    hdr.payloadRaw = oldRaw + newRaw;
    bool ok = std::fwrite(&hdr, sizeof(hdr), 1, f) == 1;
    for (const auto& e : all) {
        SidecarEntryRecV3 rec{e.offset, e.size, 0};   // crc patched back below
        if (!ok || std::fwrite(&rec, sizeof(rec), 1, f) != 1) ok = false;
    }

    if (ok) {
        PayloadReader oldPayload(oldSc);
        if (oldRaw > 0 && !oldPayload.valid()) ok = false;

        size_t ni = 0;                         // cursor over newEntries
        uint64_t noff = 0;
        std::vector<uint8_t> raw(kLzBlockSize), comp;
        uint64_t left = hdr.payloadRaw;
        while (ok && left > 0) {
            const size_t rawLen = static_cast<size_t>(
                std::min<uint64_t>(kLzBlockSize, left));
            size_t have = 0;
            while (have < rawLen) {
                if (oldRaw > 0) {
                    const size_t want = static_cast<size_t>(
                        std::min<uint64_t>(rawLen - have, oldRaw));
                    if (!oldPayload.read(raw.data() + have, want))
                        { ok = false; break; }
                    crcFeed(raw.data() + have, want);
                    oldRaw -= want;
                    have += want;
                } else if (ni < newEntries.size()) {
                    const SidecarEntry& e = newEntries[ni];
                    const size_t want = static_cast<size_t>(
                        std::min<uint64_t>(rawLen - have, e.size - noff));
                    const size_t got = stream.read(e.offset + noff,
                                                   raw.data() + have, want);
                    if (got != want) { ok = false; break; }
                    crcFeed(raw.data() + have, want);
                    noff += want;
                    if (noff == e.size) { ++ni; noff = 0; }
                    have += want;
                } else {
                    ok = false;                // sources exhausted too early
                    break;
                }
            }
            if (!ok) break;

            comp.clear();
            uint32_t bh = rawLen;              // bit31 = 0: stored
            if (lzEncodeBlock(raw.data(), rawLen, comp)) {
                bh = rawLen | 0x80000000u;
                const uint32_t cl = static_cast<uint32_t>(comp.size());
                ok = std::fwrite(&bh, sizeof(bh), 1, f) == 1
                    && std::fwrite(&cl, sizeof(cl), 1, f) == 1
                    && std::fwrite(comp.data(), 1, comp.size(), f) == comp.size();
            } else {
                ok = std::fwrite(&bh, sizeof(bh), 1, f) == 1
                    && std::fwrite(raw.data(), 1, rawLen, f) == rawLen;
            }
            left -= rawLen;
        }
    }

    if (ok && oldSc.v3) {
        // the old payload just streamed through: its computed CRCs must
        // match what the old sidecar stored, or the backup was already
        // corrupt before this rip
        for (size_t i = 0; i < oldSc.entries.size(); ++i)
            if (crcs[i] != oldSc.entries[i].crc) {
                if (error)
                    *error = "existing backup file is corrupt (entry "
                             + std::to_string(i)
                             + " fails its CRC32): " + path;
                ok = false;
                break;
            }
    }

    if (ok) {
        // Patch the CRC fields in (they trail their records; all within the
        // header area, so the seek targets stay small even for huge files).
        const uint64_t firstCrcAt = sizeof(SidecarHeaderV2)
                                    + sizeof(SidecarEntryRecV3)
                                    - sizeof(uint32_t);
        for (size_t i = 0; ok && i < crcs.size(); ++i) {
            ok = usbutils::seek64(f, static_cast<int64_t>(
                                     firstCrcAt + i * sizeof(SidecarEntryRecV3)))
                 && std::fwrite(&crcs[i], sizeof(crcs[i]), 1, f) == 1;
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

/// A media file is "blanked without backup" when its first sector is all
/// zeros. The probe must be a FULL 2048-byte sector: 16 bytes used to be
/// enough for video (.PSS starts with an MPEG-2 pack header), but raw
/// audio (.VB bodies, .MIB streams) can legitimately begin with digital
/// silence — ADPCM silence is zero bytes — and would be misreported as
/// already blanked, which also makes ripMedia skip them. A headered
/// audio file (.VAG "VAGp", .SS2 "SShd") can never pass this probe.
/// Residual risk: a headerless stream opening with 2048 bytes of pure
/// silence — accepted, and only the "no backup" state relies on it
/// (sidecar entries are authoritative).
bool looksBlanked(GameStream& stream, const VideoEntry& v) {
    if (v.size < 2048) return false;
    uint8_t head[2048] = {};
    if (stream.read(v.offset, head, sizeof(head)) != sizeof(head))
        return false;
    for (uint8_t b : head)
        if (b != 0) return false;
    return true;
}

// ---------------------------------------------------------------------------
// scan / rip / restore — shared by the video and the audio flavor
// ---------------------------------------------------------------------------

std::vector<VideoEntry> scanMedia(const MediaDef& def,
                                  const std::string& dir,
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
            if (!e.isDir && def.isMediaName(e.path))
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
    loadSidecar(backupPath(dir, def, idHex, serial), sc);
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

RipResult ripMedia(const MediaDef& def,
                   const std::string& dir, const std::string& idHex,
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
    const std::string scPath = backupPath(dir, def, idHex, serial);
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
        newEntries.push_back({v.offset, v.size, 0});
        newBytes += v.size;
    }
    if (newEntries.empty()) {
        res.ok = true;                             // nothing new to rip
        return res;
    }

    // ---- 2. write the sidecar BEFORE touching the game ------------------
    if (makeBackup) {
        std::string scErr;
        if (!saveSidecar(scPath, sc, newEntries, stream, &scErr)) {
            res.error = scErr.empty() ? "cannot write backup file: " + scPath
                                      : scErr;
            return res;
        }
        res.backupBytes = newBytes;
    }
    // (destructive rip: an existing backup is left untouched on purpose,
    //  so previously backed-up media remain restorable)

    // ---- 3. blank the newly selected media -------------------------------
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

RipResult restoreMedia(const MediaDef& def,
                       const std::string& dir, const std::string& idHex,
                       const std::string& serial, uint32_t parts,
                       const ProgressFn& progress,
                       const std::function<bool()>& cancelled) {
    RipResult res;

    const std::string scPath = backupPath(dir, def, idHex, serial);

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

    uint64_t total = 0;
    for (const auto& e : sc.entries) total += e.size;

    // v3 backups are verified end-to-end BEFORE the first byte is written
    // into the game: a corrupt entry is reported while the media are still
    // blanked (a blanked game still boots; a half-restored one may not),
    // and the sidecar is kept for recovery from a healthy copy. The pass
    // is read-only — one extra read of the sidecar — and reports progress
    // too, so the bar runs to 100% twice (verify, then write). v1/v2
    // sidecars carry no checksums and are restored as-is after the
    // structural checks in loadSidecar.
    if (sc.v3) {
        PayloadReader verify(sc);
        if (!verify.valid()) {
            res.error = "cannot read backup file: " + scPath;
            return res;
        }
        std::vector<uint8_t> vbuf(4 * 1024 * 1024);
        uint64_t verified = 0;
        for (size_t i = 0; i < sc.entries.size(); ++i) {
            const SidecarEntry& e = sc.entries[i];
            uint32_t crc = entryCrcSeed(e.offset, e.size);
            uint64_t left = e.size;
            while (left > 0) {
                if (cancelled && cancelled()) {
                    res.error = "cancelled";
                    return res;
                }
                const size_t want = static_cast<size_t>(
                    std::min<uint64_t>(vbuf.size(), left));
                if (!verify.read(vbuf.data(), want)) {
                    res.error = "backup file is truncated or corrupt — "
                                "nothing was restored yet";
                    return res;
                }
                crc = usbutils::crc32(crc, vbuf.data(), want);
                left -= want;
                verified += want;
                if (progress) progress(verified, total);
            }
            if (crc != e.crc) {
                const std::string name = mediaNameAt(stream, e.offset);
                res.error = "backup file is corrupt: entry "
                            + std::to_string(i) + " ("
                            + (name.empty()
                               ? "media at stream offset "
                                 + std::to_string(e.offset)
                               : name)
                            + ") failed its CRC32 — nothing was restored "
                              "yet; keep the sidecar and restore the "
                              "originals from a healthy copy";
                return res;
            }
        }
    }

    PayloadReader payload(sc);
    if (!payload.valid()) {
        res.error = "cannot read backup file: " + scPath;
        return res;
    }

    std::vector<uint8_t> buf(4 * 1024 * 1024);
    uint64_t done = 0;
    bool ok = true;
    for (const auto& e : sc.entries) {
        uint64_t left = e.size;
        while (left > 0) {
            if (cancelled && cancelled()) { ok = false; res.error = "cancelled"; break; }
            const size_t want = static_cast<size_t>(
                std::min<uint64_t>(buf.size(), left));
            if (!payload.read(buf.data(), want)) {
                ok = false;
                res.error = "backup file is truncated or corrupt";
                break;
            }
            if (!stream.write(e.offset + (e.size - left), buf.data(), want)) {
                ok = false;
                res.error = "write error while restoring";
                break;
            }
            left -= want;
            done += want;
            res.blankedBytes += want;         // bytes restored
            if (progress) progress(done, total);
        }
        if (ok) ++res.rippedCount;
        else break;
    }

    if (!ok) return res;

    std::remove(scPath.c_str());             // backup fully consumed
    res.ok = true;
    return res;
}

} // namespace

// ---------------------------------------------------------------------------
// video flavor
// ---------------------------------------------------------------------------

std::string videoBackupFileName(const std::string& idHex,
                                const std::string& serial) {
    return chunkFileName(idHex, serial, 0) + ".rip";
}

uint64_t videoBackupSize(const std::string& dir, const std::string& idHex,
                         const std::string& serial) {
    return fileSizeBytes(joinPath(dir, videoBackupFileName(idHex, serial)));
}

std::vector<VideoEntry> scanVideos(const std::string& dir,
                                   const std::string& idHex,
                                   const std::string& serial,
                                   uint32_t parts,
                                   std::string* error) {
    return scanMedia(kVideo, dir, idHex, serial, parts, error);
}

RipResult ripVideos(const std::string& dir, const std::string& idHex,
                    const std::string& serial, uint32_t parts,
                    const std::vector<VideoEntry>& selection,
                    bool makeBackup,
                    const ProgressFn& progress,
                    const std::function<bool()>& cancelled) {
    return ripMedia(kVideo, dir, idHex, serial, parts, selection,
                    makeBackup, progress, cancelled);
}

RipResult restoreVideos(const std::string& dir, const std::string& idHex,
                        const std::string& serial, uint32_t parts,
                        const ProgressFn& progress,
                        const std::function<bool()>& cancelled) {
    return restoreMedia(kVideo, dir, idHex, serial, parts, progress, cancelled);
}

// ---------------------------------------------------------------------------
// audio flavor (USBUtils extension — the original tool had no audio rip)
// ---------------------------------------------------------------------------

std::string audioBackupFileName(const std::string& idHex,
                                const std::string& serial) {
    return chunkFileName(idHex, serial, 0) + ".arip";
}

uint64_t audioBackupSize(const std::string& dir, const std::string& idHex,
                         const std::string& serial) {
    return fileSizeBytes(joinPath(dir, audioBackupFileName(idHex, serial)));
}

std::vector<std::string> backupFileNames(const std::string& idHex,
                                         const std::string& serial) {
    return {videoBackupFileName(idHex, serial),
            audioBackupFileName(idHex, serial)};
}

std::vector<VideoEntry> scanAudios(const std::string& dir,
                                   const std::string& idHex,
                                   const std::string& serial,
                                   uint32_t parts,
                                   std::string* error) {
    return scanMedia(kAudio, dir, idHex, serial, parts, error);
}

RipResult ripAudios(const std::string& dir, const std::string& idHex,
                    const std::string& serial, uint32_t parts,
                    const std::vector<VideoEntry>& selection,
                    bool makeBackup,
                    const ProgressFn& progress,
                    const std::function<bool()>& cancelled) {
    return ripMedia(kAudio, dir, idHex, serial, parts, selection,
                    makeBackup, progress, cancelled);
}

RipResult restoreAudios(const std::string& dir, const std::string& idHex,
                        const std::string& serial, uint32_t parts,
                        const ProgressFn& progress,
                        const std::function<bool()>& cancelled) {
    return restoreMedia(kAudio, dir, idHex, serial, parts, progress, cancelled);
}

} // namespace usbutils
