// SPDX-License-Identifier: MIT
#include "isoedit.h"
#include "port.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>

namespace usbutils {

namespace {

constexpr uint32_t kSector = 2048;
constexpr uint64_t kMaxOneExtent = 0xFFFFF800ull;   ///< 4 GiB - 2048

inline uint32_t bswap32(uint32_t v) {
    return ((v & 0xffu) << 24) | (((v >> 8) & 0xffu) << 16) |
           (((v >> 16) & 0xffu) << 8) | ((v >> 24) & 0xffu);
}
inline uint16_t bswap16(uint16_t v) {
    return uint16_t(((v & 0xffu) << 8) | ((v >> 8) & 0xffu));
}

uint64_t fileSizeOf(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return ~0ull;
    const bool ok = seek64(f, 0, SEEK_END);
    const int64_t sz = ok ? tell64(f) : -1;
    std::fclose(f);
    return sz < 0 ? ~0ull : uint64_t(sz);
}

bool isDChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

#pragma pack(push, 1)
struct DirRecordHeader {           // 33 bytes, identifier follows
    uint8_t  len;
    uint8_t  extLen;
    uint32_t lbaLE, lbaBE;
    uint32_t sizeLE, sizeBE;
    uint8_t  date[7];
    uint8_t  flags;
    uint8_t  unitSize;
    uint8_t  gapSize;
    uint16_t volSeqLE, volSeqBE;
    uint8_t  nameLen;
};
#pragma pack(pop)

void putDate7(uint8_t d[7]) {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    tmv = *localtime(&t);
#else
    localtime_r(&t, &tmv);
#endif
    d[0] = uint8_t(std::max(0, tmv.tm_year - 100));   // years since 1900
    d[1] = uint8_t(tmv.tm_mon + 1);
    d[2] = uint8_t(tmv.tm_mday);
    d[3] = uint8_t(tmv.tm_hour);
    d[4] = uint8_t(tmv.tm_min);
    d[5] = uint8_t(tmv.tm_sec);
    d[6] = 0;                                          // GMT
}

/// Serialize one directory record into `out`. `ident` is the raw ISO9660
/// identifier ("SLUS_202.65;1", "." = single 0x00 byte, ".." = 0x01).
void putRecord(std::vector<uint8_t>& out, const char* ident, size_t nameLen,
               uint32_t lba, uint32_t size, uint8_t flags) {
    // The record length is a single byte: 33 + identifier (+ pad) must stay
    // <= 255. Identifiers that long only come from hostile images (a real
    // Level-1/2 name is <= 30 chars); clamp so the record stays well-formed.
    if (nameLen > 222) nameLen = 222;
    DirRecordHeader h;
    std::memset(&h, 0, sizeof(h));
    h.lbaLE = lba;      h.lbaBE = bswap32(lba);
    h.sizeLE = size;    h.sizeBE = bswap32(size);
    putDate7(h.date);
    h.flags = flags;
    h.volSeqLE = 1;     h.volSeqBE = bswap16(1);
    h.nameLen = uint8_t(nameLen);
    const size_t total = 33 + nameLen + (nameLen % 2 == 0 ? 1 : 0);
    h.len = uint8_t(total);
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(&h),
               reinterpret_cast<const uint8_t*>(&h) + 33);
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(ident),
               reinterpret_cast<const uint8_t*>(ident) + nameLen);
    if (nameLen % 2 == 0) out.push_back(0);            // pad byte to even
}

/// Bytes one directory record occupies for identifier `nameLen` long.
size_t recordSizeFor(size_t nameLen) {
    return 33 + nameLen + (nameLen % 2 == 0 ? 1 : 0);
}

/// Number of extents a file of `logical` bytes takes (ISO9660 multi-extent).
uint64_t extentCountFor(uint64_t logical) {
    if (logical == 0) return 1;
    return (logical + kMaxOneExtent - 1) / kMaxOneExtent;
}

