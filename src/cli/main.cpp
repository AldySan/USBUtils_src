// SPDX-License-Identifier: MIT
/*
 * usbutils-cli — command line manager for PS2 USBExtreme game drives.
 *
 *   usbutils list                       -d <dir>
 *   usbutils install <iso>              -d <dir> [-n "Game Name"]
 *   usbutils export <index|all>         -d <dir> -o <file.iso | outdir>
 *   usbutils remove <index>             -d <dir>
 *   usbutils rename <index> -n "Name"   -d <dir>
 *   usbutils check                      -d <dir>
 *   usbutils recover                    -d <dir> [--apply]
 *   usbutils videos <index>             -d <dir>
 *   usbutils rip <index>                -d <dir> [--all | --only p1,p2] [--no-backup]
 *   usbutils unrip <index>              -d <dir>
 *   usbutils dnas list                  -p <pkg.dni>
 *   usbutils dnas patch <index>         -d <dir> -p <pkg.dni> --dna <pkg.dna> -v <ver>
 *   usbutils drives
 *   usbutils id -n "Name"               (show computed game id)
 */
#include "../core/ulcfg.h"
#include "../core/installer.h"
#include "../core/gamefile.h"
#include "../core/gamestream.h"
#include "../core/ulid.h"
#include "../core/recovery.h"
#include "../core/dnas.h"
#include "../core/videorip.h"
#include "../core/gamedb.h"
#include "../core/usbdevice.h"

#ifndef USBUTILS_VERSION
#define USBUTILS_VERSION "0.0.1"
#endif

#include <cstdio>
#include <cstring>
#include <cctype>
#include <string>
#include <vector>
#include <functional>
#include <filesystem>

using namespace usbutils;

static void usage() {
    std::printf(
        "USBUtils CLI %s - PS2 USBExtreme game manager (C++ reimplementation)\n"
        "Usage:\n"
        "  usbutils list                 -d <dir>\n"
        "  usbutils install <iso>        -d <dir> [-n \"Game Name\"]\n"
        "  usbutils export <index|all>   -d <dir> -o <file.iso | outdir>\n"
        "  usbutils remove <index>       -d <dir>\n"
        "  usbutils rename <index>       -d <dir> -n \"New Name\"\n"
        "  usbutils check                -d <dir>\n"
        "  usbutils recover              -d <dir> [--apply]\n"
        "  usbutils videos <index>       -d <dir>\n"
        "  usbutils rip <index>          -d <dir> [--all | --only path,...] [--no-backup]\n"
        "  usbutils unrip <index>        -d <dir>   (restore ripped videos)\n"
        "  usbutils id                   -n \"Name\"   (show computed game id)\n"
        "  usbutils drives\n"
        "  usbutils dnas list            -p <usbutil.dni>\n"
        "  usbutils dnas patch <index>   -d <dir> -p <usbutil.dni> --dna <usbutil.dna> -v <ver>\n"
        "  usbutils --version\n", USBUTILS_VERSION);
}

static std::string humanSize(uint64_t b) {
    char out[32];
    if (b >= 1024ull * 1024 * 1024)
        std::snprintf(out, sizeof(out), "%.2f GiB", double(b) / (1024.0 * 1024 * 1024));
    else if (b >= 1024 * 1024)
        std::snprintf(out, sizeof(out), "%.2f MiB", double(b) / (1024.0 * 1024));
    else
        std::snprintf(out, sizeof(out), "%.2f KiB", double(b) / 1024.0);
    return out;
}

static ProgressFn makeProgress() {
    return [](uint64_t done, uint64_t total) {
        std::printf("\r  %s / %s", humanSize(done).c_str(), humanSize(total).c_str());
        std::fflush(stdout);
    };
}

struct Args {
    std::string cmd, dir, iso, name, out, dni, dna, ver, only;
    int index = -1;
    bool all = false, apply = false, noBackup = false;
};

