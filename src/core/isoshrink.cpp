// SPDX-License-Identifier: MIT
#include "isoshrink.h"
#include "gamestream.h"
#include "iso9660.h"
#include "gamefile.h"
#include "videorip.h"
#include "ps2load.h"
#include "ulcfg.h"
#include "port.h"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <map>
#include <vector>

namespace usbutils {

namespace {

constexpr uint32_t kSector = 2048;
constexpr uint64_t kMaxDirBytes = 8ull << 20;      // per-directory read cap
constexpr size_t   kMaxDirs = 4096;                // corrupt-image guard
constexpr uint64_t kMaxTotalDirBytes = 256ull << 20;
constexpr uint64_t kIoChunk = 4ull << 20;          // streaming buffer size

// Primary Volume Descriptor byte offsets (ECMA-119).
constexpr size_t kPvdVolumeSpace   = 80;
constexpr size_t kPvdPathTableSize = 132;
constexpr size_t kPvdPathL    = 140;
constexpr size_t kPvdPathLOpt = 144;
constexpr size_t kPvdPathM    = 148;
constexpr size_t kPvdPathMOpt = 152;
constexpr size_t kPvdRootRecord = 156;

constexpr uint64_t kFnvBasis = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

uint32_t getLe(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}
uint32_t getBe(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24)
         | (static_cast<uint32_t>(p[1]) << 16)
         | (static_cast<uint32_t>(p[2]) << 8)
         | static_cast<uint32_t>(p[3]);
}
void putLe(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);         p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);   p[3] = static_cast<uint8_t>(v >> 24);
}
void putBe(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);   p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);    p[3] = static_cast<uint8_t>(v);
}
/// Directory-record LBA fields are recorded twice: little- then big-endian.
void putBoth(uint8_t* p, uint32_t v) { putLe(p, v); putBe(p + 4, v); }

uint32_t sectorsOf(uint64_t bytes) {
    return static_cast<uint32_t>((bytes + kSector - 1) / kSector);
}

std::string cleanName(const std::string& raw) {
    std::string n = raw;
    if (!n.empty() && n.back() == '\0') n.pop_back();
    const size_t semi = n.find(';');
    if (semi != std::string::npos) n.resize(semi);
    return n;
}

uint64_t fileSizeOnDisk(const std::string& p) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return 0;
    const bool ok = seek64(f, 0, SEEK_END);
    const int64_t sz = ok ? tell64(f) : -1;
    std::fclose(f);
    return sz < 0 ? 0 : static_cast<uint64_t>(sz);
}

// ---------------------------------------------------------------------------
// shrink model
// ---------------------------------------------------------------------------

struct Extent {
    uint32_t lba;
    uint32_t size;
};

struct FileNode {
    std::string path;                // '\\' separators, ";N" stripped
    std::string rawName;             // record identifier (grouping key)
    int         parent = -1;         // index into Model::dirs
    std::vector<Extent> exts;        // one per record, in record order
    std::vector<size_t>  recOffs;    // record offset in the parent's content
    uint64_t    total = 0;
    uint64_t    hash = kFnvBasis;
    bool        allZero = false;
    int         dupOf = -1;          // relink target (exact duplicate)
    int         zeroAnchor = -1;     // relink target (all-zero content)
    std::vector<uint32_t> newLbas;   // per extent, filled by the planner
};

struct FileRef {                      // a file record inside a directory
    int      file;
    size_t   off;                     // byte offset in the dir's content
    size_t   ext;                     // extent index within the file
};

struct DirRef {
    int      dir;
    size_t   off;
};

struct DirNode {
    std::string path;                 // "" = root
    std::string rawName;
    int         parent = -1;
    uint32_t    lba = 0, size = 0;
    std::vector<uint8_t> content;     // the whole extent, records patched here
    size_t      selfDot = SIZE_MAX, parentDot = SIZE_MAX;   // record offsets
    std::vector<FileRef> files;
    std::vector<DirRef>  dirs;
    uint32_t    newLba = 0;
};

struct Move {
    uint64_t src, dst;                // absolute stream byte offsets
    uint32_t size;
};

struct Model {
    std::vector<DirNode>  dirs;       // [0] = root, discovery order
    std::vector<FileNode> files;
    uint32_t volumeSectors = 0;
    uint32_t vdEnd = 17;              // first sector past the descriptors
    uint32_t ptSize = 0;
    uint32_t oldPtL = 0, oldPtM = 0, oldPtLOpt = 0, oldPtMOpt = 0;
    std::vector<uint8_t> ptL, ptM;    // original table bytes (patched later)
    uint64_t streamSize = 0;

    // plan
    uint32_t newPtL = 0, newPtM = 0, newPtLOpt = 0, newPtMOpt = 0;
    uint32_t finalSectors = 0;
    std::vector<Move> moves;
    uint64_t duplicateBytes = 0, zeroBytes = 0;
    size_t   duplicateFiles = 0, zeroFiles = 0;
    uint64_t hashBytes = 0;           // progress accounting
};

bool readBytes(GameStream& s, uint64_t off, void* dst, size_t n) {
    return n == 0 || s.read(off, dst, n) == n;
}