/// Writer-side directory node.
struct WriteDir {
    IsoNode* node = nullptr;
    WriteDir* parent = nullptr;
    std::vector<std::unique_ptr<WriteDir>> children;
    uint64_t dirBytes = 0;
    uint32_t dirLba = 0;
    uint16_t pathIndex = 0;
};

/// Writer-side file extent: one contiguous output extent.
struct WriteExtent {
    const IsoNode* node = nullptr;   ///< owning file node
    uint64_t bytes = 0;              ///< logical bytes in THIS extent
    uint32_t lba = 0;
    uint64_t skip = 0;               ///< logical bytes to skip in node content
};

} // namespace

// ---------------------------------------------------------------------------
// identifier sanitising
// ---------------------------------------------------------------------------

std::string isoIdentifierFor(const std::string& displayName, bool isDir) {
    std::string up;
    up.reserve(displayName.size());
    for (char c : displayName) {
        if (c >= 'a' && c <= 'z') c = char(c - 32);
        // keep the separator '.' — handled by the base/ext split below
        up += (isDChar(c) || c == '.') ? c : '_';
    }
    // keep only the first '.'
    const size_t dot = up.find('.');
    if (dot != std::string::npos) {
        for (size_t i = dot + 1; i < up.size();) {
            if (up[i] == '.') up.erase(i, 1); else ++i;
        }
    }
    std::string base = up, ext;
    if (dot != std::string::npos) {
        base = up.substr(0, dot);
        ext  = up.substr(dot + 1);
    }
    if (base.size() > 8) base.resize(8);
    if (ext.size() > 3)  ext.resize(3);
    std::string id = base;
    if (!ext.empty()) id += "." + ext;
    if (id.empty()) id = "_";
    if (!isDir) id += ";1";
    return id;
}

// ---------------------------------------------------------------------------
// IsoNode
// ---------------------------------------------------------------------------

uint64_t IsoNode::size() const {
    if (!extPath.empty()) return extSize;
    uint64_t s = 0;
    for (const auto& r : runs) s += r.srcSize;
    return s;
}

// ---------------------------------------------------------------------------
// open
// ---------------------------------------------------------------------------