static Args parse(int argc, char** argv) {
    Args a;
    if (argc < 2) return a;
    a.cmd = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string s = argv[i];
        auto next = [&]() -> std::string {
            return (i + 1 < argc) ? argv[++i] : "";
        };
        if (s == "-d") a.dir = next();
        else if (s == "-n") a.name = next();
        else if (s == "-o") a.out = next();
        else if (s == "-p") a.dni = next();
        else if (s == "--dna") a.dna = next();
        else if (s == "-v") a.ver = next();
        else if (s == "--only") a.only = next();
        else if (s == "--all") a.all = true;
        else if (s == "--apply") a.apply = true;
        else if (s == "--no-backup") a.noBackup = true;
        else if (s == "all") a.all = true;
        else if (std::isdigit(static_cast<unsigned char>(s[0])) && s.find_first_not_of("0123456789") == std::string::npos)
            a.index = std::atoi(s.c_str());
        else if (a.iso.empty() && a.cmd == "install") a.iso = s;
    }
    return a;
}

/// Resolve the record + computed id for commands that take an index.
static bool resolveGame(const Args& a, UlCfg& cfg, size_t& index,
                        std::string& idHex) {
    if (a.dir.empty() || a.index < 0) { usage(); return false; }
    if (!cfg.load(a.dir)) { std::printf("ERROR: no ul.cfg in %s\n", a.dir.c_str()); return false; }
    if (a.index >= (int)cfg.records.size()) { std::printf("ERROR: bad index\n"); return false; }
    index = static_cast<size_t>(a.index);
    idHex = gameIdHex(cfg.records[index].nameStr());
    return true;
}

static int cmdList(const Args& a) {
    UlCfg cfg;
    if (!cfg.load(a.dir)) {
        std::printf("no ul.cfg in %s (empty or new drive)\n", a.dir.c_str());
        return 0;
    }
    std::printf("%-3s %-32s %-12s %-5s %-6s %s\n", "#", "NAME", "SERIAL", "PARTS", "MEDIA", "SIZE");
    for (size_t i = 0; i < cfg.records.size(); ++i) {
        const UlCfgRecord& r = cfg.records[i];
        const std::string id = gameIdHex(r.nameStr());
        const uint64_t sz = gameTotalSize(a.dir, id, r.serialStr(), r.parts);
        std::printf("%-3zu %-32s %-12s %-5u %-6s %s\n", i,
                    r.nameStr().substr(0, 32).c_str(), r.serialStr().c_str(),
                    r.parts, r.media == kMediaCd ? "CD" : "DVD",
                    humanSize(sz).c_str());
    }
    std::printf("%zu game(s)\n", cfg.records.size());
    return 0;
}

static int cmdInstall(const Args& a) {
    if (a.iso.empty() || a.dir.empty()) { usage(); return 2; }
    InstallOptions opt;
    opt.gameName = a.name;

    const InstallResult r = installIso(a.iso, a.dir, opt, makeProgress());
    std::printf("\n");
    if (!r.ok) { std::printf("ERROR: %s\n", r.error.c_str()); return 1; }
    std::printf("installed: %s\n  id=%s serial=%s parts=%u size=%s\n",
                opt.gameName.c_str(), r.idHex.c_str(), r.serial.c_str(),
                r.parts, humanSize(r.totalBytes).c_str());
    return 0;
}

static int cmdExport(const Args& a) {
    if (a.dir.empty() || a.out.empty() || (a.index < 0 && !a.all)) { usage(); return 2; }
    UlCfg cfg;
    if (!cfg.load(a.dir)) { std::printf("ERROR: no ul.cfg\n"); return 1; }

    const auto exportOne = [&](size_t i, const std::string& outPath) -> bool {
        const bool ok = exportIso(a.dir, cfg.records[i], outPath, makeProgress());
        std::printf("\n");
        if (ok) std::printf("exported %s\n", outPath.c_str());
        else    std::printf("ERROR: export failed (%s)\n", outPath.c_str());
        return ok;
    };

    if (a.all) {
        // out is treated as an output directory (created when missing)
        std::string outDir = a.out;
        std::error_code ec;
        std::filesystem::create_directories(outDir, ec);
        if (ec) { std::printf("ERROR: cannot create %s\n", outDir.c_str()); return 1; }
        bool ok = true;
        for (size_t i = 0; i < cfg.records.size() && ok; ++i) {
            std::string name = cfg.records[i].nameStr();
            for (char& c : name)
                if (std::strchr("\\/:*?\"<>|", c)) c = '_';
            const std::string outPath =
                (outDir.back() == '/' || outDir.back() == '\\')
                    ? outDir + name + ".iso"
                    : outDir + "/" + name + ".iso";
            ok = exportOne(i, outPath);
        }
        return ok ? 0 : 1;
    }

    if (a.index >= (int)cfg.records.size()) { std::printf("ERROR: bad index\n"); return 1; }
    return exportOne(static_cast<size_t>(a.index), a.out) ? 0 : 1;
}

