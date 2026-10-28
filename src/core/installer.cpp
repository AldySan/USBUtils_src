// SPDX-License-Identifier: MIT
#include "installer.h"
#include "ulid.h"

#include <cstdio>
#include <algorithm>
#include <sys/stat.h>
#include <sys/types.h>
#include "port.h"

namespace usbutils {

static std::string joinPath(const std::string& dir, const std::string& file) {
    if (dir.empty()) return file;
    if (dir.back() == '/' || dir.back() == '\\') return dir + file;
    return dir + "/" + file;
}

/// Create the destination directory when missing (mkdir -p; the original
/// assumed the USB drive root already existed, we are friendlier).
static bool ensureDir(const std::string& dir) {
    struct stat st{};
    if (stat(dir.c_str(), &st) == 0) return S_ISDIR(st.st_mode) != 0;
#if defined(_WIN32)
    return ::_mkdir(dir.c_str()) == 0;
#else
    return ::mkdir(dir.c_str(), 0755) == 0;
#endif
}

static uint64_t fileSizeOf(const std::string& p) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return 0;
    if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); return 0; }
    const long long sz = usbutils::tell64(f);
    std::fclose(f);
    return sz < 0 ? 0 : static_cast<uint64_t>(sz);
}

InstallResult installIso(const std::string& isoPath, const std::string& dir,
                         const InstallOptions& opt,
                         const ProgressFn& progress,
                         const std::function<bool()>& cancelled) {
    InstallResult res;

    if (!ensureDir(dir)) {
        res.error = "cannot access or create directory: " + dir;
        return res;
    }

    const uint64_t isoSize = fileSizeOf(isoPath);
    if (isoSize == 0) { res.error = "cannot open ISO: " + isoPath; return res; }

    // ---- Serial (SYSTEM.CNF) --------------------------------------------
    IsoInfo iso = readIsoInfo(isoPath);
    std::string serial = opt.serialOverride;
    if (opt.useIsoSerial && iso.valid) serial = iso.serial;
    if (serial.empty()) { res.error = "no serial (SYSTEM.CNF missing)"; return res; }

    // ---- Name / id -------------------------------------------------------
    std::string name = opt.gameName;
    if (name.empty() && !iso.volumeId.empty()) name = iso.volumeId;
    if (name.empty()) name = serial;
    const std::string idHex = gameIdHex(name);

    // ---- Media -----------------------------------------------------------
    const uint8_t media = opt.forceMedia ? opt.mediaOverride : mediaForSize(isoSize);

    // ---- Parts -----------------------------------------------------------
    const uint32_t parts = chunkCount(isoSize);

    // ---- Duplicate check -------------------------------------------------
    UlCfg cfg;
    cfg.load(dir);
    if (cfg.findByIdSerial(idHex, serial) != SIZE_MAX) {
        res.error = "game already installed (id " + idHex + ")";
        return res;
    }

    // ---- Copy chunks -----------------------------------------------------
    FILE* in = std::fopen(isoPath.c_str(), "rb");
    if (!in) { res.error = "cannot reopen ISO"; return res; }

    std::vector<uint8_t> buf(8 * 1024 * 1024);
    uint64_t done = 0;
    bool ok = true;

    for (uint32_t i = 0; i < parts && ok; ++i) {
        if (cancelled && cancelled()) { ok = false; break; }
        const uint64_t remaining = isoSize - done;
        const uint64_t chunkLeft =
            remaining > kChunkSize ? kChunkSize : remaining;
        const std::string chunkPath =
            joinPath(dir, chunkFileName(idHex, serial, i));

        FILE* out = std::fopen(chunkPath.c_str(), "wb");
        if (!out) { ok = false; res.error = "cannot create " + chunkPath; break; }

        uint64_t written = 0;
        while (written < chunkLeft) {
            const size_t want = static_cast<size_t>(
                std::min<uint64_t>(buf.size(), chunkLeft - written));
            const size_t n = std::fread(buf.data(), 1, want, in);
            if (n == 0 || std::fwrite(buf.data(), 1, n, out) != n) {
                ok = false; res.error = "write error on " + chunkPath;
                break;
            }
            written += n;
            done += n;
            if (progress) progress(done, isoSize);
        }
        std::fclose(out);
        if (ok) res.writtenFiles.push_back(chunkPath);
    }
    std::fclose(in);

    if (!ok) {
        for (const auto& f : res.writtenFiles) std::remove(f.c_str());
        return res;
    }

    // ---- ul.cfg record (BuildRecord @0x39B518) ---------------------------
    UlCfgRecord rec;
    rec.setName(name);
    rec.setSerial(serial);
    rec.parts = static_cast<uint8_t>(parts);
    rec.media = media;
    rec.flag  = 8;

    if (!cfg.append(rec)) {
        res.error = "failed to update ul.cfg in " + dir;
        for (const auto& f : res.writtenFiles) std::remove(f.c_str());
        return res;
    }

    res.ok = true;
    res.idHex = idHex;
    res.serial = serial;
    res.parts = parts;
    res.totalBytes = isoSize;
    return res;
}

bool exportIso(const std::string& dir, const UlCfgRecord& rec,
               const std::string& isoPath,
               const ProgressFn& progress,
               const std::function<bool()>& cancelled) {
    const std::string idHex = gameIdHex(rec.nameStr());
    const std::string serial = rec.serialStr();
    const uint64_t total = gameTotalSize(dir, idHex, serial, rec.parts);
    if (total == 0) return false;

    FILE* out = std::fopen(isoPath.c_str(), "wb");
    if (!out) return false;

    std::vector<uint8_t> buf(8 * 1024 * 1024);
    uint64_t done = 0;
    bool ok = true;
    for (uint32_t i = 0; i < rec.parts && ok; ++i) {
        FILE* in = std::fopen(
            joinPath(dir, chunkFileName(idHex, serial, i)).c_str(), "rb");
        if (!in) { ok = false; break; }
        size_t n;
        while ((n = std::fread(buf.data(), 1, buf.size(), in)) > 0) {
            if (cancelled && cancelled()) { ok = false; break; }
            if (std::fwrite(buf.data(), 1, n, out) != n) { ok = false; break; }
            done += n;
            if (progress) progress(done, total);
        }
        std::fclose(in);
    }
    std::fclose(out);
    if (!ok) std::remove(isoPath.c_str());
    return ok;
}

} // namespace usbutils
