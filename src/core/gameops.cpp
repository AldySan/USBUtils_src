// SPDX-License-Identifier: MIT
/*
 * USBUtils — open reimplementation of USBUtil v2.2 (PS2 USBExtreme manager)
 * Copyright (c) 2025 AldySan
 * Based on the reverse-engineering of "USBUtil v2.2 rev1.0" by ISEKO (2009).
 */
#include "gameops.h"

#include "ulid.h"
#include "gamestream.h"
#include "usbdevice.h"
#include "videorip.h"
#include "ps2load.h"
#include "port.h"

#include <cstdio>
#include <cstring>
#include <cctype>
#include <ctime>
#include <algorithm>
#include <filesystem>
#include <set>

namespace fs = std::filesystem;

namespace usbutils {

namespace {

constexpr size_t kCopyBuffer = 8u << 20;   // 8 MiB, same as WriteChunkFile

/// ASCII-only lower-case (locale independent, byte-wise).
std::string toLowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

/// The broken serial marker written by some third-party tools (string
/// @0x39B854; compared against 11 chars of the game-list code column).
constexpr const char* kBrokenSerial = "666.66";

uint64_t fileSizeOf(const std::string& p) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return 0;
    if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); return 0; }
    const long long sz = usbutils::tell64(f);
    std::fclose(f);
    return sz < 0 ? 0 : static_cast<uint64_t>(sz);
}

bool ensureDir(const std::string& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    return !ec;
}

/// Copy one file verbatim (8 MiB buffer, progress + cancel support).
/// `running` is the byte count already copied, `total` the expected grand
/// total; returns the new running count or UINT64_MAX on error/cancel.
uint64_t copyFile(const std::string& from, const std::string& to,
                  uint64_t running, uint64_t total,
                  const ProgressFn& progress,
                  const std::function<bool()>& cancelled,
                  std::string& error) {
    FILE* in = std::fopen(from.c_str(), "rb");
    if (!in) { error = "cannot open " + from; return UINT64_MAX; }
    FILE* out = std::fopen(to.c_str(), "wb");
    if (!out) { std::fclose(in); error = "cannot create " + to; return UINT64_MAX; }

    std::vector<uint8_t> buf(kCopyBuffer);
    uint64_t done = running;
    bool failed = false;
    while (true) {
        if (cancelled && cancelled()) { error = "cancelled"; failed = true; break; }
        const size_t n = std::fread(buf.data(), 1, buf.size(), in);
        if (n == 0) break;
        if (std::fwrite(buf.data(), 1, n, out) != n) {
            error = "write error on " + to; failed = true; break;
        }
        done += n;
        if (progress) progress(done, total);
    }
    std::fclose(in);
    if (std::fclose(out) != 0 && !failed) { error = "write error on " + to; failed = true; }
    return failed ? UINT64_MAX : done;
}

/// Read a game's SYSTEM.CNF through its chunk stream and extract the
/// serial (BOOT2 -> basename, normalized). Empty when not derivable.
} // namespace

// ---------------------------------------------------------------------------
// Find file in games
// ---------------------------------------------------------------------------