static int cmdRemove(const Args& a) {
    if (a.dir.empty() || a.index < 0) { usage(); return 2; }
    UlCfg cfg;
    if (!cfg.load(a.dir)) { std::printf("ERROR: no ul.cfg\n"); return 1; }
    if (a.index >= (int)cfg.records.size()) { std::printf("ERROR: bad index\n"); return 1; }
    const int n = cfg.removeGame(static_cast<size_t>(a.index));
    std::printf("removed %d chunk file(s)\n", n);
    return 0;
}

static int cmdRename(const Args& a) {
    if (a.dir.empty() || a.index < 0 || a.name.empty()) { usage(); return 2; }
    UlCfg cfg;
    if (!cfg.load(a.dir)) { std::printf("ERROR: no ul.cfg\n"); return 1; }
    if (a.index >= (int)cfg.records.size()) { std::printf("ERROR: bad index\n"); return 1; }
    if (!cfg.renameGame(static_cast<size_t>(a.index), a.name)) {
        std::printf("ERROR: rename failed\n");
        return 1;
    }
    std::printf("renamed to \"%s\" (id now %s)\n", a.name.c_str(),
                gameIdHex(a.name).c_str());
    return 0;
}

static int cmdCheck(const Args& a) {
    if (a.dir.empty()) { usage(); return 2; }
    UlCfg cfg;
    if (!cfg.load(a.dir)) { std::printf("ERROR: no ul.cfg\n"); return 1; }
    int bad = 0;
    for (size_t i = 0; i < cfg.records.size(); ++i) {
        const UlCfgRecord& r = cfg.records[i];
        const std::string id = gameIdHex(r.nameStr());
        const StructureReport rep = checkStructure(a.dir, id, r.serialStr(), r.parts);
        if (!rep.ok()) {
            ++bad;
            std::printf("[%zu] %-32s MISSING parts:", i, r.nameStr().c_str());
            for (uint32_t m : rep.missing) std::printf(" %02u", m);
            std::printf("\n");
        } else {
            std::printf("[%zu] %-32s OK (%s)\n", i, r.nameStr().c_str(),
                        humanSize(rep.totalBytes).c_str());
        }
    }
    if (bad) std::printf("%d problem(s)\n", bad);
    else     std::printf("structure OK (%zu games)\n", cfg.records.size());
    return bad ? 1 : 0;
}

static int cmdRecover(const Args& a) {
    if (a.dir.empty()) { usage(); return 2; }
    const auto orphans = findOrphans(a.dir);
    if (orphans.empty()) { std::printf("no orphan games found\n"); return 0; }

    UlCfg cfg;
    cfg.load(a.dir);

    for (size_t i = 0; i < orphans.size(); ++i) {
        const OrphanGame& og = orphans[i];
        std::printf("[%zu] ul.%s.%s  parts=%u size=%s\n", i, og.idHex.c_str(),
                    og.serial.c_str(), og.partsFound, humanSize(og.totalBytes).c_str());
        if (a.apply) {
            const UlCfgRecord rec = makeRecoveredRecord(og, og.suggestedName);
            cfg.append(rec);
            std::printf("     -> recovered as \"%s\"\n", rec.nameStr().c_str());
        }
    }
    if (a.apply) {
        cfg.save();
        std::printf("ul.cfg updated\n");
    } else {
        std::printf("run with --apply to rebuild ul.cfg\n");
    }
    return 0;
}