// ---------------------------------------------------------------------------
// volume descriptor scan — finds where the descriptors end and refuses the
// layouts a compacting rewrite cannot keep consistent (Joliet SVD, UDF)
// ---------------------------------------------------------------------------

bool scanDescriptors(GameStream& s, uint32_t volumeSectors, uint32_t& vdEnd,
                     std::string& err) {
    uint8_t sec[kSector];
    for (uint32_t lba = 16; lba < volumeSectors && lba < 16 + 32; ++lba) {
        if (!readBytes(s, static_cast<uint64_t>(lba) * kSector, sec, kSector)) {
            err = "cannot read the volume descriptor region";
            return false;
        }
        bool zeros = true;
        for (uint8_t b : sec) if (b) { zeros = false; break; }
        if (zeros) { vdEnd = lba; return true; }   // tolerated (no terminator)

        const std::string magic(reinterpret_cast<char*>(sec) + 1, 5);
        if (magic == "CD001") {
            const uint8_t type = sec[0];
            if (type == 255) { vdEnd = lba + 1; return true; }
            if (type != 1) {
                err = "image has a supplementary volume descriptor "
                      "(Joliet) — shrinking it is not supported";
                return false;
            }
            continue;                               // another PVD — keep going
        }
        if (magic == "BEA01" || magic == "NSR02" || magic == "NSR03") {
            err = "image carries UDF descriptors — shrinking it is not "
                  "supported (re-create it as plain ISO9660)";
            return false;
        }
        if (magic == "TEA01") { vdEnd = lba + 1; return true; }
        // Anything else is not a volume descriptor: the set ended without
        // a terminator (tolerated — e.g. images this tool already shrunk
        // compact their data right behind the descriptors).
        vdEnd = lba;
        return true;
    }
    err = "no end of the volume descriptor set found";
    return false;
}

// ---------------------------------------------------------------------------
// directory tree scan
// ---------------------------------------------------------------------------

bool parseDirectory(Model& m, GameStream& s, int dirIdx,
                    std::vector<int>& pending,
                    std::map<uint32_t, int>& dirByLba,
                    std::string& err) {
    DirNode& d = m.dirs[dirIdx];
    if (d.size == 0 || d.size > kMaxDirBytes) {
        err = "directory extent has an invalid size: " + d.path;
        return false;
    }
    d.content.resize(d.size);
    if (!readBytes(s, static_cast<uint64_t>(d.lba) * kSector,
                   d.content.data(), d.content.size())) {
        err = "cannot read directory extent: " + d.path;
        return false;
    }

    int lastFile = -1;
    const uint8_t* c = d.content.data();
    size_t off = 0;
    while (off + 1 < d.content.size()) {
        const uint8_t recLen = c[off];
        if (recLen == 0) {                       // padding to sector boundary
            off = (off / kSector + 1) * kSector;
            continue;
        }
        if (recLen % 2 || off + recLen > d.content.size()) {
            err = "corrupt directory record in " + d.path;
            return false;
        }
        const uint8_t nameLen = c[off + 32];
        if (recLen < 34 || size_t(nameLen) + 33 > recLen) {
            err = "corrupt directory record in " + d.path;
            return false;
        }
        if (c[off + 1] || c[off + 26] || c[off + 27]) {
            err = "extended-attribute or interleaved records are not "
                  "supported (" + d.path + ")";
            return false;
        }
        // Anything nonzero past the identifier (pad byte aside) is system-
        // use data — Rock Ridge SUSP entries carry data we cannot relocate.
        size_t nameEnd = off + 33 + nameLen;
        if (nameEnd % 2) ++nameEnd;
        for (size_t i = nameEnd; i < off + recLen; ++i) {
            if (c[i]) {
                err = "Rock Ridge / system-use data is not supported ("
                      + d.path + ")";
                return false;
            }
        }

        const std::string raw(reinterpret_cast<const char*>(c + off + 33),
                              nameLen);
        const uint32_t lba  = getLe(c + off + 2);
        const uint32_t size = getLe(c + off + 10);
        const bool    isDir = (c[off + 25] & 0x02) != 0;

        if (nameLen == 1 && c[off + 33] < 2) {   // "." / ".."
            if (c[off + 33] == 0) d.selfDot = off;
            else                   d.parentDot = off;
            off += recLen;
            continue;
        }
        if (isDir) {
            if (m.dirs.size() >= kMaxDirs) {
                err = "too many directories (image corrupt?)";
                return false;
            }
            const auto alias = dirByLba.find(lba);
            if (alias != dirByLba.end()) {
                // two names for one extent (or a self-reference): both
                // records are re-pointed at the node's new LBA
                d.dirs.push_back({alias->second, off});
            } else if (lba == d.lba) {
                d.dirs.push_back({dirIdx, off});
            } else {
                DirNode nd;
                nd.rawName = raw;
                nd.path = d.path.empty() ? cleanName(raw)
                                         : d.path + "\\" + cleanName(raw);
                nd.parent = dirIdx;
                nd.lba = lba;
                nd.size = size;
                d.dirs.push_back({static_cast<int>(m.dirs.size()), off});
                dirByLba[lba] = static_cast<int>(m.dirs.size());
                m.dirs.push_back(std::move(nd));
                pending.push_back(static_cast<int>(m.dirs.size() - 1));
            }
            lastFile = -1;
        } else {
            // consecutive records with the same identifier form one
            // multi-extent file (files larger than 4 GiB)
            if (lastFile >= 0 && m.files[lastFile].rawName == raw
                && m.files[lastFile].parent == dirIdx) {
                FileNode& f = m.files[lastFile];
                f.exts.push_back({lba, size});
                f.recOffs.push_back(off);
                d.files.push_back({lastFile, off, f.exts.size() - 1});
                f.total += size;
            } else {
                FileNode f;
                f.rawName = raw;
                f.path = d.path.empty() ? cleanName(raw)
                                        : d.path + "\\" + cleanName(raw);
                f.parent = dirIdx;
                f.exts.push_back({lba, size});
                f.recOffs.push_back(off);
                f.total = size;
                d.files.push_back({static_cast<int>(m.files.size()), off, 0});
                m.files.push_back(std::move(f));
                lastFile = static_cast<int>(m.files.size() - 1);
            }
        }
        off += recLen;
    }
    return true;
}