bool matchPattern(const std::string& nameOrPath, const std::string& pattern) {
    if (pattern.empty()) return true;

    // case-insensitive
    auto upper = [](const std::string& s) {
        std::string u = s;
        for (char& c : u)
            if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        return u;
    };
    const std::string text = upper(nameOrPath);
    const std::string pat  = upper(pattern);

    // classic iterative glob over ('*' / '?')
    size_t p = 0, t = 0, starP = std::string::npos, starT = 0;
    while (t < text.size()) {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == text[t])) {
            ++p; ++t;
        } else if (p < pat.size() && pat[p] == '*') {
            starP = p++; starT = t;
        } else if (starP != std::string::npos) {
            p = starP + 1; t = ++starT;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

std::vector<FileHit> findFileInGames(const std::string& dir, const UlCfg& cfg,
                                     const std::string& pattern,
                                     uint64_t sizeFilter) {
    std::vector<FileHit> hits;
    for (size_t i = 0; i < cfg.records.size(); ++i) {
        const UlCfgRecord& r = cfg.records[i];
        const std::string id = gameIdHex(r.nameStr());
        GameStream gs;
        if (!gs.open(dir, id, r.serialStr(), r.parts)) continue;

        walkIso([&gs](uint64_t off, void* dst, size_t n) {
            return gs.read(off, dst, n) == n;
        }, [&](const IsoEntry& e) {
            if (!e.isDir &&
                (sizeFilter == 0 || e.size == sizeFilter) &&
                (matchPattern(e.name, pattern) || matchPattern(e.path, pattern))) {
                FileHit h;
                h.gameIndex = i;
                h.gameName  = r.nameStr();
                h.entry     = e;
                hits.push_back(std::move(h));
            }
            return true;
        });
    }
    return hits;
}

// ---------------------------------------------------------------------------
// Explore / extract / inject
// ---------------------------------------------------------------------------

std::vector<IsoEntry> listGameFiles(const std::string& dir,
                                    const std::string& idHex,
                                    const std::string& serial,
                                    uint32_t parts,
                                    std::string* err) {
    std::vector<IsoEntry> files;
    GameStream gs;
    if (!gs.open(dir, idHex, serial, parts)) {
        if (err) *err = "cannot open game chunks";
        return files;
    }
    if (!walkIso([&gs](uint64_t off, void* dst, size_t n) {
            return gs.read(off, dst, n) == n;
        }, [&files](const IsoEntry& e) {
            if (!e.isDir) files.push_back(e);
            return true;
        })) {
        files.clear();
        if (err) *err = "not an ISO9660 game";
    }
    return files;
}

bool extractGameFile(const std::string& dir, const std::string& idHex,
                     const std::string& serial, uint32_t parts,
                     const IsoEntry& entry, const std::string& outPath,
                     const ProgressFn& progress,
                     const std::function<bool()>& cancelled) {
    GameStream gs;
    if (!gs.open(dir, idHex, serial, parts)) return false;

    FILE* out = std::fopen(outPath.c_str(), "wb");
    if (!out) return false;

    std::vector<uint8_t> buf(kCopyBuffer);
    const uint64_t offset = uint64_t(entry.lba) * 2048ull;
    uint64_t done = 0;
    bool ok = true;
    while (done < entry.size) {
        if (cancelled && cancelled()) { ok = false; break; }
        const size_t want = static_cast<size_t>(
            std::min<uint64_t>(buf.size(), entry.size - done));
        const size_t got = gs.read(offset + done, buf.data(), want);
        if (got != want) { ok = false; break; }
        if (std::fwrite(buf.data(), 1, got, out) != got) { ok = false; break; }
        done += got;
        if (progress) progress(done, entry.size);
    }
    std::fclose(out);
    if (!ok) std::remove(outPath.c_str());
    return ok;
}

InjectResult injectGameFile(const std::string& dir, const std::string& idHex,
                            const std::string& serial, uint32_t parts,
                            const std::string& isoPath,
                            const std::string& localFile,
                            const ProgressFn& progress) {
    InjectResult res;

    // normalize the wanted in-game path: '\' separators, no leading sep
    std::string want = isoPath;
    for (char& c : want) if (c == '/') c = '\\';
    while (!want.empty() && want.front() == '\\') want.erase(want.begin());
    if (want.empty()) { res.error = "empty in-game path"; return res; }

    const uint64_t localSize = fileSizeOf(localFile);
    if (localSize == 0) {
        res.error = "cannot read replacement file: " + localFile;
        return res;
    }

    // find the in-game file (path match is case-insensitive)
    const auto files = listGameFiles(dir, idHex, serial, parts, nullptr);
    auto ieq = [](const std::string& x, const std::string& y) {
        if (x.size() != y.size()) return false;
        for (size_t k = 0; k < x.size(); ++k) {
            char cx = x[k], cy = y[k];
            if (cx >= 'a' && cx <= 'z') cx = char(cx - 'a' + 'A');
            if (cy >= 'a' && cy <= 'z') cy = char(cy - 'a' + 'A');
            if (cx != cy) return false;
        }
        return true;
    };
    const IsoEntry* target = nullptr;
    for (const IsoEntry& e : files) {
        std::string p = e.path;
        while (!p.empty() && p.front() == '\\') p.erase(p.begin());
        if (ieq(p, want)) { target = &e; break; }
    }
    if (!target) {
        res.error = "no such file inside the game: " + want +
                    " (run `usbutils files` to list them)";
        return res;
    }
    if (target->size != localSize) {
        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      "size mismatch — in-game file is %llu bytes, replacement "
                      "is %llu bytes; only exact-size replacement is possible "
                      "(LBAs must stay valid)",
                      static_cast<unsigned long long>(target->size),
                      static_cast<unsigned long long>(localSize));
        res.error = buf;
        return res;
    }

    GameStream gs;
    if (!gs.open(dir, idHex, serial, parts)) {
        res.error = "cannot open game chunks";
        return res;
    }

    FILE* in = std::fopen(localFile.c_str(), "rb");
    if (!in) { res.error = "cannot open " + localFile; return res; }

    std::vector<uint8_t> buf(kCopyBuffer);
    const uint64_t offset = uint64_t(target->lba) * 2048ull;
    uint64_t done = 0;
    while (done < localSize) {
        const size_t want2 = static_cast<size_t>(
            std::min<uint64_t>(buf.size(), localSize - done));
        if (std::fread(buf.data(), 1, want2, in) != want2) {
            std::fclose(in);
            res.error = "short read on " + localFile;
            return res;
        }
        if (!gs.write(offset + done, buf.data(), want2)) {
            std::fclose(in);
            res.error = "write error inside the game stream";
            return res;
        }
        done += want2;
        if (progress) progress(done, localSize);
    }
    std::fclose(in);

    res.ok = true;
    res.path = want;
    res.bytes = done;
    return res;
}