static int cmdDrives() {
    for (const UsbDrive& d : listDrives()) {
        std::printf("%-24s %-16s %10s free  %s%s\n", d.root.c_str(),
                    d.label.substr(0, 16).c_str(),
                    humanSize(d.freeBytes).c_str(),
                    d.fat32 ? "FAT32 " : "",
                    d.hasUlCfg ? "[ul.cfg]" : "");
    }
    return 0;
}

// ---------------------------------------------------------------------------
// video rip
// ---------------------------------------------------------------------------

static int cmdVideos(const Args& a) {
    UlCfg cfg;
    size_t index = 0;
    std::string id;
    if (!resolveGame(a, cfg, index, id)) return 2;

    std::string err;
    const auto videos = scanVideos(a.dir, id, cfg.records[index].serialStr(),
                                   cfg.records[index].parts, &err);
    if (!err.empty()) { std::printf("ERROR: %s\n", err.c_str()); return 1; }

    if (videos.empty()) {
        std::printf("no video files recognized in this game\n");
        return 0;
    }
    uint64_t total = 0, blanked = 0;
    std::printf("%-4s %-44s %10s  %s\n", "#", "FILE", "SIZE", "STATE");
    for (size_t i = 0; i < videos.size(); ++i) {
        const VideoEntry& v = videos[i];
        total   += v.size;
        blanked += v.blanked ? v.size : 0;
        std::printf("%-4zu %-44s %10s  %s\n", i, v.path.substr(0, 44).c_str(),
                    humanSize(v.size).c_str(),
                    v.blanked ? (v.hasBackup ? "blanked (restorable)"
                                            : "blanked (no backup)")
                              : "original");
    }
    const uint64_t backup = videoBackupSize(a.dir, id, cfg.records[index].serialStr());
    std::printf("videos: %zu  total %s  blanked %s\n", videos.size(),
                humanSize(total).c_str(), humanSize(blanked).c_str());
    if (backup) std::printf("backup file: %s\n", humanSize(backup).c_str());
    return 0;
}

static int cmdRip(const Args& a) {
    UlCfg cfg;
    size_t index = 0;
    std::string id;
    if (!resolveGame(a, cfg, index, id)) return 2;

    std::string err;
    const auto videos = scanVideos(a.dir, id, cfg.records[index].serialStr(),
                                   cfg.records[index].parts, &err);
    if (!err.empty()) { std::printf("ERROR: %s\n", err.c_str()); return 1; }
    if (videos.empty()) { std::printf("no video files recognized\n"); return 0; }

    // selection: --only (comma-separated paths, case-insensitive) or everything
    std::vector<VideoEntry> selection;
    if (!a.only.empty()) {
        std::vector<std::string> wanted;
        std::string cur;
        for (char c : a.only) {
            if (c == ',') { wanted.push_back(cur); cur.clear(); }
            else cur += c;
        }
        wanted.push_back(cur);
        auto ieq = [](const std::string& x, const std::string& y) {
            if (x.size() != y.size()) return false;
            for (size_t i = 0; i < x.size(); ++i) {
                char cx = x[i], cy = y[i];
                if (cx >= 'A' && cx <= 'Z') cx += 32;
                if (cy >= 'A' && cy <= 'Z') cy += 32;
                if (cx != cy) return false;
            }
            return true;
        };
        auto baseName = [](const std::string& p) {
            const size_t s = p.rfind('\\');
            return s == std::string::npos ? p : p.substr(s + 1);
        };
        for (const VideoEntry& v : videos) {
            for (const auto& w : wanted)
                if (ieq(w, v.path) || ieq(w, baseName(v.path))) {
                    selection.push_back(v);
                    break;
                }
        }
        if (selection.empty()) {
            std::printf("ERROR: none of the --only paths matched; "
                        "run `usbutils videos %d -d %s` to list them\n",
                        a.index, a.dir.c_str());
            return 1;
        }
    } else {
        selection = videos;                       // default: everything
    }

    const RipResult r = ripVideos(a.dir, id, cfg.records[index].serialStr(),
                                  cfg.records[index].parts, selection,
                                  !a.noBackup, makeProgress());
    std::printf("\n");
    if (!r.ok) { std::printf("ERROR: %s\n", r.error.c_str()); return 1; }
    if (r.rippedCount == 0) { std::printf("nothing to rip (already blanked)\n"); return 0; }
    std::printf("ripped %u video(s), blanked %s", r.rippedCount,
                humanSize(r.blankedBytes).c_str());
    if (!a.noBackup)
        std::printf(", backup %s", humanSize(r.backupBytes).c_str());
    std::printf("\nundo with: usbutils unrip %d -d %s\n", a.index, a.dir.c_str());
    return 0;
}

