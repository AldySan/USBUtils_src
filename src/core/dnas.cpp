// SPDX-License-Identifier: MIT
#include "dnas.h"
#include "gamefile.h"
#include "gamestream.h"
#include "iso9660.h"
#include "port.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>

namespace usbutils {

static std::string toLower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        out += static_cast<char>((c >= 'A' && c <= 'Z') ? c + 32 : c);
    return out;
}

bool DnasPackage::loadIndex(const std::string& dniPath) {
    entries_.clear();
    FILE* f = std::fopen(dniPath.c_str(), "rb");
    if (!f) return false;

    std::string line;
    int c;
    uint64_t offset = 0;
    while ((c = std::fgetc(f)) != EOF) {
        if (c == '\n') {
            // "DNAS<ver>[<size>];" — parsed like the original's dni reader
            if (!line.empty() && line[0] != '/') {
                const size_t b = line.find('[');
                const size_t e = line.find(']');
                if (b != std::string::npos && e != std::string::npos && e > b &&
                    line.compare(0, 4, "DNAS") == 0) {
                    DnasEntry ent;
                    ent.version = line.substr(4, b - 4);
                    ent.size = static_cast<uint32_t>(
                        std::strtoul(line.substr(b + 1, e - b - 1).c_str(),
                                     nullptr, 10));
                    ent.offset = offset;
                    offset += ent.size;
                    entries_.push_back(std::move(ent));
                }
            }
            line.clear();
        } else if (c != '\r') {
            line += static_cast<char>(c);
        }
    }
    std::fclose(f);
    return !entries_.empty();
}

void DnasPackage::setDataPath(const std::string& dnaPath) {
    dnaPath_ = dnaPath;
    FILE* f = std::fopen(dnaPath.c_str(), "rb");
    if (f) {
        if (usbutils::seek64(f, 0, SEEK_END))
            blobSize_ = static_cast<uint64_t>(usbutils::tell64(f));
        std::fclose(f);
    }
}

std::string DnasPackage::bestFor(const std::string& consoleVer) const {
    // Prefer exact match, then the highest entry <= consoleVer.
    std::string best;
    long target = std::strtol(consoleVer.c_str(), nullptr, 10);
    long bestVal = -1;
    for (const auto& e : entries_) {
        if (e.version == consoleVer) return e.version;
        const long v = std::strtol(e.version.c_str(), nullptr, 10);
        if (v <= target && v > bestVal) { bestVal = v; best = e.version; }
    }
    return best;
}

bool DnasPackage::findImage(const std::string& version,
                             uint64_t& off, uint32_t& size) const {
    for (const auto& e : entries_) {
        if (e.version == version) {
            off = e.offset;
            size = e.size;
            return off + size <= blobSize_ || blobSize_ == 0;
        }
    }
    return false;
}

bool DnasPackage::extract(const std::string& version,
                          const std::string& outPath) const {
    uint64_t off = 0;
    uint32_t size = 0;
    if (!findImage(version, off, size)) return false;

    FILE* in = std::fopen(dnaPath_.c_str(), "rb");
    if (!in) return false;
    if (!usbutils::seek64(in, static_cast<int64_t>(off))) {
        std::fclose(in);
        return false;
    }
    FILE* out = std::fopen(outPath.c_str(), "wb");
    if (!out) { std::fclose(in); return false; }

    std::vector<uint8_t> buf(1 << 16);
    uint32_t left = size;
    bool ok = true;
    while (left > 0) {
        const size_t n = std::fread(buf.data(), 1,
                                    std::min<size_t>(buf.size(), left), in);
        if (n == 0 || std::fwrite(buf.data(), 1, n, out) != n) { ok = false; break; }
        left -= static_cast<uint32_t>(n);
    }
    std::fclose(in);
    std::fclose(out);
    if (!ok) std::remove(outPath.c_str());
    return ok;
}

// ---------------------------------------------------------------------------
// IOPRP location
// ---------------------------------------------------------------------------

