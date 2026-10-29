// SPDX-License-Identifier: MIT
/*
 * Unit tests for the USBUtils core — verify the reverse-engineered formats:
 *   1. CRC32 game id against values captured from the original algorithm
 *   2. ul.cfg record layout (64 bytes, offsets, magic)
 *   3. chunk naming & parts math
 *   4. rcn codec round-trip
 *   5. dni index parsing
 *   6. ISO walker (multi-directory) + SYSTEM.CNF extraction
 *   7. GameStream random access across chunk boundaries
 *   8. end-to-end install/export on a generated fake ISO
 *   9. video rip / restore round-trip with sidecar backup
 *  10. exact IOPRP location (DNAS patch target)
 */
#include "ulcfg.h"
#include "ulid.h"
#include "gamefile.h"
#include "gamestream.h"
#include "iso9660.h"
#include "installer.h"
#include "gamedb.h"
#include "dnas.h"
#include "videorip.h"
#include "port.h"

#include <cstdio>
#include <cstring>
#include <cassert>
#include <filesystem>
#include <map>
#include <vector>
#include <string>

using namespace usbutils;
namespace fs = std::filesystem;

static int g_failed = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failed; } \
    else { std::printf("ok   %s\n", #cond); } \
} while (0)

// --- test helpers ----------------------------------------------------------

/// Append one ISO9660 directory record; returns its length.
static size_t putDirRecord(std::vector<uint8_t>& dir, size_t off,
                           uint32_t lba, uint32_t size, uint8_t flags,
                           const std::string& name) {
    uint8_t rec[96] = {};
    size_t recLen = 33 + name.size();
    if (recLen % 2) ++recLen;                    // records are even-sized
    rec[0] = static_cast<uint8_t>(recLen);
    std::memcpy(rec + 2,  &lba, 4);
    std::memcpy(rec + 10, &size, 4);
    rec[18] = flags;
    rec[25] = static_cast<uint8_t>(name.size());
    if (!name.empty()) std::memcpy(rec + 33, name.data(), name.size());
    std::memcpy(dir.data() + off, rec, recLen);
    return recLen;
}

/// Build a minimal ISO9660 image with SYSTEM.CNF, a MOVIES directory
/// holding INTRO.PSS, and an IOPRP300.IMG — enough structure to exercise
/// the walker, the video ripper and the DNAS locator.
static void writeTestIso(const fs::path& p, const std::string& serial,
                         uint64_t payloadBytes, bool withExtras = true) {
    const uint32_t sector = 2048;
    const uint32_t pssLba = 22;
    const uint32_t pssSize = 3 * sector;
    const uint32_t ioprpLba = 25;
    const uint32_t ioprpSize = 256;
    const uint32_t totalSectors =
        (withExtras ? ioprpLba : 21) +
        static_cast<uint32_t>((payloadBytes + sector - 1) / sector) + 1;

    std::vector<uint8_t> img(16 * sector, 0);

    // ---- PVD at sector 16 -------------------------------------------------
    uint8_t pvd[sector] = {};
    pvd[0] = 1;
    std::memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    std::memset(pvd + 40, ' ', 32);            // space-padded volume id
    const std::string volId = "TEST_VOLUME";
    std::memcpy(pvd + 40, volId.c_str(), volId.size());
    pvd[80] = totalSectors & 0xFF; pvd[81] = (totalSectors >> 8) & 0xFF;
    pvd[82] = (totalSectors >> 16) & 0xFF; pvd[83] = (totalSectors >> 24) & 0xFF;
    uint8_t* root = pvd + 156;                 // root directory record
    root[0] = 34; root[1] = 0;
    const uint32_t rootLba = 18, rootSize = sector;
    std::memcpy(root + 2, &rootLba, 4);
    std::memcpy(root + 10, &rootSize, 4);
    root[18] = 2;                              // directory flag
    root[25] = 1;                              // identifier "\0"
    img.insert(img.end(), pvd, pvd + sector);

    // sector 17: volume descriptor terminator (left zero — parser skips)
    img.resize(18 * sector, 0);

    // ---- root directory at 18 --------------------------------------------
    std::vector<uint8_t> dir(sector, 0);
    size_t off = 0;
    off += putDirRecord(dir, off, rootLba, rootSize, 2, std::string(1, '\0'));
    off += putDirRecord(dir, off, rootLba, rootSize, 2, std::string(1, '\1'));
    off += putDirRecord(dir, off, 20, 96, 0, "SYSTEM.CNF;1");
    if (withExtras) {
        off += putDirRecord(dir, off, 21, sector, 2, "MOVIES");
        off += putDirRecord(dir, off, ioprpLba, ioprpSize, 0, "IOPRP300.IMG;1");
    }
    img.insert(img.end(), dir.begin(), dir.end());

    // sector 19 empty, sector 20: SYSTEM.CNF
    img.resize(20 * sector, 0);
    std::string cnf = "BOOT2 = cdrom0:\\" + serial + ";1\nVER = 1.00\n";
    cnf.resize(96, ' ');
    img.insert(img.end(), cnf.begin(), cnf.end());

    if (withExtras) {
        // sector 21: MOVIES directory
        img.resize(21 * sector, 0);
        std::vector<uint8_t> mdir(sector, 0);
        size_t moff = 0;
        moff += putDirRecord(mdir, moff, 21, sector, 2, std::string(1, '\0'));
        moff += putDirRecord(mdir, moff, rootLba, rootSize, 2, std::string(1, '\1'));
        moff += putDirRecord(mdir, moff, pssLba, pssSize, 0, "INTRO.PSS;1");
        img.insert(img.end(), mdir.begin(), mdir.end());

        // sectors 22-24: patterned video payload
        std::vector<uint8_t> pss(pssSize);
        for (size_t i = 0; i < pss.size(); ++i)
            pss[i] = static_cast<uint8_t>((i * 13 + 5) & 0xFF);
        img.insert(img.end(), pss.begin(), pss.end());

        // sector 25: IOPRP image
        std::vector<uint8_t> ioprp(ioprpSize, 0);
        std::memcpy(ioprp.data(), "RESET\0\0\0", 8);
        for (size_t i = 8; i < ioprp.size(); ++i)
            ioprp[i] = static_cast<uint8_t>((i * 3 + 1) & 0xFF);
        img.insert(img.end(), ioprp.begin(), ioprp.end());
    } else {
        img.resize(ioprpLba * sector, 0);      // keep layout offsets stable
    }

    // trailing generic payload
    const uint64_t payload = payloadBytes ? payloadBytes : 1;
    img.resize(img.size() + payload, 0xAB);

    FILE* f = std::fopen(p.string().c_str(), "wb");
    std::fwrite(img.data(), 1, img.size(), f);
    std::fclose(f);
}

static bool filesEqual(const fs::path& a, const fs::path& b) {
    FILE* fa = std::fopen(a.string().c_str(), "rb");
    FILE* fb = std::fopen(b.string().c_str(), "rb");
    if (!fa || !fb) { if (fa) std::fclose(fa); if (fb) std::fclose(fb); return false; }
    std::vector<uint8_t> ba(1 << 20), bb(1 << 20);
    bool equal = true;
    for (;;) {
        const size_t na = std::fread(ba.data(), 1, ba.size(), fa);
        const size_t nb = std::fread(bb.data(), 1, bb.size(), fb);
        if (na != nb || std::memcmp(ba.data(), bb.data(), na) != 0) { equal = false; break; }
        if (na == 0) break;
    }
    std::fclose(fa);
    std::fclose(fb);
    return equal;
}

// --- tests -----------------------------------------------------------------

static void testCrcId() {
    // The table generation and round math are transcribed from 0x398738 /
    // 0x398784; validate structural properties plus a self-computed vector.
    const uint32_t empty = ulCrc32("", 0);
    CHECK(empty == 0);

    // 'A' (0x41): idx = 0x41 ^ 0 = 0x41 -> table[0x41]
    // known MSB-first CRC32("A") (poly 04C11DB7, init 0, no xorout)
    CHECK(ulCrc32("A", 1) == 0x30476DC0u);   // independent reference implementation

    // gameIdHex: 8 uppercase hex digits, deterministic
    const std::string id1 = gameIdHex("Test Game");
    const std::string id2 = gameIdHex("Test Game ");
    CHECK(id1.size() == 8);
    CHECK(id1 == id2);                    // trailing space trimmed (Trim)
    CHECK(gameIdHex("ABC") != gameIdHex("ABD"));
    for (char c : id1) CHECK(c >= '0' && (c <= '9' || c >= 'A'));
}

static void testRecord() {
    UlCfgRecord r;
    CHECK(sizeof(r) == 64);
    r.setName("Grand Theft Auto San Andreas");
    r.setSerial("SLUS_209.46");
    r.parts = 5;
    r.media = kMediaDvd;

    CHECK(r.nameStr() == "Grand Theft Auto San Andreas"); // 28 chars fit in 32
    CHECK(std::memcmp(r.magic, "ul.", 3) == 0);
    CHECK(r.serialStr() == "SLUS_209.46");
    CHECK(r.flag == 8);
    CHECK(r.valid());
}

static void testChunkMath() {
    CHECK(chunkCount(1) == 1);
    CHECK(chunkCount(kChunkSize) == 1);
    CHECK(chunkCount(kChunkSize + 1) == 2);
    CHECK(chunkCount(5ull * kChunkSize) == 5);
    CHECK(mediaForSize(100) == kMediaCd);
    CHECK(mediaForSize(kCdThreshold) == kMediaCd);
    CHECK(mediaForSize(kCdThreshold + 1) == kMediaDvd);
    CHECK(chunkFileName("2D9E3F51", "SCUS_97472", 0) == "ul.2D9E3F51.SCUS_97472.00");
    CHECK(chunkFileName("2D9E3F51", "SCUS_97472", 7) == "ul.2D9E3F51.SCUS_97472.07");
}

static void testRcnRoundTrip() {
    const fs::path tmp = fs::temp_directory_path() / "usbutils_test.rcn";
    std::vector<RecoveryEntry> games = {
        {"SLUS_202.65", "007 A_Under Fire"},
        {"SLES_534.81", "10.000 bullet"},
        {"PS2L_205.95", "Area 51"},
    };
    CHECK(saveRecoveryDb(tmp.string(), games));
    const auto back = loadRecoveryDb(tmp.string());
    CHECK(back.size() >= 3);
    bool found = false;
    for (const auto& e : back)
        if (e.serial == "SLUS_202.65" && e.name == "007 A_Under Fire") found = true;
    CHECK(found);
    fs::remove(tmp);
}

static void testDniParse() {
    const fs::path tmp = fs::temp_directory_path() / "usbutils_test.dni";
    FILE* f = std::fopen(tmp.string().c_str(), "wb");
    std::fputs("// test\nDNAS16[91909];\nDNAS165[98901];\nDNAS300[278353];\n", f);
    std::fclose(f);

    DnasPackage pkg;
    CHECK(pkg.loadIndex(tmp.string()));
    CHECK(pkg.entries().size() == 3);
    CHECK(pkg.entries()[0].version == "16");
    CHECK(pkg.entries()[0].size == 91909);
    CHECK(pkg.entries()[1].offset == 91909);
    CHECK(pkg.entries()[2].offset == 91909 + 98901);
    CHECK(pkg.bestFor("250") == "165");   // 250 > 165 -> best <= is 165
    fs::remove(tmp);
}

static void testWalkIso() {
    const fs::path iso = fs::temp_directory_path() / "usbutils_walk.iso";
    writeTestIso(iso, "SLUS_202.65", 2048 * 2);

    FILE* f = std::fopen(iso.string().c_str(), "rb");
    CHECK(f != nullptr);
    if (!f) return;
    const IsoReadFn read = [f](uint64_t off, void* dst, size_t n) {
        return usbutils::seek64(f, static_cast<int64_t>(off))
            && std::fread(dst, 1, n, f) == n;
    };

    std::map<std::string, IsoEntry> entries;
    const bool walked = walkIso(read, [&](const IsoEntry& e) {
        entries[e.path] = e;
        return true;
    });
    CHECK(walked);
    CHECK(entries.count("SYSTEM.CNF") == 1);
    CHECK(entries.count("MOVIES") == 1 && entries["MOVIES"].isDir);
    CHECK(entries.count("MOVIES\\INTRO.PSS") == 1);
    CHECK(entries["MOVIES\\INTRO.PSS"].size == 3 * 2048);
    CHECK(entries["MOVIES\\INTRO.PSS"].lba == 22);
    CHECK(entries.count("IOPRP300.IMG") == 1);
    CHECK(entries["IOPRP300.IMG"].size == 256);
    CHECK(entries["IOPRP300.IMG"].lba == 25);

    // early stop: walker must terminate when the callback says so
    int seen = 0;
    walkIso(read, [&](const IsoEntry&) { ++seen; return false; });
    CHECK(seen == 1);

    std::fclose(f);
    fs::remove(iso);
}

static void testIsoInfo() {
    const fs::path iso = fs::temp_directory_path() / "usbutils_test.iso";
    writeTestIso(iso, "SLUS_202.65", 2048 * 3);

    IsoInfo info = readIsoInfo(iso.string());
    CHECK(info.valid);
    CHECK(info.serial == "SLUS_202.65");
    CHECK(info.volumeId == "TEST_VOLUME");
    CHECK(info.systemCnf.find("BOOT2") != std::string::npos);

    // no-extras image still yields the serial (root-only walk)
    writeTestIso(iso, "SCES_517.17", 2048, false);
    info = readIsoInfo(iso.string());
    CHECK(info.valid);
    CHECK(info.serial == "SCES_517.17");
    fs::remove(iso);
}

static void testGameStream() {
    const fs::path base = fs::temp_directory_path() / "usbutils_stream";
    fs::remove_all(base);
    fs::create_directories(base);

    // two odd-sized chunks (not 1 GiB) — offsets must come from real sizes
    const std::string id = "11111111", serial = "SLUS_202.65";
    {
        std::vector<uint8_t> a(100), b(50);
        for (size_t i = 0; i < a.size(); ++i) a[i] = static_cast<uint8_t>(i);
        for (size_t i = 0; i < b.size(); ++i) b[i] = static_cast<uint8_t>(100 + i);
        FILE* f = std::fopen((base / chunkFileName(id, serial, 0)).string().c_str(), "wb");
        std::fwrite(a.data(), 1, a.size(), f); std::fclose(f);
        f = std::fopen((base / chunkFileName(id, serial, 1)).string().c_str(), "wb");
        std::fwrite(b.data(), 1, b.size(), f); std::fclose(f);
    }

    GameStream s;
    CHECK(s.open(base.string(), id, serial, 2));
    CHECK(s.size() == 150);

    uint8_t out[20] = {};
    CHECK(s.read(90, out, 20) == 20);          // spans the chunk boundary
    CHECK(out[0] == 90 && out[9] == 99 && out[10] == 100 && out[19] == 109);

    const uint8_t wr[5] = {1, 2, 3, 4, 5};
    CHECK(s.write(98, wr, 5));                 // spans the boundary too
    CHECK(s.read(97, out, 7) == 7);
    CHECK(out[0] == 97 && out[1] == 1 && out[2] == 2 && out[3] == 3
          && out[4] == 4 && out[5] == 5 && out[6] == 103);

    CHECK(s.read(150, out, 4) == 0);           // past the end
    fs::remove_all(base);
}

static void testInstallExport() {
    const fs::path base = fs::temp_directory_path() / "usbutils_e2e";
    fs::remove_all(base);
    fs::create_directories(base);
    const fs::path iso = base / "game.iso";
    writeTestIso(iso, "SLUS_202.65", 2048 * 3);

    InstallOptions opt;
    opt.gameName = "Test Game";
    InstallResult r = installIso(iso.string(), base.string(), opt);
    CHECK(r.ok);
    CHECK(r.parts == 1);
    CHECK(r.serial == "SLUS_202.65");
    CHECK(fs::exists(base / ("ul." + r.idHex + ".SLUS_202.65.00")));
    if (!r.ok) { fs::remove_all(base); return; }   // bail out early on failure

    UlCfg cfg;
    CHECK(cfg.load(base.string()));
    CHECK(cfg.records.size() == 1);
    if (cfg.records.empty()) { fs::remove_all(base); return; }
    CHECK(cfg.records[0].nameStr() == "Test Game");
    CHECK(cfg.records[0].media == kMediaCd);          // tiny image -> CD
    CHECK(cfg.records[0].parts == 1);

    // structure check
    const StructureReport rep = checkStructure(base.string(), r.idHex, "SLUS_202.65", 1);
    CHECK(rep.ok());

    // rename flow
    CHECK(cfg.renameGame(0, "Renamed Game"));
    CHECK(gameIdHex("Renamed Game") != r.idHex);
    CHECK(fs::exists(base / ("ul." + gameIdHex("Renamed Game") + ".SLUS_202.65.00")));

    // export round trip
    CHECK(exportIso(base.string(), cfg.records[0], (base / "back.iso").string()));
    CHECK(filesEqual(iso, base / "back.iso"));

    // remove — ul.cfg is rewritten (now empty), chunk file gone
    CHECK(cfg.removeGame(0) == 1);
    CHECK(!fs::exists(base / ("ul." + gameIdHex("Renamed Game") + ".SLUS_202.65.00")));
    CHECK(fs::file_size(base / "ul.cfg") == 0);

    fs::remove_all(base);
}

static void testVideoRip() {
    const fs::path base = fs::temp_directory_path() / "usbutils_vrip";
    fs::remove_all(base);
    fs::create_directories(base);
    const fs::path iso = base / "game.iso";
    writeTestIso(iso, "SLUS_202.65", 2048 * 2);

    InstallOptions opt;
    opt.gameName = "Video Game";
    const InstallResult r = installIso(iso.string(), base.string(), opt);
    CHECK(r.ok);
    if (!r.ok) { fs::remove_all(base); return; }
    const std::string id = r.idHex, serial = "SLUS_202.65";

    // ---- scan ------------------------------------------------------------
    std::string err;
    auto videos = scanVideos(base.string(), id, serial, 1, &err);
    CHECK(err.empty());
    CHECK(videos.size() == 1);
    if (videos.size() == 1) {
        CHECK(videos[0].path == "MOVIES\\INTRO.PSS");
        CHECK(videos[0].size == 3 * 2048);
        CHECK(videos[0].offset == 22ull * 2048);
        CHECK(!videos[0].blanked);
        CHECK(!videos[0].hasBackup);
    }

    // ---- rip with backup --------------------------------------------------
    const RipResult rr = ripVideos(base.string(), id, serial, 1, videos, true);
    CHECK(rr.ok);
    CHECK(rr.rippedCount == 1);
    CHECK(rr.blankedBytes == 3 * 2048);
    CHECK(rr.backupBytes == 3 * 2048);
    CHECK(fs::exists(base / (chunkFileName(id, serial, 0) + ".rip")));

    // video range is zeroed in the chunk stream
    {
        GameStream s;
        CHECK(s.open(base.string(), id, serial, 1));
        uint8_t head[16] = {1};
        CHECK(s.read(22ull * 2048, head, sizeof(head)) == sizeof(head));
        bool zeroed = true;
        for (uint8_t b : head) if (b != 0) zeroed = false;
        CHECK(zeroed);
    }

    // ---- rescan reports state ----------------------------------------------
    videos = scanVideos(base.string(), id, serial, 1, &err);
    CHECK(err.empty());
    CHECK(videos.size() == 1);
    CHECK(videos[0].blanked);
    CHECK(videos[0].hasBackup);

    // ---- incremental rip is a no-op (already blanked) -----------------------
    const RipResult rr2 = ripVideos(base.string(), id, serial, 1, videos, true);
    CHECK(rr2.ok);
    CHECK(rr2.rippedCount == 0);

    // ---- restore ------------------------------------------------------------
    const RipResult rv = restoreVideos(base.string(), id, serial, 1);
    CHECK(rv.ok);
    CHECK(rv.rippedCount == 1);
    CHECK(!fs::exists(base / (chunkFileName(id, serial, 0) + ".rip")));

    // pattern is back, byte-exact
    {
        GameStream s;
        CHECK(s.open(base.string(), id, serial, 1));
        uint8_t head[16] = {};
        CHECK(s.read(22ull * 2048, head, sizeof(head)) == sizeof(head));
        CHECK(head[0] == (0 * 13 + 5) && head[1] == (1 * 13 + 5));
        CHECK(head[15] == ((15 * 13 + 5) & 0xFF));
    }

    // the game still exports bit-identical to the original ISO
    {
        UlCfg cfg;
        CHECK(cfg.load(base.string()));
        CHECK(exportIso(base.string(), cfg.records[0], (base / "back.iso").string()));
        CHECK(filesEqual(iso, base / "back.iso"));
    }

    fs::remove_all(base);
}

static void testIoprpLocate() {
    const fs::path base = fs::temp_directory_path() / "usbutils_ioprp";
    fs::remove_all(base);
    fs::create_directories(base);
    const fs::path iso = base / "game.iso";
    writeTestIso(iso, "SLUS_202.65", 2048);

    InstallOptions opt;
    opt.gameName = "Ioprp Game";
    const InstallResult r = installIso(iso.string(), base.string(), opt);
    CHECK(r.ok);
    if (!r.ok) { fs::remove_all(base); return; }

    uint64_t offset = 0;
    uint32_t length = 0;
    CHECK(locateIoprp(base.string(), r.idHex, "SLUS_202.65", 1, offset, length));
    CHECK(offset == 25ull * 2048);      // exact LBA from the directory entry
    CHECK(length == 256);               // exact size — no over-zeroing

    fs::remove_all(base);
}

int main() {
    std::printf("== USBUtils core tests ==\n");
    testCrcId();
    testRecord();
    testChunkMath();
    testRcnRoundTrip();
    testDniParse();
    testWalkIso();
    testIsoInfo();
    testGameStream();
    testInstallExport();
    testVideoRip();
    testIoprpLocate();
    std::printf(g_failed ? "\n%d FAILURE(S)\n" : "\nALL TESTS PASSED\n", g_failed);
    return g_failed ? 1 : 0;
}