static int cmdUnrip(const Args& a) {
    UlCfg cfg;
    size_t index = 0;
    std::string id;
    if (!resolveGame(a, cfg, index, id)) return 2;

    const RipResult r = restoreVideos(a.dir, id, cfg.records[index].serialStr(),
                                      cfg.records[index].parts, makeProgress());
    std::printf("\n");
    if (!r.ok) { std::printf("ERROR: %s\n", r.error.c_str()); return 1; }
    std::printf("restored %u video(s), %s\n", r.rippedCount,
                humanSize(r.blankedBytes).c_str());
    return 0;
}

static int cmdDnas(const Args& a) {
    if (a.dni.empty()) { usage(); return 2; }
    DnasPackage pkg;
    if (!pkg.loadIndex(a.dni)) { std::printf("ERROR: cannot read %s\n", a.dni.c_str()); return 1; }
    if (!a.dna.empty()) pkg.setDataPath(a.dna);

    if (a.cmd == "list" || a.index < 0) {
        for (const auto& e : pkg.entries())
            std::printf("DNAS%-5s size=%7u offset=%llu\n", e.version.c_str(),
                        e.size, static_cast<unsigned long long>(e.offset));
        return 0;
    }

    // dnas patch <index> -v <ver>
    if (a.dir.empty() || a.dna.empty() || a.ver.empty()) { usage(); return 2; }
    UlCfg cfg;
    if (!cfg.load(a.dir)) { std::printf("ERROR: no ul.cfg\n"); return 1; }
    if (a.index >= (int)cfg.records.size()) { std::printf("ERROR: bad index\n"); return 1; }
    const UlCfgRecord& r = cfg.records[a.index];
    const std::string id = gameIdHex(r.nameStr());
    const int n = pkg.patchGame(a.dir, id, r.serialStr(), r.parts, a.ver);
    if (n < 0) { std::printf("ERROR: patch failed (IOPRP not found or too large)\n"); return 1; }
    std::printf("patched with DNAS%s\n", a.ver.c_str());
    return 0;
}

int main(int argc, char** argv) {
    const Args a = parse(argc, argv);
    if (a.cmd.empty()) { usage(); return 2; }

    if (a.cmd == "--version" || a.cmd == "version") {
        std::printf("usbutils %s\n", USBUTILS_VERSION);
        return 0;
    }
    if (a.cmd == "list")    return cmdList(a);
    if (a.cmd == "install") return cmdInstall(a);
    if (a.cmd == "export")  return cmdExport(a);
    if (a.cmd == "remove")  return cmdRemove(a);
    if (a.cmd == "rename")  return cmdRename(a);
    if (a.cmd == "check")   return cmdCheck(a);
    if (a.cmd == "recover") return cmdRecover(a);
    if (a.cmd == "drives")  return cmdDrives();
    if (a.cmd == "videos")  return cmdVideos(a);
    if (a.cmd == "rip")     return cmdRip(a);
    if (a.cmd == "unrip")   return cmdUnrip(a);
    if (a.cmd == "dnas")    return cmdDnas(a);
    if (a.cmd == "id") {
        if (a.name.empty()) { usage(); return 2; }
        std::printf("%s\n", gameIdHex(a.name).c_str());
        return 0;
    }
    usage();
    return 2;
}