/// Fallback: scan the stream for the "RESET\0\0\0" IOPRP magic. Only used
/// when the image has no IOPRP*.IMG directory entry (non-standard rips).
static bool scanForIoprpMagic(GameStream& stream, uint64_t& offset,
                              uint32_t& length) {
    static const uint8_t magic[8] = {'R','E','S','E','T',0,0,0};

    std::vector<uint8_t> buf(4 * 1024 * 1024);
    std::vector<uint8_t> carry;
    uint64_t carryStart = 0;                     // absolute offset of carry[0]
    uint64_t pos = 0;
    size_t n;
    while ((n = stream.read(pos, buf.data(), buf.size())) > 0) {
        std::vector<uint8_t> hay = carry;
        hay.insert(hay.end(), buf.begin(), buf.begin() + n);
        const size_t found = std::search(hay.begin(), hay.end(),
                                         magic, magic + 8) - hay.begin();
        if (found < hay.size()) {
            offset = carryStart + found;
            length = 512 * 1024;                 // upper bound, see caller
            if (offset + length > stream.size())
                length = static_cast<uint32_t>(stream.size() - offset);
            return true;
        }
        if (hay.size() >= 7) {
            carry.assign(hay.end() - 7, hay.end());
            carryStart += hay.size() - 7;
        } else {
            carry = hay;                          // short tail at EOF — no UB
        }
        pos += n;
    }
    return false;
}

bool locateIoprp(const std::string& dir, const std::string& idHex,
                 const std::string& serial, uint32_t parts,
                 uint64_t& offset, uint32_t& length) {
    GameStream stream;
    if (!stream.open(dir, idHex, serial, parts) || stream.size() == 0)
        return false;

    // Preferred: exact IOPRP*.IMG directory entry — gives the precise
    // extent, so patching never writes beyond the real image.
    bool found = false;
    uint64_t fOff = 0;
    uint64_t fLen = 0;
    const IsoReadFn read = [&stream](uint64_t off, void* dst, size_t n) {
        return stream.read(off, dst, n) == n;
    };
    walkIso(read, [&](const IsoEntry& e) {
        if (found || e.isDir) return true;
        const std::string low = toLower(e.name);
        if (low.compare(0, 5, "ioprp") == 0 &&
            low.size() > 9 && low.compare(low.size() - 4, 4, ".img") == 0) {
            fOff = static_cast<uint64_t>(e.lba) * 2048ull;
            fLen = e.size;
            found = true;
            return false;
        }
        return true;
    });
    if (found && fOff + fLen <= stream.size() && fLen > 0) {
        offset = fOff;
        length = static_cast<uint32_t>(fLen);
        return true;
    }

    // Fallback: magic scan (bounded by the stream end).
    return scanForIoprpMagic(stream, offset, length);
}

// ---------------------------------------------------------------------------
// patching
// ---------------------------------------------------------------------------

int DnasPackage::patchGame(const std::string& dir, const std::string& idHex,
                           const std::string& serial, uint32_t parts,
                           const std::string& version) const {
    uint64_t repOff = 0;
    uint32_t repSize = 0;
    if (!findImage(version, repOff, repSize)) return -1;

    uint64_t offset = 0;
    uint32_t length = 0;
    if (!locateIoprp(dir, idHex, serial, parts, offset, length))
        return -1;
    if (repSize > length) return -1;   // must fit the original slot

    // Read replacement image
    std::vector<uint8_t> img(repSize);
    FILE* dna = std::fopen(dnaPath_.c_str(), "rb");
    if (!dna) return -1;
    bool ok = usbutils::seek64(dna, static_cast<int64_t>(repOff))
           && std::fread(img.data(), 1, repSize, dna) == repSize;
    std::fclose(dna);
    if (!ok) return -1;

    // Overwrite in place through the chunk stream; pad the remainder of
    // the exact slot with zeros.
    GameStream stream;
    if (!stream.open(dir, idHex, serial, parts)) return -1;
    ok = stream.write(offset, img.data(), repSize);
    if (ok) {
        const uint32_t pad = length - repSize;
        if (pad) {
            std::vector<uint8_t> zeros(pad, 0);
            ok = stream.write(offset + repSize, zeros.data(), pad);
        }
    }
    return ok ? 1 : -1;
}

} // namespace usbutils