bool IsoImageDoc::open(const std::string& path) {
    close();

    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;

    uint8_t pvd[kSector];
    if (!seek64(f, 16ll * kSector) ||
        std::fread(pvd, 1, kSector, f) != kSector) {
        std::fclose(f);
        return false;
    }
    if (std::memcmp(pvd + 1, "CD001", 5) != 0) {
        std::fclose(f);
        return false;
    }

    volumeId_.assign(reinterpret_cast<const char*>(pvd + 40), 32);
    while (!volumeId_.empty() &&
           (volumeId_.back() == ' ' || volumeId_.back() == '\0'))
        volumeId_.pop_back();

    if (seek64(f, 0, SEEK_END)) {
        const int64_t sz = tell64(f);
        if (sz > 0) origSize_ = uint64_t(sz);
    }

    const uint8_t* rootRec = pvd + 156;
    const uint32_t rootLba  = rootRec[2] | (rootRec[3] << 8) |
                              (rootRec[4] << 16) | (uint32_t(rootRec[5]) << 24);
    const uint32_t rootSize = rootRec[10] | (rootRec[11] << 8) |
                              (rootRec[12] << 16) | (uint32_t(rootRec[13]) << 24);

    root_ = std::make_unique<IsoNode>();
    root_->name = "<root>";
    root_->isoName.assign(1, '\x01');                 // root marker
    root_->isDir = true;
    root_->fromSource = true;
    srcPath_ = path;

    struct StackItem { uint32_t lba; uint64_t size; IsoNode* parent; };
    std::vector<StackItem> stack;
    std::vector<uint32_t> visited;
    stack.push_back({rootLba, rootSize, root_.get()});

    const IsoReadFn read = [f](uint64_t off, void* dst, size_t n) {
        return seek64(f, int64_t(off)) && std::fread(dst, 1, n, f) == n;
    };

    while (!stack.empty()) {
        const StackItem cur = stack.back();
        stack.pop_back();

        if (visited.size() >= 4096) break;             // hostile image guard
        visited.push_back(cur.lba);

        uint64_t dirLen = cur.size ? cur.size : kSector;
        if (dirLen > 8ull * 1024 * 1024) dirLen = 8ull * 1024 * 1024;
        std::vector<uint8_t> dir(static_cast<size_t>(dirLen), 0);
        if (!read(uint64_t(cur.lba) * kSector, dir.data(), dir.size()))
            continue;

        // file-section flags of the record parsed just before the current
        // one in THIS directory extent (0 when none / it was a directory) —
        // per ECMA-119 a multi-extent file's LAST record clears the 0x80
        // bit, so "the previous record had 0x80" is what marks the current
        // record as a continuation, not the current record's own bit.
        uint8_t prevRecordFlags = 0;
        for (size_t off = 0; off + 1 < dir.size();) {
            const uint8_t recLen = dir[off];
            if (recLen == 0) {
                off = (off / kSector + 1) * kSector;
                continue;
            }
            if (off + recLen > dir.size()) break;
            const size_t recOff = off;
            off += recLen;

            // Sanity: header + identifier must fit inside the record, or a
            // malformed image makes the identifier read below run past the
            // buffer.
            if (recLen < 34) continue;
            const uint8_t nameLen = dir[recOff + 32];
            if (size_t(nameLen) + 33 > recLen) continue;
            if (nameLen == 1 && dir[recOff + 33] < 2) continue;   // "." / ".."

            std::string ident(reinterpret_cast<const char*>(&dir[recOff + 33]),
                              nameLen);
            if (!ident.empty() && ident.back() == '\0') ident.pop_back();

            const uint32_t lba = dir[recOff + 2] | (dir[recOff + 3] << 8) |
                                 (dir[recOff + 4] << 16) |
                                 (uint32_t(dir[recOff + 5]) << 24);
            const uint32_t esz = dir[recOff + 10] | (dir[recOff + 11] << 8) |
                                 (dir[recOff + 12] << 16) |
                                 (uint32_t(dir[recOff + 13]) << 24);
            const uint8_t flags = dir[recOff + 25];
            const bool isDir = (flags & 0x02) != 0;

            std::string disp = ident;
            const size_t semi = disp.find(';');
            if (semi != std::string::npos) disp.resize(semi);

            if (isDir) {
                auto node = std::make_unique<IsoNode>();
                node->name = disp;
                node->isoName = ident;
                node->isDir = true;
                node->fromSource = true;
                IsoNode* raw = node.get();
                cur.parent->children.push_back(std::move(node));
                prevRecordFlags = 0;

                bool seen = false;
                for (uint32_t v : visited) if (v == lba) { seen = true; break; }
                if (!seen && lba != cur.lba)
                    stack.push_back({lba, esz, raw});
            } else {
                // merge multi-extent continuation records: same identifier
                // as the last file entry of this directory AND that previous
                // record carried the 0x80 bit. This also tolerates images
                // (our own pre-0.0.7 writer) that set 0x80 on every record
                // of the file, including the last.
                IsoNode* prev = nullptr;
                if (!cur.parent->children.empty()) {
                    IsoNode* cand = cur.parent->children.back().get();
                    if (!cand->isDir && cand->isoName == ident && !cand->isExternal())
                        prev = cand;
                }
                const bool continuesPrev = prev && (prevRecordFlags & 0x80);
                if (continuesPrev) {
                    prev->runs.push_back({srcPath_,
                                          uint64_t(lba) * kSector, esz});
                } else {
                    auto node = std::make_unique<IsoNode>();
                    node->name = disp;
                    node->isoName = ident;
                    node->isDir = false;
                    node->fromSource = true;
                    node->runs.push_back({srcPath_,
                                          uint64_t(lba) * kSector, esz});
                    cur.parent->children.push_back(std::move(node));
                }
                prevRecordFlags = flags;
            }
        }
    }

    std::fclose(f);
    dirty_ = false;
    return true;
}

void IsoImageDoc::close() {
    root_.reset();
    srcPath_.clear();
    volumeId_.clear();
    origSize_ = 0;
    dirty_ = false;
}

