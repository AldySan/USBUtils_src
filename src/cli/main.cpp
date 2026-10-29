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
 *   usbutils ps2load <index>            -d <dir>
 *   usbutils ps2load inject <index>     -d <dir> [--elf <PS2LOAD.ELF>]
 *   usbutils ps2load remove <index>     -d <dir>
 *   usbutils dnas list                  -p <pkg.dni>
 *   usbutils dnas patch <index>         -d <dir> -p <pkg.dni> --dna <pkg.dna> -v <ver>
 *   usbutils drives
 *   usbutils id -n "Name"               (show computed game id)
 *   usbutils find <pattern>             -d <dir> [--size N]
 *   usbutils files <index>              -d <dir>
 *   usbutils extract <index> --path <p> -d <dir> [-o <outdir|file>]
 *   usbutils inject <index> --path <p>  -d <dir> --file <replacement>
 *   usbutils copy <index|all> --to <d>  -d <dir>
 *   usbutils move <index|all> --to <d>  -d <dir>
 *   usbutils report                     -d <dir> [-o <file>]
 *   usbutils fix                        -d <dir> [--apply]
 *   usbutils check-update
 */
#include "../core/ulcfg.h"
#include "../core/installer.h"
#include "../core/gamefile.h"
#include "../core/gamestream.h"
#include "../core/ulid.h"
#include "../core/recovery.h"
#include "../core/dnas.h"
#include "../core/videorip.h"
#include "../core/ps2load.h"
#include "../core/gameops.h"
#include "../core/gamedb.h"
#include "../core/usbdevice.h"
#include "../core/update.h"
#include "../core/isoedit.h"
#include "../core/supportfiles.h"

#ifndef USBUTILS_VERSION
#define USBUTILS_VERSION "0.0.1"
#endif

#include <cstdio>
#include <cstring>
#include <cctype>
#include <string>
#include <vector>
#include <map>
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
        "  usbutils ps2load <index>       -d <dir>   (show injection status)\n"
        "  usbutils ps2load inject <index> -d <dir> [--elf <PS2LOAD.ELF>]\n"
        "  usbutils ps2load remove <index> -d <dir>   (undo injection)\n"
        "  usbutils id                   -n \"Name\"   (show computed game id)\n"
        "  usbutils drives\n"
        "  usbutils dnas list            -p <usbutil.dni>\n"
        "  usbutils dnas patch <index>   -d <dir> -p <usbutil.dni> --dna <usbutil.dna> -v <ver>\n"
        "  usbutils find <pattern>       -d <dir> [--size N]   (wildcards * ?; searches\n"
        "                                                     every game's file list)\n"
        "  usbutils files <index>        -d <dir>              (list in-game files)\n"
        "  usbutils extract <index> --path <p> [-o out] -d <dir>  (pull one file out)\n"
        "  usbutils inject <index> --path <p> --file <f> -d <dir>  (same-size replace)\n"
        "  usbutils copy <index|all> --to <dir> -d <dir>  (copy games to another drive)\n"
        "  usbutils move <index|all> --to <dir> -d <dir>  (move games to another drive)\n"
        "  usbutils report               -d <dir> [-o <file>] (drive report)\n"
        "  usbutils sort                  -d <dir>   (sort games A-Z, save ul.cfg)\n"
        "  usbutils clean                 -d <dir> [--apply]   (find left-over\n"
        "                                                     files no game uses;\n"
        "                                                     --apply deletes them)\n"
        "  usbutils fix                  -d <dir> [--apply]   (correct code 666.66)\n"
        "  usbutils iso info <image.iso>               (image summary)\n"
        "  usbutils iso list <image.iso>               (file tree)\n"
        "  usbutils iso extract <image.iso> --path <p> [-o out]\n"
        "  usbutils support                            (check the shipped\n"
        "                                              data files)\n"
        "  usbutils check-update                                (look for a newer release)\n"
        "  usbutils --version\n"
        "\n"
        "install also accepts a device path (e.g. /dev/sr0 on Linux) to\n"
        "create a game straight from a PS2 disc.\n", USBUTILS_VERSION);
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
    std::string cmd, sub, dir, iso, name, out, dni, dna, ver, only, elf;
    std::string pattern, dest, path, file;
    uint64_t    sizeFilter = 0;
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
        else if (s == "--elf") a.elf = next();
        else if (s == "--to") a.dest = next();
        else if (s == "--path") a.path = next();
        else if (s == "--file") a.file = next();
        else if (s == "--size") a.sizeFilter = std::strtoull(next().c_str(), nullptr, 10);
        else if (a.cmd == "ps2load" && a.sub.empty()
                 && (s == "inject" || s == "remove" || s == "status"))
            a.sub = s;
        else if (a.cmd == "iso" && a.sub.empty()
                 && (s == "info" || s == "list" || s == "extract"))
            a.sub = s;
        else if (s == "--all") a.all = true;
        else if (s == "--apply") a.apply = true;
        else if (s == "--no-backup") a.noBackup = true;
        else if (s == "all") a.all = true;
        else if (std::isdigit(static_cast<unsigned char>(s[0])) && s.find_first_not_of("0123456789") == std::string::npos)
            a.index = std::atoi(s.c_str());
        else if (a.iso.empty() && (a.cmd == "install" || a.cmd == "iso")) a.iso = s;
        else if (a.pattern.empty() && a.cmd == "find") a.pattern = s;
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