// ---------------------------------------------------------------------------
// progress accumulator — three phases share one (done, total) curve
// ---------------------------------------------------------------------------

struct ProgressAcc {
    ProgressFn  fn;
    uint64_t    total = 0;
    uint64_t    base = 0;             // bytes finished in earlier phases
    void report(uint64_t phaseDone) const {
        if (fn) fn(base + phaseDone, total);
    }
};

// ---------------------------------------------------------------------------
// hashing and content comparison
// ---------------------------------------------------------------------------

bool hashFile(GameStream& s, FileNode& f, uint64_t& phaseDone,
              const ProgressAcc& acc, const std::function<bool()>& cancelled,
              std::string& err) {
    std::vector<uint8_t> buf(kIoChunk);
    uint64_t h = kFnvBasis;
    bool allZero = true;
    for (const Extent& e : f.exts) {
        uint64_t left = e.size;
        while (left > 0) {
            if (cancelled && cancelled()) { err = "cancelled"; return false; }
            const size_t want = static_cast<size_t>(
                std::min<uint64_t>(buf.size(), left));
            if (s.read(static_cast<uint64_t>(e.lba) * kSector
                       + (e.size - left), buf.data(), want) != want) {
                err = "read error while hashing " + f.path;
                return false;
            }
            for (size_t i = 0; i < want; ++i) {
                h ^= buf[i];
                h *= kFnvPrime;
                if (buf[i]) allZero = false;
            }
            left       -= want;
            phaseDone  += want;
            acc.report(phaseDone);
        }
    }
    f.hash    = h;
    f.allZero = allZero && f.total > 0;
    return true;
}

/// Byte-compare two files (logical content, extents in order) — the hash
/// match is only a hint; a relink decision must survive a full compare.
bool filesIdentical(GameStream& s, const FileNode& a, const FileNode& b) {
    if (a.exts.size() != b.exts.size()) return false;
    for (size_t i = 0; i < a.exts.size(); ++i)
        if (a.exts[i].size != b.exts[i].size) return false;
    std::vector<uint8_t> ba(kIoChunk), bb(kIoChunk);
    for (size_t i = 0; i < a.exts.size(); ++i) {
        uint64_t left = a.exts[i].size;
        while (left > 0) {
            const size_t want = static_cast<size_t>(
                std::min<uint64_t>(ba.size(), left));
            if (s.read(static_cast<uint64_t>(a.exts[i].lba) * kSector
                       + (a.exts[i].size - left), ba.data(), want) != want)
                return false;
            if (s.read(static_cast<uint64_t>(b.exts[i].lba) * kSector
                       + (b.exts[i].size - left), bb.data(), want) != want)
                return false;
            if (std::memcmp(ba.data(), bb.data(), want) != 0) return false;
            left -= want;
        }
    }
    return true;
}

uint64_t footprintOf(const FileNode& f) {
    uint64_t n = 0;
    for (const Extent& e : f.exts) n += uint64_t(sectorsOf(e.size)) * kSector;
    return n;
}

// ---------------------------------------------------------------------------
// model build: scan + hash + relink analysis + layout plan
// ---------------------------------------------------------------------------