// ---------------------------------------------------------------------------
// modifications
// ---------------------------------------------------------------------------

IsoNode* IsoImageDoc::addDir(IsoNode* parent, const std::string& name) {
    if (!parent || !parent->isDir) return nullptr;
    auto node = std::make_unique<IsoNode>();
    node->name = name.empty() ? "NEW_DIR" : name;
    node->isoName = isoIdentifierFor(node->name, true);
    node->isDir = true;
    node->fromSource = false;
    IsoNode* raw = node.get();
    parent->children.push_back(std::move(node));
    dirty_ = true;
    return raw;
}

IsoNode* IsoImageDoc::addFile(IsoNode* parent, const std::string& externalPath,
                              const std::string& nameOverride) {
    if (!parent || !parent->isDir) return nullptr;
    const uint64_t sz = fileSizeOf(externalPath);
    if (sz == ~0ull) return nullptr;

    std::string disp = nameOverride;
    if (disp.empty()) {
        const size_t cut = externalPath.find_last_of("/\\");
        disp = (cut == std::string::npos) ? externalPath
                                          : externalPath.substr(cut + 1);
    }

    auto node = std::make_unique<IsoNode>();
    node->name = disp;
    node->isoName = isoIdentifierFor(disp, false);
    node->isDir = false;
    node->fromSource = false;
    node->extPath = externalPath;
    node->extSize = sz;
    IsoNode* raw = node.get();
    parent->children.push_back(std::move(node));
    dirty_ = true;
    return raw;
}

bool IsoImageDoc::replaceFile(IsoNode* node, const std::string& externalPath) {
    if (!node || node->isDir) return false;
    const uint64_t sz = fileSizeOf(externalPath);
    if (sz == ~0ull) return false;
    node->extPath = externalPath;
    node->extSize = sz;
    node->fromSource = false;
    dirty_ = true;
    return true;
}

bool IsoImageDoc::removeChild(IsoNode* parent, IsoNode* child) {
    if (!parent || !child || !parent->isDir) return false;
    if (child->isDir && !child->children.empty()) return false;
    for (auto it = parent->children.begin(); it != parent->children.end(); ++it) {
        if (it->get() == child) {
            parent->children.erase(it);
            dirty_ = true;
            return true;
        }
    }
    return false;
}

bool IsoImageDoc::renameNode(IsoNode* node, const std::string& newName) {
    if (!node || node == root_.get() || newName.empty()) return false;
    node->name = newName;
    node->isoName = isoIdentifierFor(newName, node->isDir);
    dirty_ = true;
    return true;
}

bool IsoImageDoc::resetNode(IsoNode* node) {
    if (!node || node == root_.get() || !node->fromSource) return false;
    node->extPath.clear();
    node->extSize = 0;
    return true;
}

// ---------------------------------------------------------------------------
// extract
// ---------------------------------------------------------------------------

bool IsoImageDoc::extractTo(const IsoNode* node, const std::string& outPath) {
    if (!node || node->isDir) return false;

    FILE* out = std::fopen(outPath.c_str(), "wb");
    if (!out) return false;
    bool ok = true;

    if (node->isExternal()) {
        FILE* in = std::fopen(node->extPath.c_str(), "rb");
        if (!in) { std::fclose(out); return false; }
        uint8_t buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0)
            if (std::fwrite(buf, 1, n, out) != n) { ok = false; break; }
        if (std::ferror(in)) ok = false;
        std::fclose(in);
    } else {
        for (const IsoContentRun& run : node->runs) {
            FILE* in = std::fopen(run.srcPath.c_str(), "rb");
            if (!in || !seek64(in, int64_t(run.srcOffset))) {
                if (in) std::fclose(in);
                ok = false;
                break;
            }
            uint64_t left = run.srcSize;
            uint8_t buf[65536];
            while (left) {
                const size_t take = size_t(std::min<uint64_t>(left, sizeof(buf)));
                if (std::fread(buf, 1, take, in) != take) { ok = false; break; }
                if (std::fwrite(buf, 1, take, out) != take) { ok = false; break; }
                left -= take;
            }
            std::fclose(in);
            if (!ok) break;
        }
    }
    std::fclose(out);
    return ok;
}

