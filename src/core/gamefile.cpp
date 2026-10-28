// SPDX-License-Identifier: MIT
#include "gamefile.h"

#include <cstdio>
#include <cstring>
#include "port.h"
#include <algorithm>
#include "port.h"
#include <cstdlib>

#if defined(_WIN32)
  #include <windows.h>
  struct FindHandle { HANDLE h; };
#else
  #include <dirent.h>
#endif

namespace usbutils {

static std::string joinPath(const std::string& dir, const std::string& file) {
    if (dir.empty()) return file;
    if (dir.back() == '/' || dir.back() == '\\') return dir + file;
    return dir + "/" + file;
}

uint32_t chunkCount(uint64_t totalSize) {
    // div + mod pair at 0x3A11C2/0x3A11E7: parts = size/1GiB (+1 if remainder)
    if (totalSize == 0) return 1;
    uint64_t parts = totalSize / kChunkSize;
    if (totalSize % kChunkSize) ++parts;
    return static_cast<uint32_t>(parts);
}

uint8_t mediaForSize(uint64_t totalSize) {
    return totalSize <= kCdThreshold ? kMediaCd : kMediaDvd;
}

std::string chunkFileName(const std::string& idHex, const std::string& serial,
                          uint32_t index) {
    char nn[4];
    // Original writes ".0" + Chr('0'+i); we zero-pad two digits, compatible
    // for i<=9 and correct beyond it.
    std::snprintf(nn, sizeof(nn), "%02u", static_cast<unsigned>(index % 100));
    return "ul." + idHex + "." + serial + "." + nn;
}

// ---------------------------------------------------------------------------

#if defined(_WIN32)
static std::vector<std::string> listDir(const std::string& dir) {
    std::vector<std::string> out;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.cFileName[0] != '.') out.push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return out;
}
#else
static std::vector<std::string> listDir(const std::string& dir) {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (struct dirent* e = readdir(d)) {
        if (e->d_name[0] != '.') out.push_back(e->d_name);
    }
    closedir(d);
    return out;
}
#endif

std::vector<FoundChunk> scanFirstChunks(const std::string& dir) {
    std::vector<FoundChunk> out;
    for (const auto& fn : listDir(dir)) {
        // Pattern: ul.<ID>.<SERIAL>.00  (FindFirst "ul.*.00" @0x3A2B77)
        if (fn.size() < 6 || fn.compare(0, 3, "ul.") != 0) continue;
        if (fn.compare(fn.size() - 3, 3, ".00") != 0) continue;

        const std::string mid = fn.substr(3, fn.size() - 6); // between "ul." and ".00"
        const size_t dot = mid.find('.');
        if (dot == std::string::npos) continue;

        FoundChunk c;
        c.idHex = mid.substr(0, dot);
        c.serial = mid.substr(dot + 1);
        if (c.idHex.size() != 8) continue;              // CRC id is 8 hex digits
        c.fileName = joinPath(dir, fn);
        out.push_back(std::move(c));
    }
    return out;
}

static uint64_t fileSizeOf(const std::string& p) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return 0;
    if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); return 0; }
    const long long sz = usbutils::tell64(f);
    std::fclose(f);
    return sz < 0 ? 0 : static_cast<uint64_t>(sz);
}

uint64_t gameTotalSize(const std::string& dir, const std::string& idHex,
                       const std::string& serial, uint32_t parts) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < parts; ++i)
        total += fileSizeOf(joinPath(dir, chunkFileName(idHex, serial, i)));
    return total;
}

int deleteChunks(const std::string& dir, const std::string& idHex,
                 const std::string& serial, uint32_t parts) {
    int removed = 0;
    for (uint32_t i = 0; i < parts; ++i) {
        if (std::remove(joinPath(dir, chunkFileName(idHex, serial, i)).c_str()) == 0)
            ++removed;
    }
    return removed;
}

bool renameChunks(const std::string& dir, const std::string& oldIdHex,
                  const std::string& newIdHex, const std::string& serial,
                  uint32_t parts) {
    for (uint32_t i = 0; i < parts; ++i) {
        const std::string from = joinPath(dir, chunkFileName(oldIdHex, serial, i));
        const std::string to   = joinPath(dir, chunkFileName(newIdHex, serial, i));
        if (std::rename(from.c_str(), to.c_str()) != 0) return false;
    }
    return true;
}

bool copyFile(const std::string& src, const std::string& dst,
              const ProgressFn& progress,
              const std::function<bool()>& cancelled) {
    FILE* in = std::fopen(src.c_str(), "rb");
    if (!in) return false;
    FILE* out = std::fopen(dst.c_str(), "wb");
    if (!out) { std::fclose(in); return false; }

    const uint64_t total = fileSizeOf(src);
    uint64_t done = 0;

    // 8 MiB buffer like the original's global @0x3C3290 (0x800020 bytes).
    std::vector<uint8_t> buf(8 * 1024 * 1024);
    bool ok = true;
    while (ok) {
        if (cancelled && cancelled()) { ok = false; break; }
        const size_t n = std::fread(buf.data(), 1, buf.size(), in);
        if (n == 0) break;
        if (std::fwrite(buf.data(), 1, n, out) != n) { ok = false; break; }
        done += n;
        if (progress) progress(done, total);
        if (n < buf.size()) break;
    }
    std::fclose(in);
    std::fclose(out);
    if (!ok) std::remove(dst.c_str());
    return ok;
}

StructureReport checkStructure(const std::string& dir, const std::string& idHex,
                               const std::string& serial, uint32_t parts) {
    StructureReport r;
    for (uint32_t i = 0; i < parts; ++i) {
        const std::string p = joinPath(dir, chunkFileName(idHex, serial, i));
        const uint64_t sz = fileSizeOf(p);
        if (sz == 0) r.missing.push_back(i);
        else { ++r.partsPresent; r.totalBytes += sz; }
    }
    return r;
}

} // namespace usbutils