bool buildModel(GameStream& s, Model& m, std::string& err,
                ProgressAcc& acc,
                const std::function<bool()>& cancelled) {
    m.streamSize = s.size();

    // ---- PVD -------------------------------------------------------------
    uint8_t pvd[kSector];
    if (!readBytes(s, 16ull * kSector, pvd, kSector)
        || std::memcmp(pvd + 1, "CD001", 5) != 0) {
        err = "not a valid ISO9660 image (game data corrupt?)";
        return false;
    }
    const uint32_t blockSize = pvd[128] | (uint32_t(pvd[129]) << 8);
    if (blockSize != 0 && blockSize != kSector) {  // 0: field unset (lenient)
        err = "the image does not use 2048-byte sectors";
        return false;
    }
    m.volumeSectors = getLe(pvd + kPvdVolumeSpace);
    if (m.volumeSectors < 17
        || uint64_t(m.volumeSectors) * kSector > m.streamSize) {
        err = "the game data is shorter than its volume says — "
              "re-install the game";
        return false;
    }
    if (!scanDescriptors(s, m.volumeSectors, m.vdEnd, err)) return false;

    m.ptSize     = getLe(pvd + kPvdPathTableSize);
    m.oldPtL     = getLe(pvd + kPvdPathL);
    m.oldPtM     = getBe(pvd + kPvdPathM);
    m.oldPtLOpt  = getLe(pvd + kPvdPathLOpt);
    m.oldPtMOpt  = getBe(pvd + kPvdPathMOpt);
    if (m.ptSize > m.streamSize) {
        err = "path table size is invalid";
        return false;
    }
    if (m.ptSize > 0) {
        if (m.oldPtL < m.vdEnd || m.oldPtM < m.vdEnd
            || uint64_t(m.oldPtL) * kSector + m.ptSize > m.streamSize
            || uint64_t(m.oldPtM) * kSector + m.ptSize > m.streamSize
            || (m.oldPtLOpt && (m.oldPtLOpt < m.vdEnd
                || uint64_t(m.oldPtLOpt) * kSector + m.ptSize > m.streamSize))
            || (m.oldPtMOpt && (m.oldPtMOpt < m.vdEnd
                || uint64_t(m.oldPtMOpt) * kSector + m.ptSize > m.streamSize))) {
            err = "path table location is invalid";
            return false;
        }
        m.ptL.resize(m.ptSize);
        m.ptM.resize(m.ptSize);
        if (!readBytes(s, uint64_t(m.oldPtL) * kSector, m.ptL.data(), m.ptSize)
            || !readBytes(s, uint64_t(m.oldPtM) * kSector, m.ptM.data(),
                          m.ptSize)) {
            err = "cannot read the path tables";
            return false;
        }
    }

    // ---- directory tree ----------------------------------------------------
    DirNode root;
    root.lba  = getLe(pvd + kPvdRootRecord + 2);
    root.size = getLe(pvd + kPvdRootRecord + 10);
    m.dirs.push_back(std::move(root));

    std::map<uint32_t, int> dirByLba;
    dirByLba[m.dirs[0].lba] = 0;
    std::vector<int> pending{0};
    while (!pending.empty()) {
        const int idx = pending.back();
        pending.pop_back();
        if (!parseDirectory(m, s, idx, pending, dirByLba, err)) return false;
    }
    uint64_t totalDirBytes = 0;
    for (const DirNode& d : m.dirs) totalDirBytes += d.size;
    if (totalDirBytes > kMaxTotalDirBytes) {
        err = "directory data is implausibly large (image corrupt?)";
        return false;
    }
    for (const DirNode& d : m.dirs) {
        if (d.lba < m.vdEnd
            || uint64_t(d.lba) * kSector + d.size
               > uint64_t(m.volumeSectors) * kSector) {
            err = "directory extent lies outside the volume: " + d.path;
            return false;
        }
    }
    for (const FileNode& f : m.files) {
        for (const Extent& e : f.exts) {
            if (e.lba < m.vdEnd
                || uint64_t(e.lba) * kSector + e.size
                   > uint64_t(m.volumeSectors) * kSector) {
                err = "file extent lies outside the volume: " + f.path;
                return false;
            }
        }
    }

    // ---- hash every file ---------------------------------------------------
    for (const FileNode& f : m.files) m.hashBytes += f.total;
    acc.total = m.hashBytes;
    uint64_t phaseDone = 0;
    for (FileNode& f : m.files) {
        if (f.total == 0) continue;
        if (!hashFile(s, f, phaseDone, acc, cancelled, err)) return false;
    }

    // ---- relink analysis ---------------------------------------------------
    std::map<std::pair<uint64_t, uint64_t>, std::vector<int>> groups;
    for (size_t i = 0; i < m.files.size(); ++i) {
        const FileNode& f = m.files[i];
        if (f.total > 0 && !f.allZero)
            groups[{f.total, f.hash}].push_back(static_cast<int>(i));
    }
    for (auto& kv : groups) {
        std::vector<int>& g = kv.second;
        if (g.size() < 2) continue;
        std::sort(g.begin(), g.end(), [&](int a, int b) {
            return m.files[a].exts[0].lba < m.files[b].exts[0].lba;
        });
        for (size_t i = 1; i < g.size(); ++i) {
            if (filesIdentical(s, m.files[g[0]], m.files[g[i]])) {
                m.files[g[i]].dupOf = g[0];
                // an already-relinked copy shares the canonical's extent —
                // reclaiming it frees nothing, so it must not be counted
                if (m.files[g[i]].exts[0].lba != m.files[g[0]].exts[0].lba) {
                    m.duplicateBytes += footprintOf(m.files[g[i]]);
                    ++m.duplicateFiles;
                }
            }
        }
    }

    std::vector<int> zeros;                       // single-extent, all zero
    for (size_t i = 0; i < m.files.size(); ++i)
        if (m.files[i].allZero && m.files[i].exts.size() == 1)
            zeros.push_back(static_cast<int>(i));
    if (zeros.size() >= 2) {
        int anchor = zeros[0];
        for (int i : zeros)
            if (m.files[i].total > m.files[anchor].total
                || (m.files[i].total == m.files[anchor].total
                    && m.files[i].exts[0].lba < m.files[anchor].exts[0].lba))
                anchor = i;
        for (int i : zeros) {
            if (i == anchor) continue;
            m.files[i].zeroAnchor = anchor;
            if (m.files[i].exts[0].lba != m.files[anchor].exts[0].lba) {
                m.zeroBytes += footprintOf(m.files[i]);
                ++m.zeroFiles;
            }
        }
    }

    // ---- layout plan ---------------------------------------------------------
    uint64_t cursor = m.vdEnd;
    if (m.ptSize > 0) {
        m.newPtL = static_cast<uint32_t>(cursor);
        cursor += sectorsOf(m.ptSize);
        m.newPtM = static_cast<uint32_t>(cursor);
        cursor += sectorsOf(m.ptSize);
        if (m.oldPtLOpt) {
            m.newPtLOpt = static_cast<uint32_t>(cursor);
            cursor += sectorsOf(m.ptSize);
        }
        if (m.oldPtMOpt) {
            m.newPtMOpt = static_cast<uint32_t>(cursor);
            cursor += sectorsOf(m.ptSize);
        }
    }
    for (DirNode& d : m.dirs) {
        d.newLba = static_cast<uint32_t>(cursor);
        cursor += sectorsOf(d.size);
    }

    std::vector<int> moved;
    for (size_t i = 0; i < m.files.size(); ++i)
        if (m.files[i].total > 0 && m.files[i].dupOf < 0
            && m.files[i].zeroAnchor < 0)
            moved.push_back(static_cast<int>(i));
    std::sort(moved.begin(), moved.end(), [&](int a, int b) {
        return m.files[a].exts[0].lba < m.files[b].exts[0].lba;
    });
    for (int fi : moved) {
        FileNode& f = m.files[fi];
        f.newLbas.resize(f.exts.size());
        for (size_t j = 0; j < f.exts.size(); ++j) {
            if (j > 0 && f.exts[j].lba < f.exts[j - 1].lba) {
                err = "multi-extent file is not sequential: " + f.path;
                return false;
            }
            f.newLbas[j] = static_cast<uint32_t>(cursor);
            cursor += sectorsOf(f.exts[j].size);
            m.moves.push_back({uint64_t(f.exts[j].lba) * kSector,
                               uint64_t(f.newLbas[j]) * kSector,
                               f.exts[j].size});
        }
    }
    for (FileNode& f : m.files) {
        if (f.dupOf >= 0) {
            f.newLbas = m.files[f.dupOf].newLbas;   // structure verified equal
        } else if (f.zeroAnchor >= 0) {
            f.newLbas.assign(1, m.files[f.zeroAnchor].newLbas[0]);
        } else if (f.total == 0) {
            f.newLbas.resize(f.exts.size());
            for (size_t j = 0; j < f.exts.size(); ++j)
                f.newLbas[j] = f.exts[j].lba;       // never dereferenced
        }
    }

    // In-place compaction safety: sorted by source, every move goes DOWN
    // (dst <= src) and no two moves share source bytes. Writes then always
    // stay below the sources of the moves still to come.
    std::sort(m.moves.begin(), m.moves.end(),
              [](const Move& a, const Move& b) { return a.src < b.src; });
    for (size_t i = 0; i < m.moves.size(); ++i) {
        const Move& mv = m.moves[i];
        if (mv.dst > mv.src) {
            err = "this layout cannot be compacted in place "
                  "(data sits before the directories)";
            return false;
        }
        if (i + 1 < m.moves.size()
            && mv.src + mv.size > m.moves[i + 1].src) {
            err = "two files share one extent — refusing to shrink";
            return false;
        }
    }
    m.finalSectors = static_cast<uint32_t>(cursor);
    return true;
}