void IsoImageDoc::forEachNode(
        const std::function<void(const IsoNode*, int)>& fn) const {
    if (!root_) return;
    std::vector<std::pair<const IsoNode*, int>> stack{{root_.get(), 0}};
    while (!stack.empty()) {
        auto [node, depth] = stack.back();
        stack.pop_back();
        fn(node, depth);
        for (auto it = node->children.rbegin(); it != node->children.rend(); ++it)
            stack.push_back({it->get(), depth + 1});
    }
}

// ---------------------------------------------------------------------------
// saveAs — minimal ISO9660 Level-1 writer
// ---------------------------------------------------------------------------

namespace {

/// Stream `bytes` of a node's logical content (skipping `skip` bytes) to
/// `out`. Handles external files and multi-run source extents.
bool writeNodeContent(FILE* out, const IsoNode* node, uint64_t skip,
                      uint64_t bytes) {
    if (node->isExternal()) {
        FILE* in = std::fopen(node->extPath.c_str(), "rb");
        if (!in || (skip && !seek64(in, int64_t(skip)))) {
            if (in) std::fclose(in);
            return false;
        }
        uint8_t buf[65536];
        while (bytes) {
            const size_t take = size_t(std::min<uint64_t>(bytes, sizeof(buf)));
            if (std::fread(buf, 1, take, in) != take) { std::fclose(in); return false; }
            if (std::fwrite(buf, 1, take, out) != take) { std::fclose(in); return false; }
            bytes -= take;
        }
        std::fclose(in);
        return true;
    }

    // walk runs, skipping
    for (const IsoContentRun& run : node->runs) {
        if (skip >= run.srcSize) { skip -= run.srcSize; continue; }
        FILE* in = std::fopen(run.srcPath.c_str(), "rb");
        if (!in || !seek64(in, int64_t(run.srcOffset + skip))) {
            if (in) std::fclose(in);
            return false;
        }
        uint64_t left = std::min(bytes, run.srcSize - skip);
        uint8_t buf[65536];
        while (left) {
            const size_t take = size_t(std::min<uint64_t>(left, sizeof(buf)));
            if (std::fread(buf, 1, take, in) != take) { std::fclose(in); return false; }
            if (std::fwrite(buf, 1, take, out) != take) { std::fclose(in); return false; }
            left -= take;
            bytes -= take;
        }
        std::fclose(in);
        skip = 0;
        if (bytes == 0) return true;
    }
    return bytes == 0;
}

} // namespace