// ---------------------------------------------------------------------------
// Copy / move
// ---------------------------------------------------------------------------

CopyResult copyGame(const std::string& srcDir, UlCfg& srcCfg, size_t index,
                    const std::string& destDir, bool deleteSource,
                    const ProgressFn& progress,
                    const std::function<bool()>& cancelled) {
    CopyResult res;
    if (index >= srcCfg.records.size()) { res.error = "bad game index"; return res; }

    const UlCfgRecord r = srcCfg.records[index];
    const std::string id     = gameIdHex(r.nameStr());
    const std::string serial = r.serialStr();

    std::error_code ec;
    if (fs::equivalent(srcDir, destDir, ec) || (!ec && srcDir == destDir)) {
        res.error = "source and destination are the same drive";
        return res;
    }
    if (!ensureDir(destDir)) {
        res.error = "cannot access or create directory: " + destDir;
        return res;
    }

    // a game with the same id+serial must not already exist there
    {
        UlCfg dest;
        dest.load(destDir);
        if (dest.findByIdSerial(id, serial) != SIZE_MAX) {
            res.error = "\"" + r.nameStr() +
                        "\" already exists on the destination drive";
            return res;
        }
    }

    const uint64_t total = gameTotalSize(srcDir, id, serial, r.parts);
    uint64_t done = 0;
    std::vector<std::string> written;

    for (uint32_t i = 0; i < r.parts; ++i) {
        const std::string name = chunkFileName(id, serial, i);
        const std::string from = joinPath(srcDir, name);
        const std::string to   = joinPath(destDir, name);
        if (!fs::exists(from)) {
            res.error = "missing chunk file: " + name +
                        " (run a structure check first)";
            for (const std::string& w : written) std::remove(w.c_str());
            return res;
        }
        done = copyFile(from, to, done, total, progress, cancelled, res.error);
        if (done == UINT64_MAX) {
            // roll back every chunk written so far: half-copied games would
            // otherwise sit on the destination as unreferenced "obsolete"
            // files until Restore-space finds them
            for (const std::string& w : written) std::remove(w.c_str());
            std::remove(to.c_str());
            return res;
        }
        written.push_back(to);
    }

    // carry over the video-rip sidecar so ripped games stay reversible
    const std::string sidecarFrom =
        joinPath(srcDir, chunkFileName(id, serial, 0) + ".rip");
    const std::string sidecarTo =
        joinPath(destDir, chunkFileName(id, serial, 0) + ".rip");
    uint64_t sidecarBytes = 0;
    if (fs::exists(sidecarFrom)) {
        const uint64_t sc = copyFile(sidecarFrom, sidecarTo, done, total,
                                     progress, cancelled, res.error);
        if (sc == UINT64_MAX) {
            for (const std::string& w : written) std::remove(w.c_str());
            std::remove(sidecarTo.c_str());
            return res;
        }
        sidecarBytes = sc - done;
        done = sc;
        written.push_back(sidecarTo);
    }

    // append the record on the destination
    {
        UlCfg dest;
        dest.load(destDir);
        if (!dest.append(r)) {
            res.error = "cannot write ul.cfg on the destination drive";
            for (const std::string& w : written) std::remove(w.c_str());
            return res;
        }
    }

    if (deleteSource) {
        // mirror the original's move semantics: delete source data + record
        if (fs::exists(sidecarFrom)) std::remove(sidecarFrom.c_str());
        srcCfg.removeGame(index);
    }

    res.ok = true;
    res.copiedBytes = done;
    res.filesWritten = static_cast<int>(written.size()) + 1;   // + ul.cfg
    (void)sidecarBytes;
    return res;
}