static int cmdSort(const Args& a) {
    if (a.dir.empty()) { usage(); return 2; }
    size_t moved = 0;
    if (!sortGames(a.dir, &moved)) {
        std::printf("ERROR: could not write ul.cfg\n");
        return 1;
    }
    if (moved == 0) std::printf("already sorted\n");
    else            std::printf("sorted, %zu record(s) moved\n", moved);
    return 0;
}

static int cmdClean(const Args& a) {
    if (a.dir.empty()) { usage(); return 2; }
    const DriveSpaceReport rep = scanDriveSpace(a.dir);
    std::printf("games on drive        : %zu\n", rep.games);
    std::printf("files used by games   : %zu (%s)\n",
                rep.gameFiles, humanSize(rep.gameBytes).c_str());
    std::printf("obsolete files        : %zu (%s)\n",
                rep.obsoleteFiles, humanSize(rep.obsoleteBytes).c_str());
    std::printf("files with size zero  : %zu\n", rep.zeroSizeFiles);
    std::printf("current free space    : %s\n", humanSize(rep.freeBytes).c_str());
    if (!rep.obsolete.empty()) {
        std::printf("\nleft-over files no game uses:\n");
        for (const std::string& n : rep.obsolete)
            std::printf("  %s\n", n.c_str());
        if (!a.apply) {
            std::printf("\n(dry run - run again with --apply to delete them)\n");
            return 1;
        }
        const int n = restoreSpace(a.dir, rep);
        std::printf("\ndeleted %d file(s), %s reclaimed\n",
                    n, humanSize(rep.obsoleteBytes).c_str());
        return n == static_cast<int>(rep.obsoleteFiles) ? 0 : 1;
    }
    std::printf("\nnothing to clean\n");
    return 0;
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
    const std::vector<UsbDrive> drives = listDrives();
    for (const UsbDrive& d : drives) {
        std::printf("%-24s %-16s %10s free  %s%s\n", d.root.c_str(),
                    d.label.substr(0, 16).c_str(),
                    humanSize(d.freeBytes).c_str(),
                    d.fat32 ? "FAT32 " : "",
                    d.hasUlCfg ? "[ul.cfg]" : "");
    }
    if (drives.empty())
        std::puts("No removable drives found. Plug in a USB drive and try again.");
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

static int cmdPs2Load(const Args& a) {
    UlCfg cfg;
    size_t index = 0;
    std::string id;
    if (!resolveGame(a, cfg, index, id)) return 2;
    const UlCfgRecord& r = cfg.records[index];

    if (a.sub == "inject") {
        std::string elf = a.elf;
        if (elf.empty()) {
            for (const char* cand : {"PS2LOAD.ELF", "data/PS2LOAD.ELF"}) {
                FILE* f = std::fopen(cand, "rb");
                if (f) { std::fclose(f); elf = cand; break; }
            }
        }
        if (elf.empty()) {
            std::printf("ERROR: PS2LOAD.ELF not found next to the tool; "
                        "pass it with --elf (shipped in data/)\n");
            return 1;
        }
        std::printf("injecting from %s ...\n", elf.c_str());
        const Ps2LoadResult res =
            injectPs2Load(a.dir, id, r.serialStr(), r.parts, elf,
                          makeProgress());
        std::printf("\n");
        if (!res.ok && res.appendedBytes == 0) {
            std::printf("ERROR: %s\n", res.error.c_str());
            return 1;
        }
        if (!res.error.empty())
            std::printf("NOTE: %s\n", res.error.c_str());
        std::printf("boot record '%s;1' injected (%s appended)\n",
                    res.ps2lName.c_str(), humanSize(res.appendedBytes).c_str());
        return 0;
    }
    if (a.sub == "remove") {
        const Ps2LoadResult res =
            removePs2Load(a.dir, id, r.serialStr(), r.parts, makeProgress());
        if (!res.ok) { std::printf("ERROR: %s\n", res.error.c_str()); return 1; }
        std::printf("PS2Load removed, SYSTEM.CNF restored\n");
        return 0;
    }

    // status
    const Ps2LoadInfo info = queryPs2Load(a.dir, id, r.serialStr(), r.parts);
    if (!info.error.empty()) {
        std::printf("ERROR: %s\n", info.error.c_str());
        return 1;
    }
    switch (info.state) {
    case Ps2LoadState::NotInjected:
        std::printf("not injected (boot ELF '%s' -> would become '%s')\n",
                    info.bootElf.c_str(), info.ps2lName.c_str());
        break;
    case Ps2LoadState::InjectedByTool:
        std::printf("injected by the tool (boot '%s') - removable\n",
                    info.ps2lName.c_str());
        break;
    case Ps2LoadState::InjectedManually:
        std::printf("injected MANUALLY (marker LBA %u) - not removable here\n",
                    info.markerLba);
        break;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// gameops commands: find / files / extract / inject / copy / move / report / fix
// ---------------------------------------------------------------------------

static int cmdFind(const Args& a) {
    if (a.dir.empty()) { usage(); return 2; }
    UlCfg cfg;
    if (!cfg.load(a.dir)) { std::printf("ERROR: no ul.cfg in %s\n", a.dir.c_str()); return 1; }

    const auto hits = findFileInGames(a.dir, cfg, a.pattern, a.sizeFilter);
    if (hits.empty()) {
        std::printf("no matching files in %zu game(s)\n", cfg.records.size());
        return 0;
    }
    std::printf("%-32s %-40s %12s\n", "GAME", "FILE", "SIZE");
    for (const FileHit& h : hits)
        std::printf("%-32s %-40s %12s\n",
                    h.gameName.substr(0, 32).c_str(),
                    h.entry.path.substr(0, 40).c_str(),
                    humanSize(h.entry.size).c_str());
    std::printf("%zu file(s)\n", hits.size());
    return 0;
}

static int cmdFiles(const Args& a) {
    UlCfg cfg;
    size_t index = 0;
    std::string id;
    if (!resolveGame(a, cfg, index, id)) return 2;

    std::string err;
    const auto files = listGameFiles(a.dir, id, cfg.records[index].serialStr(),
                                     cfg.records[index].parts, &err);
    if (!err.empty()) { std::printf("ERROR: %s\n", err.c_str()); return 1; }
    std::printf("%-52s %12s %10s\n", "FILE", "SIZE", "LBA");
    for (const IsoEntry& e : files)
        std::printf("%-52s %12s %10u\n", e.path.substr(0, 52).c_str(),
                    humanSize(e.size).c_str(), e.lba);
    std::printf("%zu file(s)\n", files.size());
    return 0;
}

static int cmdExtract(const Args& a) {
    UlCfg cfg;
    size_t index = 0;
    std::string id;
    if (!resolveGame(a, cfg, index, id)) return 2;
    if (a.path.empty()) { usage(); return 2; }

    std::string err;
    const auto files = listGameFiles(a.dir, id, cfg.records[index].serialStr(),
                                     cfg.records[index].parts, &err);
    if (!err.empty()) { std::printf("ERROR: %s\n", err.c_str()); return 1; }

    // wildcard selection over the path / base name
    std::vector<IsoEntry> selection;
    for (const IsoEntry& e : files)
        if (matchPattern(e.name, a.path) || matchPattern(e.path, a.path))
            selection.push_back(e);
    if (selection.empty()) {
        std::printf("ERROR: no file matching \"%s\"; run `usbutils files %d -d %s`\n",
                    a.path.c_str(), a.index, a.dir.c_str());
        return 1;
    }

    const bool many = selection.size() > 1;
    if (many && a.out.empty()) {
        std::printf("ERROR: %zu files match; pass an output directory with -o\n",
                    selection.size());
        return 1;
    }
    std::string outDir = a.out;
    if (many) {
        std::error_code ec;
        std::filesystem::create_directories(outDir, ec);
        if (ec) { std::printf("ERROR: cannot create %s\n", outDir.c_str()); return 1; }
    }

    bool ok = true;
    for (const IsoEntry& e : selection) {
        std::string outPath = a.out;
        if (many) {
            std::string name = e.name;
            for (char& c : name) if (c == '\\' || c == '/') c = '_';
            outPath = (outDir.back() == '/' || outDir.back() == '\\')
                          ? outDir + name : outDir + "/" + name;
        } else if (outPath.empty()) {
            outPath = e.name;                  // single match: default name
        }
        std::printf("extracting %s ...\n", e.path.c_str());
        if (!extractGameFile(a.dir, id, cfg.records[index].serialStr(),
                             cfg.records[index].parts, e, outPath,
                             makeProgress())) {
            std::printf("\nERROR: extract failed for %s\n", e.path.c_str());
            ok = false;
        } else {
            std::printf("\n  -> %s\n", outPath.c_str());
        }
    }
    return ok ? 0 : 1;
}

static int cmdInject(const Args& a) {
    UlCfg cfg;
    size_t index = 0;
    std::string id;
    if (!resolveGame(a, cfg, index, id)) return 2;
    if (a.path.empty() || a.file.empty()) { usage(); return 2; }

    const InjectResult r = injectGameFile(a.dir, id,
                                          cfg.records[index].serialStr(),
                                          cfg.records[index].parts,
                                          a.path, a.file, makeProgress());
    std::printf("\n");
    if (!r.ok) { std::printf("ERROR: %s\n", r.error.c_str()); return 1; }
    std::printf("replaced %s (%s)\n", r.path.c_str(), humanSize(r.bytes).c_str());
    return 0;
}

static int cmdCopyMove(const Args& a, bool move) {
    if (a.dir.empty() || a.dest.empty() || (a.index < 0 && !a.all)) { usage(); return 2; }
    UlCfg cfg;
    if (!cfg.load(a.dir)) { std::printf("ERROR: no ul.cfg\n"); return 1; }
    if (a.index >= (int)cfg.records.size() && !a.all) { std::printf("ERROR: bad index\n"); return 1; }

    // collect indices (all, or the single one) — collected up front
    // because a move invalidates positions
    std::vector<size_t> indices;
    if (a.all)
        for (size_t i = 0; i < cfg.records.size(); ++i) indices.push_back(i);
    else
        indices.push_back(static_cast<size_t>(a.index));

    bool ok = true;
    size_t done = 0;
    for (size_t idx : indices) {
        const size_t cur = move ? idx - done : idx;   // records shift after a move
        if (cur >= cfg.records.size()) { ok = false; break; }
        const std::string name = cfg.records[cur].nameStr();
        std::printf("%s \"%s\" ...\n", move ? "moving" : "copying", name.c_str());
        const CopyResult r = copyGame(a.dir, cfg, cur, a.dest, move, makeProgress());
        std::printf("\n");
        if (!r.ok) {
            std::printf("ERROR: %s\n", r.error.c_str());
            ok = false;
            break;
        }
        ++done;
        std::printf("  %zu/%zu done (%s, %d file(s))\n", done, indices.size(),
                    humanSize(r.copiedBytes).c_str(), r.filesWritten);
    }
    if (ok && !indices.empty())
        std::printf("%s %zu game(s) to %s\n", move ? "moved" : "copied",
                    indices.size(), a.dest.c_str());
    return ok ? 0 : 1;
}

static int cmdReport(const Args& a) {
    if (a.dir.empty()) { usage(); return 2; }
    UlCfg cfg;
    if (!cfg.load(a.dir)) { std::printf("ERROR: no ul.cfg in %s\n", a.dir.c_str()); return 1; }

    const std::string rep = generateReport(a.dir, cfg);
    std::fputs(rep.c_str(), stdout);

    if (!a.out.empty()) {
        FILE* f = std::fopen(a.out.c_str(), "wb");
        if (!f) { std::printf("ERROR: cannot write %s\n", a.out.c_str()); return 1; }
        std::fwrite(rep.data(), 1, rep.size(), f);
        std::fclose(f);
        std::printf("\nreport saved to %s\n", a.out.c_str());
    }
    return 0;
}

static int cmdFix(const Args& a) {
    if (a.dir.empty()) { usage(); return 2; }
    UlCfg cfg;
    if (!cfg.load(a.dir)) { std::printf("ERROR: no ul.cfg in %s\n", a.dir.c_str()); return 1; }

    const auto fixes = fixBrokenSerials(a.dir, cfg, a.apply);
    if (fixes.empty()) { std::printf("no broken serial codes found\n"); return 0; }

    for (const SerialFix& f : fixes) {
        std::printf("[%zu] %-32s %s -> %s%s\n", f.index, f.gameName.c_str(),
                    f.oldSerial.c_str(),
                    f.newSerial.empty() ? "(unknown - not fixable)" : f.newSerial.c_str(),
                    f.applied ? "  [fixed]" : "");
    }
    if (!a.apply)
        std::printf("run with --apply to fix the records and rename the chunk files\n");
    else {
        int failed = 0;
        for (const SerialFix& f : fixes)
            if (!f.applied && !f.newSerial.empty()) ++failed;
        if (failed) std::printf("WARNING: %d fix(es) could not be applied\n", failed);
    }
    return 0;
}

static int cmdCheckUpdate() {
    std::printf("Checking for updates...\n");
    const ReleaseInfo rel = fetchLatestRelease();
    if (!rel.ok) {
        std::printf("Could not check for updates: %s\n", rel.error.c_str());
        return 1;
    }
    if (isVersionNewer(USBUTILS_VERSION, rel.tag)) {
        const std::string tag = !rel.tag.empty() && rel.tag[0] == 'v'
                                    ? rel.tag.substr(1) : rel.tag;
        std::printf("A new version is available: %s (you have %s)\n"
                    "Get it at: %s\n",
                    tag.c_str(), USBUTILS_VERSION, rel.url.c_str());
        return 0;
    }
    std::printf("You are up to date. USBUtils %s is the latest version.\n",
                USBUTILS_VERSION);
    return 0;
}

// ---------------------------------------------------------------------------
// iso — "Open ISO to [Explore/Modify]" (read-only view in the CLI; the GUI
// has the full modify workflow)
// ---------------------------------------------------------------------------
static int cmdIso(const Args& a) {
    if (a.iso.empty()) { usage(); return 2; }
    const std::string& sub = a.sub.empty() ? "info" : a.sub;

    IsoImageDoc doc;
    if (!doc.open(a.iso)) {
        std::printf("ERROR: %s is not an ISO9660 image\n", a.iso.c_str());
        return 1;
    }

    if (sub == "info") {
        const IsoInfo info = readIsoInfo(a.iso);
        std::printf("image:        %s\n", a.iso.c_str());
        std::printf("volume id:    %s\n", doc.volumeId().c_str());
        std::printf("image size:   %s\n", humanSize(doc.originalSize()).c_str());
        if (info.valid) {
            std::printf("serial:       %s  (PS2 game)\n", info.serial.c_str());
            std::printf("boot file:    %s\n", info.bootElf.c_str());
        } else {
            std::printf("serial:       (no SYSTEM.CNF - not a PS2 game)\n");
        }
        size_t files = 0, dirs = 0;
        doc.forEachNode([&](const IsoNode* n, int) {
            if (n == doc.root()) return;
            if (n->isDir) ++dirs; else ++files;
        });
        std::printf("folders:      %zu\nfiles:        %zu\n", dirs, files);
        return 0;
    }

    if (sub == "list") {
        doc.forEachNode([&](const IsoNode* n, int depth) {
            if (n == doc.root()) return;
            for (int i = 0; i < depth - 1; ++i) std::printf("  ");
            std::printf("%s%-14s %10s%s\n",
                        n->isDir ? "[DIR] " : "      ",
                        n->name.c_str(),
                        n->isDir ? "" : humanSize(n->size()).c_str(),
                        n->isExternal() ? "   (added)" : "");
        });
        return 0;
    }

    if (sub == "extract") {
        if (a.path.empty()) {
            std::printf("ERROR: pass --path <file-in-image> (see `usbutils iso list`)\n");
            return 2;
        }
        // build node -> full-path map, then wildcard-match over it
        std::map<const IsoNode*, std::string> paths;
        std::function<void(const IsoNode*, const std::string&)> collect =
            [&](const IsoNode* n, const std::string& prefix) {
            for (const auto& c : n->children) {
                const std::string p = prefix.empty()
                    ? c->name : prefix + "\\" + c->name;
                paths[c.get()] = p;
                if (c->isDir) collect(c.get(), p);
            }
        };
        collect(doc.root(), "");

        std::vector<const IsoNode*> selection;
        for (const auto& [n, p] : paths) {
            if (n->isDir) continue;
            if (matchPattern(n->name, a.path) || matchPattern(p, a.path))
                selection.push_back(n);
        }
        if (selection.empty()) {
            std::printf("ERROR: no file matching \"%s\"\n", a.path.c_str());
            return 1;
        }
        const bool many = selection.size() > 1;
        if (many && a.out.empty()) {
            std::printf("ERROR: %zu files match; pass an output directory with -o\n",
                        selection.size());
            return 1;
        }
        std::error_code ec;
        if (many) std::filesystem::create_directories(a.out, ec);
        bool ok = true;
        for (const IsoNode* n : selection) {
            std::string outPath = a.out;
            if (many) {
                std::string name = n->name;
                outPath = (outPath.back() == '/' || outPath.back() == '\\')
                              ? outPath + name : outPath + "/" + name;
            } else if (outPath.empty()) {
                outPath = n->name;
            } else {
                // single match with -o: an existing directory means
                // "put the file inside it"
                std::error_code ec2;
                if (std::filesystem::is_directory(outPath, ec2)) {
                    outPath = (outPath.back() == '/' || outPath.back() == '\\')
                                  ? outPath + n->name
                                  : outPath + "/" + n->name;
                }
            }
            std::printf("extracting %s ...\n", n->name.c_str());
            if (!doc.extractTo(n, outPath)) {
                std::printf("ERROR: extract failed for %s\n", n->name.c_str());
                ok = false;
            } else {
                std::printf("  -> %s (%s)\n", outPath.c_str(),
                            humanSize(n->size()).c_str());
            }
        }
        return ok ? 0 : 1;
    }

    usage();
    return 2;
}

// ---------------------------------------------------------------------------
// support — "Useful acquired" from the command line
// ---------------------------------------------------------------------------
static int cmdSupport() {
    const auto st = checkSupportFiles();
    size_t found = 0;
    std::printf("Support files (searched next to the program, in ./data and "
                "$USBUTILS_DATA):\n\n");
    for (const SupportStatus& s : st) {
        if (s.found) ++found;
        std::printf("  %-22s %-13s %s\n",
                    s.file.fileName,
                    s.found ? (s.verified ? "found (orig)" : "found") : "MISSING",
                    s.found ? humanSize(s.size).c_str()
                            : (s.file.required ? "required!" : "optional"));
    }
    std::printf("\n%zu/%zu present\n", found, st.size());
    for (const SupportStatus& s : st) {
        if (!s.found && s.file.required)
            std::printf("  missing %s breaks: %s\n", s.file.fileName, s.file.feature);
    }
    return found == st.size() ? 0 : 1;
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
    if (a.cmd == "ps2load") return cmdPs2Load(a);
    if (a.cmd == "find")    return cmdFind(a);
    if (a.cmd == "files")   return cmdFiles(a);
    if (a.cmd == "extract") return cmdExtract(a);
    if (a.cmd == "inject")  return cmdInject(a);
    if (a.cmd == "copy")    return cmdCopyMove(a, false);
    if (a.cmd == "move")    return cmdCopyMove(a, true);
    if (a.cmd == "report")  return cmdReport(a);
    if (a.cmd == "sort")    return cmdSort(a);
    if (a.cmd == "clean" || a.cmd == "restore-space")
        return cmdClean(a);
    if (a.cmd == "fix")     return cmdFix(a);
    if (a.cmd == "iso")     return cmdIso(a);
    if (a.cmd == "support") return cmdSupport();
    if (a.cmd == "check-update" || a.cmd == "update-check")
        return cmdCheckUpdate();
    if (a.cmd == "id") {
        if (a.name.empty()) { usage(); return 2; }
        std::printf("%s\n", gameIdHex(a.name).c_str());
        return 0;
    }
    usage();
    return 2;
}