bool IsoImageDoc::saveAs(const std::string& outPath,
                          const ProgressFn& progress,
                          std::string* error) {
    if (!root_) { if (error) *error = "no image is open"; return false; }
    if (error) error->clear();

    // ---- 1. plan: mirror the tree, BFS order for directories -------------
    std::vector<WriteDir*> dirs;
    auto wroot = std::make_unique<WriteDir>();
    wroot->node = root_.get();
    dirs.push_back(wroot.get());

    for (size_t di = 0; di < dirs.size(); ++di) {
        WriteDir* wd = dirs[di];
        for (const auto& k : wd->node->children) {
            if (!k->isDir) continue;
            auto wc = std::make_unique<WriteDir>();
            wc->node = k.get();
            wc->parent = wd;
            WriteDir* raw = wc.get();
            wd->children.push_back(std::move(wc));
            dirs.push_back(raw);
        }
    }

    // ---- 2. directory extent sizes ----------------------------------------
    for (WriteDir* wd : dirs) {
        uint64_t bytes = recordSizeFor(1) * 2;          // "." and ".."
        for (const auto& k : wd->node->children) {
            const uint64_t nExt = k->isDir ? 1 : extentCountFor(k->size());
            bytes += recordSizeFor(k->isoName.size()) * nExt;
        }
        wd->dirBytes = bytes;
    }

    // ---- 3. file extents in deterministic order ---------------------------
    // (children order inside each directory, directories in BFS order)
    std::vector<WriteExtent> extents;
    for (WriteDir* wd : dirs) {
        for (const auto& k : wd->node->children) {
            if (k->isDir) continue;
            const uint64_t logical = k->size();
            if (logical == 0) {
                WriteExtent fe;
                fe.node = k.get();
                fe.bytes = 0;
                fe.skip = 0;
                extents.push_back(fe);
                continue;
            }
            uint64_t skip = 0;
            while (skip < logical) {
                WriteExtent fe;
                fe.node = k.get();
                fe.bytes = std::min(kMaxOneExtent, logical - skip);
                fe.skip = skip;
                extents.push_back(fe);
                skip += fe.bytes;
            }
        }
    }

    // ---- 4. LBA assignment --------------------------------------------------
    uint64_t pathTableBytes = 0;
    for (size_t i = 0; i < dirs.size(); ++i) {
        const size_t nameLen = (i == 0) ? 1 : dirs[i]->node->isoName.size();
        pathTableBytes += 8 + nameLen + (nameLen % 2);
    }
    const uint32_t pathSectors =
        uint32_t((pathTableBytes + kSector - 1) / kSector);
    uint32_t nextLba = 18 + pathSectors * 2;           // L + M tables

    for (size_t i = 0; i < dirs.size(); ++i) {
        dirs[i]->dirLba = nextLba;
        dirs[i]->pathIndex = uint16_t(i + 1);
        nextLba += uint32_t((dirs[i]->dirBytes + kSector - 1) / kSector);
    }
    for (WriteExtent& fe : extents) {
        fe.lba = nextLba;
        nextLba += uint32_t((fe.bytes + kSector - 1) / kSector);
    }
    const uint32_t volumeSectors = nextLba;

    // ---- 5. write -------------------------------------------------------------
    FILE* out = std::fopen(outPath.c_str(), "wb");
    if (!out) { if (error) *error = "cannot create the output file"; return false; }

    bool ok = true;
    uint64_t done = 0;
    const uint64_t total = uint64_t(volumeSectors) * kSector;
    const char kZeros[4096] = {};
    auto writeZeros = [&](uint64_t n) -> bool {
        while (n) {
            const size_t take = size_t(std::min<uint64_t>(n, sizeof(kZeros)));
            if (std::fwrite(kZeros, 1, take, out) != take) return false;
            n -= take;
        }
        return true;
    };
    auto report = [&]() {
        if (progress) progress(done, total);
    };

    // system area
    if (!writeZeros(16ull * kSector)) ok = false;
    done += 16ull * kSector;
    report();

    // ---- PVD ---------------------------------------------------------------
    if (ok) {
        uint8_t pvd[kSector];
        std::memset(pvd, 0, sizeof(pvd));
        pvd[0] = 1;
        std::memcpy(pvd + 1, "CD001", 5);
        pvd[6] = 1;
        std::memcpy(pvd + 8, "PS2", 3);
        {
            std::string up;
            for (char c : volumeId_)
                up += (c >= 'a' && c <= 'z') ? char(c - 32) : c;
            if (up.empty()) up = "USBUTILS";
            if (up.size() > 32) up.resize(32);
            std::memcpy(pvd + 40, up.c_str(), up.size());
        }
        const auto put32both = [&](size_t off, uint32_t v) {
            std::memcpy(pvd + off, &v, 4);
            const uint32_t be = bswap32(v);
            std::memcpy(pvd + off + 4, &be, 4);
        };
        put32both(80, volumeSectors);
        std::memcpy(pvd + 88, "\x01\x00\x01\x00", 4);      // set size / seq
        uint16_t bs = uint16_t(kSector);
        std::memcpy(pvd + 92, &bs, 2);
        std::memcpy(pvd + 94, &bs, 2);
        put32both(96, uint32_t(pathTableBytes));            // path table size
        const uint32_t lLba = 18, mLba = 18 + pathSectors;
        std::memcpy(pvd + 104, &lLba, 4);                   // L path table
        const uint32_t zero = 0;
        std::memcpy(pvd + 108, &zero, 4);                   // optional L
        std::memcpy(pvd + 112, &mLba, 4);                   // M path table
        std::memcpy(pvd + 116, &zero, 4);                   // optional M
        // root directory record
        {
            DirRecordHeader h;
            std::memset(&h, 0, sizeof(h));
            h.lbaLE = dirs.front()->dirLba;
            h.lbaBE = bswap32(dirs.front()->dirLba);
            h.sizeLE = uint32_t(dirs.front()->dirBytes);
            h.sizeBE = bswap32(uint32_t(dirs.front()->dirBytes));
            putDate7(h.date);
            h.flags = 0x02;
            h.volSeqLE = 1; h.volSeqBE = bswap16(1);
            h.nameLen = 1;
            h.len = uint8_t(34);
            std::memcpy(pvd + 156, &h, 33);
            pvd[156 + 33] = 0;
        }
        std::memcpy(pvd + 318, "USBUTILS", 8);              // volume set id
        std::memcpy(pvd + 572, "USBUTILS", 8);              // application id
        std::memcpy(pvd + 813, "2000010100000000", 16);
        std::memcpy(pvd + 830, "2000010100000000", 16);
        pvd[881] = 1;                                       // structure version
        if (std::fwrite(pvd, 1, sizeof(pvd), out) != sizeof(pvd)) ok = false;
        done += kSector; report();
    }

    // ---- terminator ---------------------------------------------------------
    if (ok) {
        uint8_t term[kSector];
        std::memset(term, 0, sizeof(term));
        term[0] = 255;
        std::memcpy(term + 1, "CD001", 5);
        term[6] = 1;
        if (std::fwrite(term, 1, sizeof(term), out) != sizeof(term)) ok = false;
        done += kSector; report();
    }

    // ---- path tables (L then M) ---------------------------------------------
    if (ok) {
        std::vector<uint8_t> pt;
        pt.reserve(size_t(pathTableBytes));
        for (size_t i = 0; i < dirs.size(); ++i) {
            WriteDir* wd = dirs[i];
            const std::string nm = (i == 0)
                ? std::string("\x01", 1) : wd->node->isoName;
            const uint16_t parentIdx = wd->parent ? wd->parent->pathIndex : 1;
            pt.push_back(uint8_t(nm.size()));
            pt.push_back(0);
            const size_t lbaOff = pt.size();
            pt.resize(lbaOff + 4);
            std::memcpy(&pt[lbaOff], &wd->dirLba, 4);
            pt.push_back(uint8_t(parentIdx & 0xff));
            pt.push_back(uint8_t(parentIdx >> 8));
            pt.insert(pt.end(), nm.begin(), nm.end());
            if (nm.size() % 2) pt.push_back(0);
        }
        const size_t ptPadded = size_t(pathSectors) * kSector;
        std::vector<uint8_t> padded = pt;
        padded.resize(ptPadded, 0);
        if (std::fwrite(padded.data(), 1, padded.size(), out) != padded.size())
            ok = false;

        // M variant (big-endian lba + parent)
        std::vector<uint8_t> ptM;
        ptM.reserve(pt.size());
        for (size_t o = 0; o + 8 <= pt.size();) {
            const size_t nameLen = pt[o];
            ptM.push_back(pt[o]);
            ptM.push_back(pt[o + 1]);
            const uint32_t lba = pt[o + 2] | (pt[o + 3] << 8) |
                                 (pt[o + 4] << 16) | (uint32_t(pt[o + 5]) << 24);
            const uint32_t be = bswap32(lba);
            ptM.insert(ptM.end(), reinterpret_cast<const uint8_t*>(&be),
                       reinterpret_cast<const uint8_t*>(&be) + 4);
            const uint16_t parent = uint16_t(pt[o + 6] | (pt[o + 7] << 8));
            const uint16_t pbe = bswap16(parent);
            ptM.insert(ptM.end(), reinterpret_cast<const uint8_t*>(&pbe),
                       reinterpret_cast<const uint8_t*>(&pbe) + 2);
            ptM.insert(ptM.end(), pt.begin() + o + 8,
                       pt.begin() + o + 8 + nameLen);
            if (nameLen % 2) ptM.push_back(0);
            o += 8 + nameLen + (nameLen % 2);
        }
        ptM.resize(ptPadded, 0);
        if (std::fwrite(ptM.data(), 1, ptM.size(), out) != ptM.size())
            ok = false;
        done += 2ull * ptPadded; report();
    }

    // ---- directory extents ---------------------------------------------------
    if (ok) {
        // map file node -> list of extents (per directory, in order)
        size_t extCursor = 0;
        for (WriteDir* wd : dirs) {
            std::vector<const WriteExtent*> mine;
            for (const auto& k : wd->node->children) {
                if (k->isDir) continue;
                const uint64_t nExt = extentCountFor(k->size());
                for (uint64_t e = 0; e < nExt && extCursor < extents.size(); ++e)
                    mine.push_back(&extents[extCursor++]);
            }

            std::vector<uint8_t> body;
            body.reserve(size_t(wd->dirBytes));
            putRecord(body, "\x00", 1, wd->dirLba,
                      uint32_t(wd->dirBytes), 0x02);            // "."
            const uint32_t parentLba = wd->parent ? wd->parent->dirLba : wd->dirLba;
            const uint32_t parentSize = wd->parent
                ? uint32_t(wd->parent->dirBytes) : uint32_t(wd->dirBytes);
            putRecord(body, "\x01", 1, parentLba, parentSize, 0x02);  // ".."

            size_t mineIdx = 0;
            for (const auto& k : wd->node->children) {
                const std::string& nm = k->isoName;
                if (k->isDir) {
                    WriteDir* wc = nullptr;
                    for (const auto& c : wd->children)
                        if (c->node == k.get()) { wc = c.get(); break; }
                    if (!wc) { ok = false; break; }
                    putRecord(body, nm.c_str(), nm.size(), wc->dirLba,
                              uint32_t(wc->dirBytes), 0x02);
                } else {
                    const uint64_t logical = k->size();
                    const uint64_t nExt = extentCountFor(logical);
                    for (uint64_t e = 0; e < nExt; ++e) {
                        const WriteExtent* fe = (mineIdx < mine.size())
                            ? mine[mineIdx] : nullptr;
                        ++mineIdx;
                        if (!fe) { ok = false; break; }
                        // ECMA-119 multi-extent: every section's record sets
                        // 0x80 EXCEPT the last one of the file
                        const uint8_t flags =
                            uint8_t((e + 1 < nExt) ? 0x80 : 0x00);
                        putRecord(body, nm.c_str(), nm.size(), fe->lba,
                                  uint32_t(fe->bytes), flags);
                    }
                    if (!ok) break;
                }
            }
            if (!ok) break;

            const size_t sectorBytes =
                size_t((wd->dirBytes + kSector - 1) / kSector) * kSector;
            body.resize(sectorBytes, 0);
            if (std::fwrite(body.data(), 1, body.size(), out) != body.size()) {
                ok = false;
                break;
            }
            done += sectorBytes; report();
        }
    }

    // ---- file extents ---------------------------------------------------------
    if (ok) {
        for (const WriteExtent& fe : extents) {
            if (fe.bytes == 0) continue;               // empty file: no data
            if (!writeNodeContent(out, fe.node, fe.skip, fe.bytes)) {
                ok = false;
                if (error) *error = "failed reading content of '" +
                                    fe.node->name + "'";
                break;
            }
            // pad to sector boundary
            const uint64_t pad =
                (kSector - (fe.bytes % kSector)) % kSector;
            if (pad && !writeZeros(pad)) { ok = false; break; }
            done += fe.bytes + pad;
            report();
        }
    }

    std::fclose(out);

    if (ok && error) error->clear();
    if (!ok && error && error->empty()) *error = "write failed";
    if (!ok) std::remove(outPath.c_str());
    return ok;
}

} // namespace usbutils