// ---------------------------------------------------------------------------
// Correct code 666.66
// ---------------------------------------------------------------------------

std::vector<SerialFix> fixBrokenSerials(const std::string& dir, UlCfg& cfg,
                                        bool apply) {
    std::vector<SerialFix> fixes;
    for (size_t i = 0; i < cfg.records.size(); ++i) {
        const UlCfgRecord& r = cfg.records[i];
        const std::string serial = r.serialStr();
        if (serial != kBrokenSerial && !serial.empty()) continue;

        SerialFix fix;
        fix.index     = i;
        fix.gameName  = r.nameStr();
        fix.oldSerial = serial;

        // re-derive the real serial from the game's own SYSTEM.CNF
        GameStream gs;
        std::string cnf;
        if (gs.open(dir, gameIdHex(r.nameStr()), serial, r.parts)) {
            walkIso([&gs](uint64_t off, void* dst, size_t n) {
                return gs.read(off, dst, n) == n;
            }, [&gs, &cnf](const IsoEntry& e) {
                // case-insensitive like readIsoInfo()/probeOpticalDrive() —
                // some non-standard rips store a lower-case entry name
                if (!e.isDir && toLowerAscii(e.name) == "system.cnf") {
                    const size_t want =
                        static_cast<size_t>(std::min<uint64_t>(e.size, 512));
                    cnf.assign(want, '\0');
                    gs.read(uint64_t(e.lba) * 2048ull, &cnf[0], want);
                    return false;      // stop the walk
                }
                return true;
            });
        }
        if (!cnf.empty())
            fix.newSerial = normalizeSerial(bootElfFromCnf(cnf));

        if (apply && !fix.newSerial.empty()) {
            fix.applied = cfg.renameSerial(i, fix.newSerial);
        }
        fixes.push_back(std::move(fix));
    }
    return fixes;
}

// ---------------------------------------------------------------------------
// General report
// ---------------------------------------------------------------------------