// ---------------------------------------------------------------------------
// apply
// ---------------------------------------------------------------------------

bool zeroRange(GameStream& s, uint64_t from, uint64_t to) {
    static const std::vector<uint8_t> zeros(kIoChunk, 0);
    while (from < to) {
        const size_t want = static_cast<size_t>(
            std::min<uint64_t>(zeros.size(), to - from));
        if (!s.write(from, zeros.data(), want)) return false;
        from += want;
    }
    return true;
}

/// Zero the pad between a region's end and its sector boundary.
bool zeroTail(GameStream& s, uint64_t byteOff, uint64_t size) {
    const uint64_t end = byteOff + size;
    const uint64_t full = (end + kSector - 1) / kSector * kSector;
    return end == full ? true : zeroRange(s, end, full);
}

/// Copy one extent downward inside the stream. dst <= src always holds, so
/// a plain forward copy is overlap-safe (reads never overtake writes).
bool copyDown(GameStream& s, const Move& mv, std::vector<uint8_t>& buf,
              uint64_t& phaseDone, const ProgressAcc& acc) {
    if (mv.dst == mv.src) return true;
    uint64_t done = 0;
    while (done < mv.size) {
        const size_t want = static_cast<size_t>(
            std::min<uint64_t>(buf.size(), mv.size - done));
        if (s.read(mv.src + done, buf.data(), want) != want) return false;
        if (!s.write(mv.dst + done, buf.data(), want)) return false;
        done       += want;
        phaseDone  += want;
        acc.report(phaseDone);
    }
    return true;
}

