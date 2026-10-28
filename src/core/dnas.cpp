// SPDX-License-Identifier: MIT
#include "dnas.h"
#include "gamefile.h"

#include <cstdio>
#include <cstring>
#include "port.h"
#include <cstdlib>
#include <algorithm>
#include "port.h"

namespace usbutils {

static std::string joinPath(const std::string& dir, const std::string& file) {
    if (dir.empty()) return file;
    if (dir.back() == '/' || dir.back() == '\\') return dir + file;
    return dir + "/" + file;
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
        if (std::fseek(f, 0, SEEK_END) == 0)
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
    if (std::fseek(in, static_cast<long>(off), SEEK_SET) != 0) {
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

/// Scan the game stream for the "RESET\0\0\0" IOPRP magic.
bool locateEmbeddedIoprp(const std::string& dir, const std::string& idHex,
                         const std::string& serial, uint32_t parts,
                         uint32_t& chunkIdx, uint64_t& offset, uint32_t& length) {
    static const uint8_t magic[8] = {'R','E','S','E','T',0,0,0};

    std::vector<uint8_t> buf(4 * 1024 * 1024);
    uint64_t carrySize = 0;
    std::vector<uint8_t> carry;

    for (uint32_t i = 0; i < parts; ++i) {
        FILE* f = std::fopen(joinPath(dir, chunkFileName(idHex, serial, i)).c_str(), "rb");
        if (!f) continue;
        size_t n;
        while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) {
            // search with carry-over from previous block
            std::vector<uint8_t> hay = carry;
            hay.insert(hay.end(), buf.begin(), buf.begin() + n);
            const size_t pos = std::search(hay.begin(), hay.end(),
                                           magic, magic + 8) - hay.begin();
            if (pos < hay.size()) {
                // found; image length = next "RESET" or EOF (bounded by 512 KiB)
                uint64_t absOff = carrySize + pos;
                std::fclose(f);
                chunkIdx = i;
                offset = absOff;
                // typical IOPRP sizes: 92 KiB .. 272 KiB — cap at 512 KiB
                length = 512 * 1024;
                return true;
            }
            carry.assign(hay.end() - 7, hay.end());   // keep last 7 bytes
            carrySize += hay.size() - 7;
        }
        std::fclose(f);
    }
    return false;
}

int DnasPackage::patchGame(const std::string& dir, const std::string& idHex,
                           const std::string& serial, uint32_t parts,
                           const std::string& version) const {
    uint64_t repOff = 0;
    uint32_t repSize = 0;
    if (!findImage(version, repOff, repSize)) return -1;

    uint32_t chunkIdx = 0;
    uint64_t offset = 0;
    uint32_t length = 0;
    if (!locateEmbeddedIoprp(dir, idHex, serial, parts, chunkIdx, offset, length))
        return -1;
    if (repSize > length) return -1;   // must fit the original slot

    // Read replacement image
    std::vector<uint8_t> img(repSize);
    FILE* dna = std::fopen(dnaPath_.c_str(), "rb");
    if (!dna) return -1;
    bool ok = std::fseek(dna, static_cast<long>(repOff), SEEK_SET) == 0
           && std::fread(img.data(), 1, repSize, dna) == repSize;
    std::fclose(dna);
    if (!ok) return -1;

    // Overwrite in place; pad the remainder of the slot with zeros.
    const std::string path = joinPath(dir, chunkFileName(idHex, serial, chunkIdx));
    FILE* f = std::fopen(path.c_str(), "r+b");
    if (!f) return -1;
    ok = std::fseek(f, static_cast<long>(offset), SEEK_SET) == 0;
    if (ok) ok = std::fwrite(img.data(), 1, repSize, f) == repSize;
    if (ok) {
        const uint32_t pad = length - repSize;
        std::vector<uint8_t> zeros(pad, 0);
        if (pad) ok = std::fwrite(zeros.data(), 1, pad, f) == pad;
    }
    std::fclose(f);
    return ok ? 1 : -1;
}

} // namespace usbutils