std::string generateReport(const std::string& dir, const UlCfg& cfg) {
    std::string out;
    char line[512];

    std::snprintf(line, sizeof(line),
                  "USBUtils %s - general report\n", USBUTILS_VERSION);
    out += line;
    {
        char tbuf[32];
        const time_t now = std::time(nullptr);
        std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M UTC",
                      std::gmtime(&now));
        out += std::string("Generated: ") + tbuf + "\n";
    }

    uint64_t freeB = 0, totalB = 0;
    if (fsSpace(dir, freeB, totalB)) {
        std::snprintf(line, sizeof(line),
                      "Drive: %s\n  total %llu bytes, free %llu bytes\n",
                      dir.c_str(), static_cast<unsigned long long>(totalB),
                      static_cast<unsigned long long>(freeB));
        out += line;
    } else {
        out += "Drive: " + dir + "\n";
    }

    uint64_t gamesBytes = 0;
    for (const UlCfgRecord& r : cfg.records)
        gamesBytes += gameTotalSize(dir, gameIdHex(r.nameStr()),
                                    r.serialStr(), r.parts);
    std::snprintf(line, sizeof(line), "Games total: %zu (%llu bytes)\n",
                  cfg.records.size(), static_cast<unsigned long long>(gamesBytes));
    out += line;

    out += "\n------ Game files included ------\n";
    for (size_t i = 0; i < cfg.records.size(); ++i) {
        const UlCfgRecord& r = cfg.records[i];
        const std::string id = gameIdHex(r.nameStr());
        const StructureReport rep = checkStructure(dir, id, r.serialStr(), r.parts);

        std::string videos = "n/a";
        std::string err;
        const auto vids = scanVideos(dir, id, r.serialStr(), r.parts, &err);
        if (err.empty()) {
            uint64_t blanked = 0;
            for (const VideoEntry& v : vids)
                if (v.blanked) blanked += v.size;
            videos = std::to_string(vids.size()) + " video(s)";
            if (blanked) videos += ", blanked";
        }

        std::string injected = "no";
        const Ps2LoadInfo ps2 = queryPs2Load(dir, id, r.serialStr(), r.parts);
        if (ps2.state == Ps2LoadState::InjectedByTool)        injected = "PS2Load";
        else if (ps2.state == Ps2LoadState::InjectedManually) injected = "PS2Load (manual)";

        std::snprintf(line, sizeof(line),
                      "[%zu] %-32s %-12s %s  parts=%u  %10llu bytes  %s  %s  %s\n",
                      i, r.nameStr().c_str(), r.serialStr().c_str(),
                      r.media == kMediaCd ? "CD " : "DVD", r.parts,
                      static_cast<unsigned long long>(rep.totalBytes),
                      rep.ok() ? "structure OK" : "MISSING parts",
                      videos.c_str(), injected.c_str());
        out += line;
    }

    out += "\n------ Structure evaluation ------\n";
    int bad = 0;
    for (size_t i = 0; i < cfg.records.size(); ++i) {
        const UlCfgRecord& r = cfg.records[i];
        const StructureReport rep =
            checkStructure(dir, gameIdHex(r.nameStr()), r.serialStr(), r.parts);
        if (rep.ok()) continue;
        ++bad;
        std::string miss = "[";
        for (uint32_t m : rep.missing)
            miss += std::to_string(m) + " ";
        miss += "]";
        std::snprintf(line, sizeof(line), "  [%zu] %-32s missing parts %s\n",
                      i, r.nameStr().c_str(), miss.c_str());
        out += line;
    }
    if (bad == 0)
        out += cfg.records.empty() ? "  no games on the drive\n"
                                   : "  all games complete\n";

    out += "\n------ Broken serial codes ------\n";
    UlCfg tmp = cfg;                 // dry run: never applied here
    const auto fixes = fixBrokenSerials(dir, tmp, false);
    if (fixes.empty()) {
        out += "  none\n";
    } else {
        for (const SerialFix& f : fixes) {
            std::snprintf(line, sizeof(line), "  [%zu] %-32s %s -> %s\n",
                          f.index, f.gameName.c_str(), f.oldSerial.c_str(),
                          f.newSerial.empty() ? "(unknown - not fixable)"
                                              : f.newSerial.c_str());
            out += line;
        }
    }

    return out;
}

// ---------------------------------------------------------------------------
// Sorting ("Sort the list of games in memory and disk")
// ---------------------------------------------------------------------------

namespace {

int icmpAscii(const std::string& a, const std::string& b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        int ca = static_cast<unsigned char>(a[i]);
        int cb = static_cast<unsigned char>(b[i]);
        if (std::isalpha(ca)) ca = std::tolower(ca);
        if (std::isalpha(cb)) cb = std::tolower(cb);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    return 0;
}

} // namespace