/// Patch the extent LBAs of a path table's entries in place (in `bytes`).
bool patchPathTable(Model& m, std::vector<uint8_t>& bytes, bool bigEndian,
                    std::string& err) {
    std::map<std::pair<int, std::string>, int> byParentName;
    for (size_t i = 1; i < m.dirs.size(); ++i)
        byParentName[{m.dirs[i].parent, m.dirs[i].rawName}]
            = static_cast<int>(i);

    std::vector<int> entryDir;                     // entry number -> dir
    size_t pos = 0;
    int entry = 1;
    while (pos < bytes.size()) {
        if (pos + 8 > bytes.size()) { err = "path table is corrupt"; return false; }
        const uint8_t di = bytes[pos];
        const size_t len = 8 + di + (di & 1);
        if (pos + len > bytes.size()) { err = "path table is corrupt"; return false; }
        const uint32_t parentEntry = bigEndian
            ? (uint32_t(bytes[pos + 6]) << 8) | bytes[pos + 7]
            : (uint32_t(bytes[pos + 7]) << 8) | bytes[pos + 6];
        if (entry > 1 && (parentEntry == 0 || parentEntry >= size_t(entry))) {
            err = "path table is corrupt";
            return false;
        }
        int dirIdx;
        if (entry == 1) {
            dirIdx = 0;                            // root, name byte 0x00
        } else {
            const std::string name(reinterpret_cast<const char*>(bytes.data()
                                                    + pos + 8), di);
            const auto it = byParentName.find({entryDir[parentEntry - 1], name});
            if (it == byParentName.end()) {
                err = "path table does not match the directory tree";
                return false;
            }
            dirIdx = it->second;
        }
        if (entryDir.size() < size_t(entry)) entryDir.resize(entry);
        entryDir[entry - 1] = dirIdx;
        if (bigEndian) putBe(bytes.data() + pos + 2, m.dirs[dirIdx].newLba);
        else           putLe(bytes.data() + pos + 2, m.dirs[dirIdx].newLba);
        pos += len;
        ++entry;
    }
    return true;
}

void patchDirectoryRecords(Model& m) {
    for (DirNode& d : m.dirs) {
        if (d.selfDot != SIZE_MAX)
            putBoth(d.content.data() + d.selfDot + 2, d.newLba);
        if (d.parentDot != SIZE_MAX) {
            const uint32_t p = d.parent >= 0 ? m.dirs[d.parent].newLba
                                             : d.newLba;
            putBoth(d.content.data() + d.parentDot + 2, p);
        }
        for (const FileRef& fr : d.files)
            putBoth(d.content.data() + fr.off + 2,
                    m.files[fr.file].newLbas[fr.ext]);
        for (const DirRef& dr : d.dirs)
            putBoth(d.content.data() + dr.off + 2, m.dirs[dr.dir].newLba);
    }
}