bool sortGames(const std::string& dir, size_t* movedOut) {
    UlCfg cfg;
    if (!cfg.load(dir)) return false;

    const std::vector<UlCfgRecord> before = cfg.records;
    std::stable_sort(cfg.records.begin(), cfg.records.end(),
                     [](const UlCfgRecord& a, const UlCfgRecord& b) {
                         // invalid/trailing garbage sinks to the end
                         if (a.valid() != b.valid()) return b.valid();
                         const int c = icmpAscii(a.nameStr(), b.nameStr());
                         if (c != 0) return c < 0;
                         return icmpAscii(a.serialStr(), b.serialStr()) < 0;
                     });

    if (movedOut) {
        *movedOut = 0;
        for (size_t i = 0; i < before.size(); ++i)
            if (i >= cfg.records.size() ||
                std::memcmp(&before[i], &cfg.records[i], UlCfgRecord::kSize))
                ++*movedOut;
    }

    // Nothing to do when already sorted — skip the rewrite (the original
    // still rewrites, but skipping keeps timestamps stable).
    bool identical = before.size() == cfg.records.size();
    for (size_t i = 0; identical && i < before.size(); ++i)
        identical = std::memcmp(&before[i], &cfg.records[i],
                                 UlCfgRecord::kSize) == 0;
    if (identical) return true;
    return cfg.save();
}

// ---------------------------------------------------------------------------
// Restore space ("Rest. Spc." / Disk Status INFORMATION panel)
// ---------------------------------------------------------------------------

namespace {

bool startsWithUlDot(const std::string& name) {
    // "ul." prefix, ASCII case-insensitive (Windows drives are case-blind)
    return name.size() > 3
        && (name[0] == 'u' || name[0] == 'U')
        && (name[1] == 'l' || name[1] == 'L')
        && name[2] == '.';
}

} // namespace

DriveSpaceReport scanDriveSpace(const std::string& dir) {
    DriveSpaceReport rep;
    rep.games = 0;

    UlCfg cfg;
    const bool haveCfg = cfg.load(dir);
    if (haveCfg) rep.games = cfg.records.size();

    // Files a game claims: its chunk files + the video-rip sidecar.
    std::set<std::string> referenced;   // lower-cased names
    if (haveCfg) {
        for (const UlCfgRecord& r : cfg.records) {
            if (!r.valid()) continue;
            const std::string id = gameIdHex(r.nameStr());
            for (uint32_t i = 0; i < r.parts; ++i) {
                std::string n = chunkFileName(id, r.serialStr(), i);
                referenced.insert(toLowerAscii(n));
            }
            std::string rip = chunkFileName(id, r.serialStr(), 0) + ".rip";
            referenced.insert(toLowerAscii(rip));
        }
    }

    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), endIt; !ec && it != endIt;
         it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (!startsWithUlDot(name)) continue;       // not game-area file
        if (toLowerAscii(name) == "ul.cfg") continue;

        bool isFile = false;
        uint64_t sz = 0;
        {
            std::error_code ec2, ec3;
            if (it->is_regular_file(ec2)) {
                isFile = true;
                const auto s = it->file_size(ec3);
                if (!ec3) sz = static_cast<uint64_t>(s);
            }
        }
        if (!isFile) continue;   // a directory named "ul.*" is not game data

        if (referenced.count(toLowerAscii(name))) {
            ++rep.gameFiles;
            rep.gameBytes += sz;
        } else {
            ++rep.obsoleteFiles;
            rep.obsoleteBytes += sz;
            rep.obsolete.push_back(name);
        }
        if (sz == 0) ++rep.zeroSizeFiles;
    }

    uint64_t freeB = 0, totalB = 0;
    if (fsSpace(dir, freeB, totalB)) rep.freeBytes = freeB;
    return rep;
}

int restoreSpace(const std::string& dir, const DriveSpaceReport& rep) {
    int removed = 0;
    for (const std::string& name : rep.obsolete) {
        std::error_code ec;
        if (fs::remove(fs::path(dir) / name, ec) && !ec) ++removed;
    }
    return removed;
}

} // namespace usbutils