bool applyModel(GameStream& s, Model& m, const std::string& dir,
                const std::string& idHex, const std::string& serial,
                uint32_t oldParts, ShrinkResult& res, std::string& err,
                const ProgressFn& progress) {
    uint64_t moveBytes = 0, verifyBytes = 0;
    for (const Move& mv : m.moves) moveBytes += mv.size;
    for (const FileNode& f : m.files) verifyBytes += f.total;
    ProgressAcc acc{progress, m.hashBytes + moveBytes + verifyBytes,
                    m.hashBytes};

    // ---- 1. move the surviving file data down -----------------------------
    {
        std::vector<uint8_t> buf(kIoChunk);
        uint64_t phaseDone = 0;
        for (const Move& mv : m.moves) {
            if (!copyDown(s, mv, buf, phaseDone, acc)) {
                err = "write error while moving " + std::to_string(mv.size)
                      + " bytes to LBA "
                      + std::to_string(mv.dst / kSector);
                return false;
            }
            if (!zeroTail(s, mv.dst, mv.size)) {
                err = "write error while padding an extent";
                return false;
            }
        }
    }

    // ---- 2. path tables ----------------------------------------------------
    if (m.ptSize > 0) {
        if (!patchPathTable(m, m.ptL, false, err)) return false;
        if (!patchPathTable(m, m.ptM, true, err)) return false;
        if (!s.write(uint64_t(m.newPtL) * kSector, m.ptL.data(), m.ptL.size())
            || !zeroTail(s, uint64_t(m.newPtL) * kSector, m.ptL.size())) {
            err = "write error while writing the path table";
            return false;
        }
        if (!s.write(uint64_t(m.newPtM) * kSector, m.ptM.data(), m.ptM.size())
            || !zeroTail(s, uint64_t(m.newPtM) * kSector, m.ptM.size())) {
            err = "write error while writing the path table";
            return false;
        }
        if (m.newPtLOpt
            && (!s.write(uint64_t(m.newPtLOpt) * kSector, m.ptL.data(),
                         m.ptL.size())
                || !zeroTail(s, uint64_t(m.newPtLOpt) * kSector,
                             m.ptL.size()))) {
            err = "write error while writing the path table";
            return false;
        }
        if (m.newPtMOpt
            && (!s.write(uint64_t(m.newPtMOpt) * kSector, m.ptM.data(),
                         m.ptM.size())
                || !zeroTail(s, uint64_t(m.newPtMOpt) * kSector,
                             m.ptM.size()))) {
            err = "write error while writing the path table";
            return false;
        }
    }

    // ---- 3. directories ----------------------------------------------------
    patchDirectoryRecords(m);
    for (const DirNode& d : m.dirs) {
        if (!s.write(uint64_t(d.newLba) * kSector, d.content.data(),
                     d.content.size())
            || !zeroTail(s, uint64_t(d.newLba) * kSector, d.content.size())) {
            err = "write error while writing directory " + d.path;
            return false;
        }
    }

    // ---- 4. PVD --------------------------------------------------------------
    {
        uint8_t pvd[kSector];
        if (!readBytes(s, 16ull * kSector, pvd, kSector)) {
            err = "cannot re-read the volume descriptor";
            return false;
        }
        putLe(pvd + kPvdVolumeSpace, m.finalSectors);
        putBe(pvd + kPvdVolumeSpace + 4, m.finalSectors);
        putBoth(pvd + kPvdRootRecord + 2, m.dirs[0].newLba);
        if (m.ptSize > 0) {
            putLe(pvd + kPvdPathL, m.newPtL);
            putLe(pvd + kPvdPathLOpt, m.newPtLOpt);
            putBe(pvd + kPvdPathM, m.newPtM);
            putBe(pvd + kPvdPathMOpt, m.newPtMOpt);
        }
        if (!s.write(16ull * kSector, pvd, kSector)) {
            err = "write error while updating the volume descriptor";
            return false;
        }
    }

    // ---- 5. cut the stream + chunk files ----------------------------------
    const uint64_t finalBytes = uint64_t(m.finalSectors) * kSector;
    uint32_t removedChunks = 0;
    if (!s.shrinkTo(finalBytes, &removedChunks)) {
        err = "the image was rewritten but could not be cut — "
              "the game still works, only the size stays";
        res.oldBytes = m.streamSize;
        res.newBytes = m.streamSize;
        res.oldParts = res.newParts = oldParts;
        return false;
    }
    const uint32_t newParts = s.parts();

    UlCfg cfg;
    if (cfg.load(dir)) {
        const size_t idx = cfg.findByIdSerial(idHex, serial);
        if (idx != SIZE_MAX) {
            cfg.records[idx].parts = newParts;
            if (!cfg.save()) {
                err = "the image was shrunk but ul.cfg could not be "
                      "updated — fix the parts field of the game record";
                res.oldBytes = m.streamSize;
                res.newBytes = finalBytes;
                res.oldParts = oldParts;
                res.newParts = newParts;
                return false;
            }
        }
    } else {
        err = "the image was shrunk but ul.cfg could not be re-read";
        res.oldBytes = m.streamSize;
        res.newBytes = finalBytes;
        res.oldParts = oldParts;
        res.newParts = newParts;
        return false;
    }

    // ---- 6. verify: re-walk and re-hash the finished image -----------------
    struct Walked {
        uint64_t size = 0;
        uint64_t hash = kFnvBasis;
    };
    std::map<std::string, Walked> walked;
    size_t walkedDirs = 0;
    const IsoReadFn reader = [&s](uint64_t off, void* dst, size_t n) {
        return s.read(off, dst, n) == n;
    };
    std::vector<uint8_t> buf(kIoChunk);
    uint64_t phaseDone = 0;
    const uint64_t verifyBase = m.hashBytes + moveBytes;
    ProgressAcc vacc{progress, acc.total, verifyBase};
    const auto verifyEntry = [&](const IsoEntry& e) -> bool {
        if (e.isDir) { ++walkedDirs; return true; }
        Walked& w = walked[e.path];
        w.size += e.size;
        uint64_t left = e.size;
        while (left > 0) {
            const size_t want = static_cast<size_t>(
                std::min<uint64_t>(buf.size(), left));
            if (s.read(uint64_t(e.lba) * kSector + (e.size - left),
                       buf.data(), want) != want) {
                err = "verification failed: cannot read " + e.path;
                return false;
            }
            for (size_t i = 0; i < want; ++i) {
                w.hash ^= buf[i];
                w.hash *= kFnvPrime;
            }
            left      -= want;
            phaseDone += want;
            vacc.report(phaseDone);
        }
        return true;
    };
    if (!walkIso(reader, verifyEntry)) {
        err = "verification failed: the rewritten image is not readable";
        res.oldBytes = m.streamSize;
        res.newBytes = finalBytes;
        res.oldParts = oldParts;
        res.newParts = newParts;
        return false;
    }
    size_t expectedDirs = 0;
    for (const DirNode& d : m.dirs) expectedDirs += d.dirs.size();
    if (walkedDirs != expectedDirs || walked.size() != m.files.size()) {
        err = "verification failed: the file tree changed";
        res.oldBytes = m.streamSize;
        res.newBytes = finalBytes;
        res.oldParts = oldParts;
        res.newParts = newParts;
        return false;
    }
    for (const FileNode& f : m.files) {
        const auto it = walked.find(f.path);
        if (it == walked.end() || it->second.size != f.total
            || it->second.hash != f.hash) {
            err = "verification failed: " + f.path + " changed";
            res.oldBytes = m.streamSize;
            res.newBytes = finalBytes;
            res.oldParts = oldParts;
            res.newParts = newParts;
            return false;
        }
    }
    {
        uint8_t pvd2[kSector];
        if (!readBytes(s, 16ull * kSector, pvd2, kSector)
            || getLe(pvd2 + kPvdVolumeSpace) != m.finalSectors) {
            err = "verification failed: the volume descriptor is wrong";
            res.oldBytes = m.streamSize;
            res.newBytes = finalBytes;
            res.oldParts = oldParts;
            res.newParts = newParts;
            return false;
        }
    }

    res.oldBytes = m.streamSize;
    res.newBytes = finalBytes;
    res.oldParts = oldParts;
    res.newParts = newParts;
    res.verified = true;
    return true;
}

bool gameHasRipBackups(const std::string& dir, const std::string& idHex,
                       const std::string& serial) {
    for (const std::string& name : backupFileNames(idHex, serial))
        if (fileSizeOnDisk(joinPath(dir, name)) > 0) return true;
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

ShrinkInfo analyzeShrink(const std::string& dir, const std::string& idHex,
                         const std::string& serial, uint32_t parts,
                         const ProgressFn& progress,
                         const std::function<bool()>& cancelled) {
    ShrinkInfo info;
    GameStream stream;
    if (!stream.open(dir, idHex, serial, parts) || stream.size() == 0) {
        info.error = "cannot open game chunks in: " + dir;
        return info;
    }
    if (queryPs2Load(dir, idHex, serial, parts).state
        != Ps2LoadState::NotInjected) {
        info.ps2LoadInjected = true;
        info.error = "this game carries a PS2Load injection, which stores "
                     "data past the volume — remove it before shrinking";
        return info;
    }
    info.hasRipBackups = gameHasRipBackups(dir, idHex, serial);

    Model m;
    std::string err;
    ProgressAcc acc{progress, 0, 0};      // total filled while hashing
    if (!buildModel(stream, m, err, acc, cancelled)) {
        info.error = err;
        return info;
    }
    info.currentBytes   = m.streamSize;
    info.duplicateBytes = m.duplicateBytes;
    info.zeroBytes      = m.zeroBytes;
    info.duplicateFiles = m.duplicateFiles;
    info.zeroFiles      = m.zeroFiles;
    info.finalBytes     = uint64_t(m.finalSectors) * kSector;
    if (info.finalBytes > m.streamSize) info.finalBytes = m.streamSize;
    const uint64_t total = info.currentBytes - info.finalBytes;
    info.gapBytes = total > info.duplicateBytes + info.zeroBytes
        ? total - info.duplicateBytes - info.zeroBytes : 0;
    info.ok = true;
    return info;
}

ShrinkResult shrinkGame(const std::string& dir, const std::string& idHex,
                        const std::string& serial, uint32_t parts,
                        bool discardBackups, const ProgressFn& progress,
                        const std::function<bool()>& cancelled) {
    ShrinkResult res;
    GameStream stream;
    if (!stream.open(dir, idHex, serial, parts) || stream.size() == 0) {
        res.error = "cannot open game chunks in: " + dir;
        return res;
    }
    if (queryPs2Load(dir, idHex, serial, parts).state
        != Ps2LoadState::NotInjected) {
        res.error = "this game carries a PS2Load injection, which stores "
                    "data past the volume — remove it before shrinking";
        return res;
    }

    Model m;
    std::string err;
    ProgressAcc acc{progress, 0, 0};
    if (!buildModel(stream, m, err, acc, cancelled)) {
        res.error = err;
        return res;
    }
    res.oldBytes = m.streamSize;
    res.oldParts = parts;
    if (uint64_t(m.finalSectors) * kSector >= m.streamSize) {
        res.ok = true;                            // nothing to gain
        res.newBytes = m.streamSize;
        res.newParts = parts;
        return res;
    }

    if (gameHasRipBackups(dir, idHex, serial)) {
        if (!discardBackups) {
            res.error = "this game has rip backups, whose stored offsets "
                        "would go stale — restore them first (unrip), or "
                        "pass --discard-backups to delete them and shrink "
                        "anyway";
            return res;
        }
        for (const std::string& name : backupFileNames(idHex, serial))
            std::remove(joinPath(dir, name).c_str());
    }

    if (!applyModel(stream, m, dir, idHex, serial, parts, res, err,
                    progress)) {
        res.error = err;
        return res;
    }
    res.ok = true;
    return res;
}

} // namespace usbutils
