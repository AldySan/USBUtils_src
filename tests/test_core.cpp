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
 * 11. gameops: find / files / extract / inject / copy / move /
 *     correct-code-666.66 / general report
 * 12. update: version comparison logic
 * 13. sidecar backup compression: block-LZSS codec round-trips,
 *     USRRIP02 writer, USRRIP01 reader, incremental rips, corruption
 * 14. iso shrink: relink/compact/truncate round-trip, chunk-file and
 *     ul.cfg parts reduction, rip-backup interplay, refusal paths
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
#include "lzpack.h"
#include "isoshrink.h"
#include "ps2load.h"
#include "gameops.h"
#include "update.h"
#include "isoedit.h"
#include "supportfiles.h"
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

/// Append one ISO9660 directory record (ECMA-119 layout); returns its length.
static size_t putDirRecord(std::vector<uint8_t>& dir, size_t off,
                           uint32_t lba, uint32_t size, uint8_t flags,
                           const std::string& name) {
    uint8_t rec[96] = {};
    size_t recLen = 33 + name.size();
    if (recLen % 2) ++recLen;                    // records are even-sized
    rec[0] = static_cast<uint8_t>(recLen);
    std::memcpy(rec + 2,  &lba, 4);
    std::memcpy(rec + 10, &size, 4);
    rec[18] = 120;                               // date: 2000-00-00 …
    rec[25] = flags;
    rec[28] = 1; rec[29] = 0;                    // volume seq LE
    rec[30] = 0; rec[31] = 1;                    // volume seq BE
    rec[32] = static_cast<uint8_t>(name.size());
    if (!name.empty()) std::memcpy(rec + 33, name.data(), name.size());
    std::memcpy(dir.data() + off, rec, recLen);
    return recLen;
}

/// Build a minimal ISO9660 image with SYSTEM.CNF, a MOVIES directory
/// holding INTRO.PSS, and an IOPRP300.IMG — enough structure to exercise
/// the walker, the video ripper and the DNAS locator. With `withAudio`, an
/// AUDIO directory (MUSIC.VAG + SOUND.ADS + PAD.VB) is appended after the
/// IOPRP image for the audio ripper; PAD.VB opens with 16 zero bytes to
/// exercise the blanked-media probe.
static void writeTestIso(const fs::path& p, const std::string& serial,
                         uint64_t payloadBytes, bool withExtras = true,
                         bool withAudio = false) {
    const uint32_t sector = 2048;
    const uint32_t pssLba = 22;
    const uint32_t pssSize = 3 * sector;
    const uint32_t ioprpLba = 25;
    const uint32_t ioprpSize = 256;
    const uint32_t audioDirLba = 26;
    const uint32_t vagLba = 27, vagSize = 2 * sector;
    const uint32_t adsLba = 29, adsSize = sector;
    const uint32_t padLba = 30, padSize = sector;
    uint32_t dataEnd = withExtras ? ioprpLba : 21;
    if (withAudio) dataEnd = padLba + 1;
    const uint32_t totalSectors =
        dataEnd +
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
    root[18] = 120;                            // date byte (ECMA-119 @18)
    root[25] = 2;                              // directory flag (@25)
    root[28] = 1; root[30] = 1;                // volume sequence numbers
    root[32] = 1;                              // identifier "\0" length
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
    if (withAudio)
        off += putDirRecord(dir, off, audioDirLba, sector, 2, "AUDIO");
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

    if (withAudio) {
        // sector 26: AUDIO directory
        img.resize(audioDirLba * sector, 0);
        std::vector<uint8_t> adir(sector, 0);
        size_t aoff = 0;
        aoff += putDirRecord(adir, aoff, audioDirLba, sector, 2, std::string(1, '\0'));
        aoff += putDirRecord(adir, aoff, rootLba, rootSize, 2, std::string(1, '\1'));
        aoff += putDirRecord(adir, aoff, vagLba, vagSize, 0, "MUSIC.VAG;1");
        aoff += putDirRecord(adir, aoff, adsLba, adsSize, 0, "SOUND.ADS;1");
        aoff += putDirRecord(adir, aoff, padLba, padSize, 0, "PAD.VB;1");
        img.insert(img.end(), adir.begin(), adir.end());

        // sectors 27-28: music payload
        std::vector<uint8_t> vag(vagSize);
        for (size_t i = 0; i < vag.size(); ++i)
            vag[i] = static_cast<uint8_t>((i * 7 + 11) & 0xFF);
        img.insert(img.end(), vag.begin(), vag.end());

        // sector 29: sound-effect payload
        std::vector<uint8_t> ads(adsSize);
        for (size_t i = 0; i < ads.size(); ++i)
            ads[i] = static_cast<uint8_t>((i * 5 + 3) & 0xFF);
        img.insert(img.end(), ads.begin(), ads.end());

        // sector 30: raw VAB body opening with digital silence (16 zero
        // bytes, then a pattern) — must NOT look like an already-ripped file
        std::vector<uint8_t> pad(padSize, 0);
        for (size_t i = 16; i < pad.size(); ++i)
            pad[i] = static_cast<uint8_t>((i * 9 + 7) & 0xFF);
        img.insert(img.end(), pad.begin(), pad.end());
    }

    // trailing generic payload (padded to a full sector like real ISOs,
    // so the image ends on a sector boundary — required by PS2Load)
    const uint64_t payload = payloadBytes ? payloadBytes : 1;
    img.resize(img.size() + payload, 0xAB);
    img.resize((img.size() + sector - 1) / sector * sector, 0);

    FILE* f = std::fopen(p.string().c_str(), "wb");
    std::fwrite(img.data(), 1, img.size(), f);
    std::fclose(f);
}

/// Minimal ISO9660 image with a MOVIES directory holding the given files
/// (name → content). The sidecar-compression tests need bigger and
/// differently-patterned payloads than writeTestIso's fixed layout.
static void writeRipIso(const fs::path& p, const std::string& serial,
                        const std::vector<std::pair<std::string,
                            std::vector<uint8_t>>>& files) {
    const uint32_t sector = 2048;
    const uint32_t rootLba = 18, cnfLba = 20, moviesLba = 21;
    const uint32_t rootSize = sector;
    uint32_t lba = 22;
    std::vector<std::pair<std::string, std::pair<uint32_t, uint32_t>>> recs;
    for (const auto& fl : files) {
        recs.push_back({fl.first, {lba,
            static_cast<uint32_t>(fl.second.size())}});
        lba += static_cast<uint32_t>((fl.second.size() + sector - 1) / sector);
    }
    const uint32_t totalSectors = lba + 1;

    std::vector<uint8_t> img(16 * sector, 0);
    uint8_t pvd[sector] = {};
    pvd[0] = 1;
    std::memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    std::memset(pvd + 40, ' ', 32);
    const std::string volId = "TEST_VOLUME";
    std::memcpy(pvd + 40, volId.c_str(), volId.size());
    pvd[80] = totalSectors & 0xFF; pvd[81] = (totalSectors >> 8) & 0xFF;
    pvd[82] = (totalSectors >> 16) & 0xFF; pvd[83] = (totalSectors >> 24) & 0xFF;
    uint8_t* root = pvd + 156;
    root[0] = 34;
    std::memcpy(root + 2, &rootLba, 4);
    std::memcpy(root + 10, &rootSize, 4);
    root[18] = 120; root[25] = 2; root[28] = 1; root[30] = 1; root[32] = 1;
    img.insert(img.end(), pvd, pvd + sector);
    img.resize(18 * sector, 0);

    std::vector<uint8_t> dir(sector, 0);
    size_t off = 0;
    off += putDirRecord(dir, off, rootLba, rootSize, 2, std::string(1, '\0'));
    off += putDirRecord(dir, off, rootLba, rootSize, 2, std::string(1, '\1'));
    off += putDirRecord(dir, off, cnfLba, 96, 0, "SYSTEM.CNF;1");
    off += putDirRecord(dir, off, moviesLba, sector, 2, "MOVIES");
    img.insert(img.end(), dir.begin(), dir.end());

    img.resize(20 * sector, 0);
    std::string cnf = "BOOT2 = cdrom0:\\" + serial + ";1\nVER = 1.00\n";
    cnf.resize(96, ' ');
    img.insert(img.end(), cnf.begin(), cnf.end());
    img.resize(21 * sector, 0);

    std::vector<uint8_t> mdir(sector, 0);
    size_t moff = 0;
    moff += putDirRecord(mdir, moff, moviesLba, sector, 2, std::string(1, '\0'));
    moff += putDirRecord(mdir, moff, rootLba, rootSize, 2, std::string(1, '\1'));
    for (const auto& r : recs)
        moff += putDirRecord(mdir, moff, r.second.first, r.second.second,
                             0, r.first + ";1");
    img.insert(img.end(), mdir.begin(), mdir.end());

    for (size_t i = 0; i < files.size(); ++i) {
        img.resize(static_cast<size_t>(recs[i].second.first) * sector, 0);
        img.insert(img.end(), files[i].second.begin(), files[i].second.end());
    }
    img.resize(static_cast<size_t>(totalSectors) * sector, 0);

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

    // handle-cache regression: append grows the last chunk and the bytes
    // stay readable through the SAME stream (cached handle escalates
    // read-only -> read/write), then truncate drops them again
    const uint8_t ap[4] = {9, 8, 7, 6};
    CHECK(s.append(ap, 4, 1ull << 30) == 150);
    CHECK(s.size() == 154);
    CHECK(s.read(150, out, 4) == 4);
    CHECK(out[0] == 9 && out[1] == 8 && out[2] == 7 && out[3] == 6);
    CHECK(s.truncate(150));
    CHECK(s.size() == 150);
    CHECK(s.read(148, out, 2) == 2);           // cached handles reopen cleanly
    CHECK(out[0] == 148 && out[1] == 149);
    CHECK(s.read(150, out, 4) == 0);           // gone again

    // the on-disk state must match (a fresh stream sees the truncation)
    {
        GameStream t;
        CHECK(t.open(base.string(), id, serial, 2));
        CHECK(t.size() == 150);
    }
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

/// The 0.0.9 audit fixes: rename collision guards, rollback of half-done
/// chunk renames, and immediate install cancellation.
static void testRenameSafety() {
    const fs::path base = fs::temp_directory_path() / "usbutils_rename";
    fs::remove_all(base);
    fs::create_directories(base);

    // fabricated games (rename paths only touch ul.cfg + file names)
    const std::string S1 = "SLUS_001.00", S2 = "SLUS_003.00";
    const std::string idA = gameIdHex("Game A"), idB = gameIdHex("Game B");
    auto putChunk = [&](const std::string& id, const std::string& serial, int i) {
        std::FILE* f = std::fopen(
            (base / chunkFileName(id, serial, i)).string().c_str(), "wb");
        std::fputs("data", f);
        std::fclose(f);
    };
    putChunk(idA, S1, 0); putChunk(idA, S1, 1);   // Game A, 2 parts
    putChunk(idB, S1, 0);                          // Game B, same serial
    putChunk(idA, S2, 0);                          // Game A again, other serial

    UlCfg cfg;
    cfg.dir = base.string();
    UlCfgRecord a;  a.setName("Game A");  a.setSerial(S1); a.parts = 2;
    UlCfgRecord b;  b.setName("Game B");  b.setSerial(S1); b.parts = 1;
    UlCfgRecord d;  d.setName("Game A");  d.setSerial(S2); d.parts = 1;
    cfg.records = {a, b, d};
    CHECK(cfg.save());

    // 1. collision guard: renaming Game A to "Game B" takes over Game B's
    //    id+serial and would clobber its chunk files — must fail cleanly
    {
        UlCfg c;
        CHECK(c.load(base.string()));
        std::string err;
        CHECK(!c.renameGame(0, "Game B", &err));
        CHECK(!err.empty());
        CHECK(fs::exists(base / chunkFileName(idA, S1, 0)));
        CHECK(fs::exists(base / chunkFileName(idB, S1, 0)));
        CHECK(c.records[0].nameStr() == "Game A");
    }

    // 2. rollback: part 1 missing -> renameSerial fails halfway and must
    //    put part 0 back under the OLD serial
    {
        fs::remove(base / chunkFileName(idA, S1, 1));
        UlCfg c;
        CHECK(c.load(base.string()));
        std::string err;
        CHECK(!c.renameSerial(0, "SLUS_999.99", &err));
        CHECK(!err.empty());
        CHECK(fs::exists(base / chunkFileName(idA, S1, 0)));
        CHECK(!fs::exists(base / chunkFileName(idA, "SLUS_999.99", 0)));
        UlCfg c2;                                   // record unchanged on disk
        CHECK(c2.load(base.string()));
        CHECK(c2.records[0].serialStr() == S1);
    }

    // 3. duplicate id+serial guard on renameSerial
    {
        putChunk(idA, S1, 1);                       // restore the part
        UlCfg c;
        CHECK(c.load(base.string()));
        std::string err;
        CHECK(!c.renameSerial(0, S2, &err));        // Game A + S2 is record 2
        CHECK(!err.empty());
    }

    // 4. a clean rename still works and reports no error
    {
        UlCfg c;
        CHECK(c.load(base.string()));
        std::string err;
        CHECK(c.renameSerial(0, "SLUS_009.99", &err));
        CHECK(err.empty());
        CHECK(fs::exists(base / chunkFileName(idA, "SLUS_009.99", 0)));
        CHECK(fs::exists(base / chunkFileName(idA, "SLUS_009.99", 1)));
        CHECK(!fs::exists(base / chunkFileName(idA, S1, 0)));
    }

    fs::remove_all(base);
}

/// Cancellation must take effect inside the first chunk, not after a whole
/// 1 GiB part, and must leave no file behind.
static void testInstallCancel() {
    const fs::path base = fs::temp_directory_path() / "usbutils_cancel";
    fs::remove_all(base);
    fs::create_directories(base);
    const fs::path iso = base / "game.iso";
    writeTestIso(iso, "SLUS_202.65", 2048 * 3);

    InstallOptions opt;
    opt.gameName = "Cancel Game";
    InstallResult r = installIso(iso.string(), base.string(), opt, nullptr,
                                 [] { return true; });   // cancelled at once
    CHECK(!r.ok);
    CHECK(r.error == "cancelled");
    CHECK(!fs::exists(base / "ul.cfg"));
    CHECK(scanFirstChunks(base.string()).empty());

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

    // ---- hostile sidecar: a lied-about entry count must not OOM ----------
    // (header claims 0xFFFFFFFF entries; the loader must clamp the vector
    // by the real file size instead of resizing to count*16 bytes)
    {
        const fs::path rip = base / (chunkFileName(id, serial, 0) + ".rip");
        {
            FILE* f = std::fopen(rip.string().c_str(), "wb");
            CHECK(f != nullptr);
            if (f) {
                const unsigned char hostile[12] = {
                    'U','S','R','R','I','P','0','1',
                    0xFF, 0xFF, 0xFF, 0xFF };
                std::fwrite(hostile, 1, sizeof(hostile), f);
                std::fclose(f);
            }
        }
        std::string err2;
        const auto v2 = scanVideos(base.string(), id, serial, 1, &err2);
        CHECK(err2.empty());          // unusable sidecar is ignored, not fatal
        CHECK(v2.size() == 1);
        CHECK(!v2[0].hasBackup);      // clamped to zero entries
        fs::remove(rip);
    }

    fs::remove_all(base);
}

static void testAudioRip() {
    const fs::path base = fs::temp_directory_path() / "usbutils_arip";
    fs::remove_all(base);
    fs::create_directories(base);
    const fs::path iso = base / "game.iso";
    writeTestIso(iso, "SLUS_202.65", 2048 * 2, true, /*withAudio*/ true);

    InstallOptions opt;
    opt.gameName = "Audio Game";
    const InstallResult r = installIso(iso.string(), base.string(), opt);
    CHECK(r.ok);
    if (!r.ok) { fs::remove_all(base); return; }
    const std::string id = r.idHex, serial = "SLUS_202.65";
    const std::string rip = chunkFileName(id, serial, 0) + ".rip";
    const std::string arip = chunkFileName(id, serial, 0) + ".arip";

    // ---- scan -----------------------------------------------------------
    std::string err;
    auto audios = scanAudios(base.string(), id, serial, 1, &err);
    CHECK(err.empty());
    CHECK(audios.size() == 3);
    if (audios.size() == 3) {
        CHECK(audios[0].path == "AUDIO\\MUSIC.VAG");
        CHECK(audios[0].offset == 27ull * 2048);
        CHECK(audios[0].size == 2 * 2048);
        CHECK(!audios[0].blanked);
        CHECK(audios[1].path == "AUDIO\\SOUND.ADS");
        CHECK(audios[1].offset == 29ull * 2048);
        CHECK(audios[1].size == 2048);
        CHECK(!audios[1].blanked);
        CHECK(audios[2].path == "AUDIO\\PAD.VB");
        CHECK(audios[2].offset == 30ull * 2048);
        CHECK(audios[2].size == 2048);
        // 16 leading zero bytes (digital silence) must NOT read as blanked —
        // the probe requires a full zero sector
        CHECK(!audios[2].blanked);
    }

    // the video scan is unaffected by the audio feature
    auto videos = scanVideos(base.string(), id, serial, 1, &err);
    CHECK(err.empty());
    CHECK(videos.size() == 1);
    CHECK(videos[0].path == "MOVIES\\INTRO.PSS");

    // ---- partial rip: MUSIC.VAG only, with backup -------------------------
    const RipResult rr = ripAudios(base.string(), id, serial, 1,
                                   {audios[0]}, true);
    CHECK(rr.ok);
    CHECK(rr.rippedCount == 1);
    CHECK(rr.blankedBytes == 2 * 2048);
    CHECK(rr.backupBytes == 2 * 2048);
    CHECK(fs::exists(base / arip));
    CHECK(!fs::exists(base / rip));          // video sidecar untouched
    {
        GameStream s;
        CHECK(s.open(base.string(), id, serial, 1));
        uint8_t vag[16] = {1};
        CHECK(s.read(27ull * 2048, vag, sizeof(vag)) == sizeof(vag));
        bool zeroed = true;
        for (uint8_t b : vag) if (b != 0) zeroed = false;
        CHECK(zeroed);                       // MUSIC.VAG blanked
        uint8_t ads[2] = {};
        CHECK(s.read(29ull * 2048, ads, sizeof(ads)) == sizeof(ads));
        CHECK(ads[0] == ((0 * 5 + 3) & 0xFF) && ads[1] == ((1 * 5 + 3) & 0xFF));
        uint8_t pss[2] = {};
        CHECK(s.read(22ull * 2048, pss, sizeof(pss)) == sizeof(pss));
        CHECK(pss[0] == ((0 * 13 + 5) & 0xFF));   // video untouched
    }

    // rescan reports the state
    audios = scanAudios(base.string(), id, serial, 1, &err);
    CHECK(err.empty());
    CHECK(audios.size() == 3);
    CHECK(audios[0].blanked);
    CHECK(audios[0].hasBackup);
    CHECK(!audios[1].blanked);
    CHECK(!audios[2].blanked);

    // ---- video and audio rips coexist -------------------------------------
    videos = scanVideos(base.string(), id, serial, 1, &err);
    CHECK(ripVideos(base.string(), id, serial, 1, videos, true).ok);
    CHECK(fs::exists(base / rip));
    CHECK(fs::exists(base / arip));

    // ---- drive tools must claim BOTH sidecars (audit 002, R1) -------------
    {
        const DriveSpaceReport rep = scanDriveSpace(base.string());
        CHECK(rep.obsoleteFiles == 0);   // .rip and .arip are game files
        CHECK(rep.gameFiles == 3);       // 1 chunk + 2 sidecars
    }

    // ---- copy carries both sidecars (audit 002, R2) -----------------------
    {
        const fs::path dest = base / "dest";
        fs::remove_all(dest);
        fs::create_directories(dest);
        UlCfg srcCfg;
        CHECK(srcCfg.load(base.string()));
        const CopyResult cr =
            copyGame(base.string(), srcCfg, 0, dest.string(), false);
        CHECK(cr.ok);
        CHECK(fs::exists(dest / rip));
        CHECK(filesEqual(base / rip, dest / rip));
        CHECK(fs::exists(dest / arip));
        CHECK(filesEqual(base / arip, dest / arip));
        fs::remove_all(dest);
    }

    // ---- restore audio; the video rip survives ----------------------------
    const RipResult rv = restoreAudios(base.string(), id, serial, 1);
    CHECK(rv.ok);
    CHECK(rv.rippedCount == 1);
    CHECK(!fs::exists(base / arip));         // audio backup consumed
    CHECK(fs::exists(base / rip));           // video backup still there
    {
        GameStream s;
        CHECK(s.open(base.string(), id, serial, 1));
        uint8_t vag[2] = {};
        CHECK(s.read(27ull * 2048, vag, sizeof(vag)) == sizeof(vag));
        CHECK(vag[0] == ((0 * 7 + 11) & 0xFF) && vag[1] == ((1 * 7 + 11) & 0xFF));
    }

    // ---- restore video, then export must be byte-identical ----------------
    CHECK(restoreVideos(base.string(), id, serial, 1).ok);
    CHECK(!fs::exists(base / rip));
    {
        UlCfg cfg;
        CHECK(cfg.load(base.string()));
        CHECK(exportIso(base.string(), cfg.records[0],
                        (base / "back.iso").string()));
        CHECK(filesEqual(iso, base / "back.iso"));
    }

    // ---- a no-backup rip is still detected by the zero probe --------------
    audios = scanAudios(base.string(), id, serial, 1, &err);
    CHECK(err.empty());
    CHECK(audios.size() == 3);
    CHECK(!audios[2].blanked);
    {
        const RipResult rn = ripAudios(base.string(), id, serial, 1,
                                      {audios[2]}, /*makeBackup*/ false);
        CHECK(rn.ok);
        CHECK(rn.rippedCount == 1);
        CHECK(rn.blankedBytes == 2048);
        CHECK(rn.backupBytes == 0);
    }
    audios = scanAudios(base.string(), id, serial, 1, &err);
    CHECK(audios.size() == 3);
    CHECK(audios[2].blanked);          // fully zeroed: probe hits
    CHECK(!audios[2].hasBackup);

    fs::remove_all(base);
}

// ---------------------------------------------------------------------------
// sidecar backup compression (USRRIP02)
// ---------------------------------------------------------------------------

static void testSidecarCompression() {
    // ---- codec round-trips ----------------------------------------------
    {
        std::vector<uint8_t> zeros(kLzBlockSize);
        std::vector<uint8_t> comp;
        CHECK(lzEncodeBlock(zeros.data(), zeros.size(), comp));
        CHECK(comp.size() < zeros.size() / 8);
        std::vector<uint8_t> back(kLzBlockSize);
        CHECK(lzDecodeBlock(comp.data(), comp.size(), back.data(), zeros.size()));
        CHECK(std::memcmp(zeros.data(), back.data(), zeros.size()) == 0);

        // pseudo-random data is not worth compressing: the block must be
        // reported as "store verbatim" instead of growing
        std::vector<uint8_t> rnd(256 * 1024);
        uint32_t x = 1;
        for (auto& b : rnd)
            b = static_cast<uint8_t>((x = x * 1664525u + 1013904223u) >> 24);
        std::vector<uint8_t> comp2;
        CHECK(!lzEncodeBlock(rnd.data(), rnd.size(), comp2));
        CHECK(comp2.empty());

        // malformed streams must fail, never write past the output
        uint8_t out[16] = {};
        const uint8_t badOffset[] = {0x00, 0x01, 0x00, 0x00};   // match at pos 0
        CHECK(!lzDecodeBlock(badOffset, sizeof(badOffset), out, sizeof(out)));
        const uint8_t badOverflow[] = {0x03, 'a', 'b', 0x01, 0x00, 0xFF};
        CHECK(!lzDecodeBlock(badOverflow, sizeof(badOverflow), out, sizeof(out)));
        const uint8_t shortStream[] = {0x01};                    // literal missing
        CHECK(!lzDecodeBlock(shortStream, sizeof(shortStream), out, sizeof(out)));
    }

    // ---- rip with backup writes a compressed USRRIP02 sidecar ------------
    const fs::path base = fs::temp_directory_path() / "usbutils_ripz";
    fs::remove_all(base);
    fs::create_directories(base);
    const fs::path iso = base / "game.iso";

    // BIG.PSS: ADPCM-like frames (predictor pattern + a silent frame every
    // 4th) — must compress well. NOISE.PSS: pseudo-random — must fall back
    // to stored blocks without growing the file.
    std::vector<uint8_t> big(2ull * 1024 * 1024 + 512 * 1024);
    for (size_t i = 0; i < big.size(); ++i) {
        const size_t frame = i / 16, off = i % 16;
        big[i] = (frame % 4 == 3) ? 0
                : static_cast<uint8_t>((off * 7 + (frame & 0xFF)) & 0xFF);
    }
    std::vector<uint8_t> noise(1024 * 1024 + 200 * 1024);
    uint32_t x = 987654321u;
    for (auto& b : noise)
        b = static_cast<uint8_t>((x = x * 1664525u + 1013904223u) >> 24);
    writeRipIso(iso, "SLUS_202.65", {{"BIG.PSS", big}, {"NOISE.PSS", noise}});

    InstallOptions opt;
    opt.gameName = "Comp Game";
    const InstallResult ir = installIso(iso.string(), base.string(), opt);
    CHECK(ir.ok);
    if (!ir.ok) { fs::remove_all(base); return; }
    const std::string id = ir.idHex, serial = "SLUS_202.65";
    const fs::path rip = base / (chunkFileName(id, serial, 0) + ".rip");

    std::string err;
    auto videos = scanVideos(base.string(), id, serial, 1, &err);
    CHECK(err.empty());
    CHECK(videos.size() == 2);
    if (videos.size() != 2) { fs::remove_all(base); return; }
    std::vector<VideoEntry> bigOnly, noiseOnly;
    for (const auto& v : videos)
        (v.path == "MOVIES\\BIG.PSS" ? bigOnly : noiseOnly).push_back(v);
    CHECK(bigOnly.size() == 1);
    CHECK(noiseOnly.size() == 1);
    if (bigOnly.size() != 1 || noiseOnly.size() != 1) {
        fs::remove_all(base);
        return;
    }

    // rip BIG first: the sidecar must come out clearly smaller than the
    // raw payload (multi-block: 2.5 MiB spans three 1 MiB blocks)
    const RipResult r1 = ripVideos(base.string(), id, serial, 1, bigOnly, true);
    CHECK(r1.ok);
    CHECK(r1.rippedCount == 1);
    CHECK(r1.backupBytes == big.size());
    CHECK(fs::exists(rip));
    {
        FILE* f = std::fopen(rip.string().c_str(), "rb");
        CHECK(f != nullptr);
        if (f) {
            char magic[8] = {};
            CHECK(std::fread(magic, 1, 8, f) == 8);
            std::fclose(f);
            CHECK(std::memcmp(magic, "USRRIP02", 8) == 0);
        }
    }
    CHECK(fs::file_size(rip) < big.size() / 2);

    // rip NOISE on top: the incremental save must re-read the v2 payload
    // (the game stream holds zeros for BIG by now) and keep everything
    // restorable; the incompressible noise ends up stored verbatim
    const uint64_t rawTotal = big.size() + noise.size();
    const RipResult r2 = ripVideos(base.string(), id, serial, 1, noiseOnly, true);
    CHECK(r2.ok);
    CHECK(r2.rippedCount == 1);
    CHECK(r2.backupBytes == noise.size());
    CHECK(fs::file_size(rip) < rawTotal * 3 / 4);
    CHECK(fs::file_size(rip) > noise.size() * 9 / 10);

    // restore both, then export must be byte-identical to the source ISO
    const RipResult rv = restoreVideos(base.string(), id, serial, 1);
    CHECK(rv.ok);
    CHECK(rv.rippedCount == 2);
    CHECK(!fs::exists(rip));
    {
        UlCfg cfg;
        CHECK(cfg.load(base.string()));
        CHECK(exportIso(base.string(), cfg.records[0],
                        (base / "back.iso").string()));
        CHECK(filesEqual(iso, base / "back.iso"));
    }

    // ---- a hand-written USRRIP01 sidecar still restores ------------------
    // (backups written by builds before the compression change)
    uint64_t noiseOff = 0;
    {
        videos = scanVideos(base.string(), id, serial, 1, &err);
        for (const auto& v : videos)
            if (v.path == "MOVIES\\NOISE.PSS") noiseOff = v.offset;
        const RipResult rn = ripVideos(base.string(), id, serial, 1,
                                       noiseOnly, /*makeBackup*/ false);
        CHECK(rn.ok);
        CHECK(rn.rippedCount == 1);
    }
    {
        FILE* f = std::fopen(rip.string().c_str(), "wb");
        CHECK(f != nullptr);
        if (f) {
            const unsigned char hdr[12] = {
                'U','S','R','R','I','P','0','1', 0x01, 0, 0, 0 };
            CHECK(std::fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr));
            const uint64_t rec[2] = {noiseOff, noise.size()};
            CHECK(std::fwrite(rec, 8, 2, f) == 2);
            CHECK(std::fwrite(noise.data(), 1, noise.size(), f) == noise.size());
            std::fclose(f);
        }
    }
    {
        const RipResult rv1 = restoreVideos(base.string(), id, serial, 1);
        CHECK(rv1.ok);
        CHECK(rv1.rippedCount == 1);
        CHECK(!fs::exists(rip));
        UlCfg cfg;
        CHECK(cfg.load(base.string()));
        CHECK(exportIso(base.string(), cfg.records[0],
                        (base / "back1.iso").string()));
        CHECK(filesEqual(iso, base / "back1.iso"));
    }

    // ---- an incremental rip upgrades a v1 sidecar to v2 unharmed ---------
    // (BIG blanked without backup + a v1 sidecar holding its originals;
    //  ripping NOISE with a backup must carry BIG's payload across)
    uint64_t bigOff = 0;
    {
        videos = scanVideos(base.string(), id, serial, 1, &err);
        std::vector<VideoEntry> b2;
        for (const auto& v : videos) {
            if (v.path == "MOVIES\\BIG.PSS") { bigOff = v.offset; b2.push_back(v); }
        }
        CHECK(ripVideos(base.string(), id, serial, 1, b2, false).ok);
    }
    {
        FILE* f = std::fopen(rip.string().c_str(), "wb");
        CHECK(f != nullptr);
        if (f) {
            const unsigned char hdr[12] = {
                'U','S','R','R','I','P','0','1', 0x01, 0, 0, 0 };
            CHECK(std::fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr));
            const uint64_t rec[2] = {bigOff, big.size()};
            CHECK(std::fwrite(rec, 8, 2, f) == 2);
            CHECK(std::fwrite(big.data(), 1, big.size(), f) == big.size());
            std::fclose(f);
        }
    }
    {
        videos = scanVideos(base.string(), id, serial, 1, &err);
        std::vector<VideoEntry> n3;
        for (const auto& v : videos)
            if (v.path == "MOVIES\\NOISE.PSS") n3.push_back(v);
        const RipResult rn = ripVideos(base.string(), id, serial, 1, n3, true);
        CHECK(rn.ok);
        CHECK(rn.rippedCount == 1);
    }
    {
        const RipResult rv2 = restoreVideos(base.string(), id, serial, 1);
        CHECK(rv2.ok);
        CHECK(rv2.rippedCount == 2);
        CHECK(!fs::exists(rip));
        UlCfg cfg;
        CHECK(cfg.load(base.string()));
        CHECK(exportIso(base.string(), cfg.records[0],
                        (base / "back2.iso").string()));
        CHECK(filesEqual(iso, base / "back2.iso"));
    }

    // ---- corruption is detected, never written into the game -------------
    // truncate a fresh sidecar mid-payload: restore must fail and keep the
    // file (nothing is consumed on failure)
    {
        videos = scanVideos(base.string(), id, serial, 1, &err);
        std::vector<VideoEntry> b3;
        for (const auto& v : videos)
            if (v.path == "MOVIES\\BIG.PSS") b3.push_back(v);
        CHECK(ripVideos(base.string(), id, serial, 1, b3, true).ok);

        std::vector<uint8_t> all;
        {
            FILE* f = std::fopen(rip.string().c_str(), "rb");
            CHECK(f != nullptr);
            if (f) {
                uint8_t tb[65536];
                size_t n;
                while ((n = std::fread(tb, 1, sizeof(tb), f)) > 0)
                    all.insert(all.end(), tb, tb + n);
                std::fclose(f);
            }
        }
        CHECK(all.size() > 20 + 16 + 4096);
        {
            FILE* f = std::fopen(rip.string().c_str(), "wb");
            CHECK(f != nullptr);
            if (f) {
                std::fwrite(all.data(), 1, 20 + 16 + 4096, f);
                std::fclose(f);
            }
        }
        const RipResult rt = restoreVideos(base.string(), id, serial, 1);
        CHECK(!rt.ok);
        CHECK(!rt.error.empty());
        CHECK(fs::exists(rip));
    }
    // tampered v2 header: payloadRaw no longer matches the entry sizes
    {
        FILE* f = std::fopen(rip.string().c_str(), "r+b");
        CHECK(f != nullptr);
        if (f) {
            CHECK(std::fseek(f, 12, SEEK_SET) == 0);
            std::fputc('X', f);
            std::fclose(f);
        }
        const RipResult rt2 = restoreVideos(base.string(), id, serial, 1);
        CHECK(!rt2.ok);
        CHECK(!rt2.error.empty());
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

static void testPs2Load() {
    const fs::path base = fs::temp_directory_path() / "usbutils_ps2l";
    fs::remove_all(base);
    fs::create_directories(base);
    const fs::path iso = base / "game.iso";
    writeTestIso(iso, "SLUS_202.65", 2048);

    InstallOptions opt;
    opt.gameName = "Ps2l Game";
    const InstallResult ir = installIso(iso.string(), base.string(), opt);
    CHECK(ir.ok);
    if (!ir.ok) { fs::remove_all(base); return; }

    // a stand-in loader ELF (the real one is 218 KiB in data/)
    const fs::path loader = base / "PS2LOAD.ELF";
    std::vector<uint8_t> elf(4096);
    std::memcpy(elf.data(), "\x7f" "ELF", 4);
    for (size_t i = 4; i < elf.size(); ++i)
        elf[i] = static_cast<uint8_t>(i & 0xFF);
    {
        FILE* f = std::fopen(loader.string().c_str(), "wb");
        std::fwrite(elf.data(), 1, elf.size(), f);
        std::fclose(f);
    }
    const uint64_t chunkBefore = fs::file_size(
        base / ("ul." + ir.idHex + ".SLUS_202.65.00"));

    // ---- status before ---------------------------------------------------
    Ps2LoadInfo info = queryPs2Load(base.string(), ir.idHex, "SLUS_202.65", 1);
    CHECK(info.state == Ps2LoadState::NotInjected);
    CHECK(info.bootElf == "SLUS_202.65");
    CHECK(info.ps2lName == "PS2L_202.65.65");   // drop 5, re-add ext

    // ---- inject ----------------------------------------------------------
    const Ps2LoadResult res = injectPs2Load(base.string(), ir.idHex,
                                            "SLUS_202.65", 1,
                                            loader.string());
    CHECK(res.ok);
    if (!res.ok) { fs::remove_all(base); return; }
    CHECK(res.ps2lName == "PS2L_202.65.65");
    CHECK(res.appendedBytes == 2048 + elf.size());  // padded CNF + ELF

    // double inject must be refused (message @0x3BB370)
    const Ps2LoadResult again = injectPs2Load(base.string(), ir.idHex,
                                              "SLUS_202.65", 1,
                                              loader.string());
    CHECK(!again.ok);

    // ---- verify through the ISO walker -----------------------------------
    GameStream gs;
    CHECK(gs.open(base.string(), ir.idHex, "SLUS_202.65", 1));
    std::map<std::string, IsoEntry> found;
    walkIso([&](uint64_t off, void* dst, size_t n) {
        return gs.read(off, dst, n) == n;
    }, [&](const IsoEntry& e) {
        found[e.name] = e;
        return true;
    });
    CHECK(found.count("SCEE_DD") == 1);
    CHECK(found.count("MSYSTEM.CNF") == 1);
    CHECK(found.count("PS2L_202.65.65") == 1);
    if (found.count("SCEE_DD")) {
        CHECK(found["SCEE_DD"].isDir);
        CHECK(found["SCEE_DD"].lba < info.rootDirLba);   // tool marker
        CHECK(found["SCEE_DD"].size == 0x158);
    }
    if (found.count("MSYSTEM.CNF")) {
        CHECK(found["MSYSTEM.CNF"].size == 2048);       // sector snapshot
        CHECK(!found["MSYSTEM.CNF"].isDir);            // file record (flags @25)
    }
    if (found.count("PS2L_202.65.65")) {
        const IsoEntry& e = found["PS2L_202.65.65"];
        CHECK(!e.isDir);       // the boot ELF is a FILE — a directory flag on
                                // it would make the game unbootable
        CHECK(e.size == elf.size());
        std::vector<uint8_t> got(elf.size());
        CHECK(gs.read(e.lba * 2048ull, got.data(), got.size()) == got.size());
        CHECK(std::memcmp(got.data(), elf.data(), elf.size()) == 0);
    }

    // SYSTEM.CNF now boots the injected record name
    {
        char cnf[97] = {};
        gs.read(20ull * 2048, cnf, 96);
        CHECK(std::strstr(cnf, "PS2L_202.65.65") != nullptr);
        CHECK(std::strstr(cnf, "cdrom0:\\SLUS_202.65") == nullptr);
    }
    // the backup keeps the original boot line
    if (found.count("MSYSTEM.CNF")) {
        char backup[97] = {};
        gs.read(found["MSYSTEM.CNF"].lba * 2048ull, backup, 96);
        CHECK(std::strstr(backup, "cdrom0:\\SLUS_202.65;1") != nullptr);
    }

    // ---- status after ----------------------------------------------------
    info = queryPs2Load(base.string(), ir.idHex, "SLUS_202.65", 1);
    CHECK(info.state == Ps2LoadState::InjectedByTool);
    CHECK(info.ps2lName == "PS2L_202.65.65");

    // ---- remove ----------------------------------------------------------
    const Ps2LoadResult rr = removePs2Load(base.string(), ir.idHex,
                                           "SLUS_202.65", 1);
    CHECK(rr.ok);
    info = queryPs2Load(base.string(), ir.idHex, "SLUS_202.65", 1);
    CHECK(info.state == Ps2LoadState::NotInjected);

    {
        char cnf[97] = {};
        gs.read(20ull * 2048, cnf, 96);
        CHECK(std::strstr(cnf, "cdrom0:\\SLUS_202.65;1") != nullptr);
    }
    // the chunk file is trimmed back to its pre-inject size
    CHECK(fs::file_size(base / ("ul." + ir.idHex + ".SLUS_202.65.00"))
          == chunkBefore);

    // export round trip is bit-exact again
    UlCfg cfg;
    CHECK(cfg.load(base.string()));
    CHECK(exportIso(base.string(), cfg.records[0],
                    (base / "back.iso").string()));
    CHECK(filesEqual(iso, base / "back.iso"));

    // ---- regression: boot name with a 14-byte PS2L record -----------------
    // "PS2L_123.4.4;1" is 14 bytes long — 14 & 0x02 != 0. The record
    // writer used to put the NAME LENGTH into the ECMA-119 flags byte, so
    // this (very common) name length marked the boot ELF as a DIRECTORY
    // and the injected game would not boot. The first scenario above
    // ("...;1" = 16 bytes) was accidentally immune: 16 & 0x02 == 0.
    {
        const fs::path iso2 = base / "game2.iso";
        writeTestIso(iso2, "SLUS_123.4", 2048);
        InstallOptions o2;
        o2.gameName = "Ps2l Two";
        const InstallResult ir2 = installIso(iso2.string(), base.string(), o2);
        CHECK(ir2.ok);
        if (ir2.ok) {
            const Ps2LoadResult r2 = injectPs2Load(
                base.string(), ir2.idHex, "SLUS_123.40", 1, loader.string());
            CHECK(r2.ok);
            CHECK(r2.ps2lName == "PS2L_123.4.4");
            if (r2.ok) {
                GameStream gs2;
                CHECK(gs2.open(base.string(), ir2.idHex, "SLUS_123.40", 1));
                std::map<std::string, IsoEntry> found2;
                walkIso([&gs2](uint64_t off, void* dst, size_t n) {
                    return gs2.read(off, dst, n) == n;
                }, [&](const IsoEntry& e) {
                    found2[e.name] = e;
                    return true;
                });
                CHECK(found2.count("PS2L_123.4.4") == 1);
                if (found2.count("PS2L_123.4.4"))
                    CHECK(!found2["PS2L_123.4.4"].isDir);  // boot ELF = FILE
            }
        }
    }

    fs::remove_all(base);
}

// ---------------------------------------------------------------------------
// 11. gameops: find / files / extract / inject / copy / move / 666.66 / report
// ---------------------------------------------------------------------------

static void testGameOps() {
    // ---- wildcard matcher --------------------------------------------------
    CHECK(matchPattern("INTRO.PSS", "*.PSS"));
    CHECK(matchPattern("intro.pss", "INTRO*"));
    CHECK(matchPattern("FILE.TXT", "F?LE.*"));
    CHECK(matchPattern("FILE.TXT", "FILE.TXT"));
    CHECK(!matchPattern("FILE.TXT", "*.BIN"));
    CHECK(matchPattern("MOVIES\\INTRO.PSS", "*.PSS"));
    CHECK(matchPattern("anything", ""));

    const fs::path base = fs::temp_directory_path() / "usbutils_gameops";
    fs::remove_all(base);
    fs::create_directories(base);
    const fs::path iso = base / "game.iso";
    writeTestIso(iso, "SLUS_202.65", 2048 * 2);

    InstallOptions opt;
    opt.gameName = "Ops Game";
    const InstallResult r = installIso(iso.string(), base.string(), opt);
    CHECK(r.ok);
    if (!r.ok) { fs::remove_all(base); return; }
    const std::string id = r.idHex, serial = "SLUS_202.65";

    // ---- file listing ------------------------------------------------------
    const auto files = listGameFiles(base.string(), id, serial, 1);
    CHECK(files.size() == 3);          // SYSTEM.CNF, MOVIES\INTRO.PSS, IOPRP300.IMG
    bool hasCnf = false, hasPss = false, hasIoprp = false;
    for (const IsoEntry& e : files) {
        if (e.name == "SYSTEM.CNF")  hasCnf   = e.size == 96;
        if (e.name == "INTRO.PSS")   hasPss   = e.size == 3 * 2048 && e.lba == 22;
        if (e.name == "IOPRP300.IMG") hasIoprp = e.size == 256;
    }
    CHECK(hasCnf && hasPss && hasIoprp);

    // ---- find in games -----------------------------------------------------
    UlCfg cfg;
    CHECK(cfg.load(base.string()));
    auto hits = findFileInGames(base.string(), cfg, "*.pss");
    CHECK(hits.size() == 1);
    CHECK(hits.size() == 1 && hits[0].entry.path == "MOVIES\\INTRO.PSS");
    hits = findFileInGames(base.string(), cfg, "*", 256);   // by size
    CHECK(hits.size() == 1);
    CHECK(hits.size() == 1 && hits[0].entry.name == "IOPRP300.IMG");
    hits = findFileInGames(base.string(), cfg, "*.bin");
    CHECK(hits.empty());

    // ---- extract one file --------------------------------------------------
    const IsoEntry* pss = nullptr;
    for (const IsoEntry& e : files)
        if (e.name == "INTRO.PSS") { pss = &e; break; }
    CHECK(pss != nullptr);
    if (pss) {
        CHECK(extractGameFile(base.string(), id, serial, 1, *pss,
                              (base / "intro.pss").string()));
        // compare against the same byte range of the source ISO
        FILE* f = std::fopen(iso.string().c_str(), "rb");
        CHECK(f != nullptr);
        if (f) {
            std::vector<uint8_t> expect(pss->size);
            usbutils::seek64(f, uint64_t(pss->lba) * 2048);
            CHECK(std::fread(expect.data(), 1, expect.size(), f) == expect.size());
            std::fclose(f);
            FILE* g = std::fopen((base / "intro.pss").string().c_str(), "rb");
            CHECK(g != nullptr);
            if (g) {
                std::vector<uint8_t> got(pss->size);
                CHECK(std::fread(got.data(), 1, got.size(), g) == got.size());
                std::fclose(g);
                CHECK(std::memcmp(expect.data(), got.data(), expect.size()) == 0);
            }
        }
    }

    // ---- copy to another drive ---------------------------------------------
    const fs::path dest = fs::temp_directory_path() / "usbutils_gameops_dest";
    fs::remove_all(dest);
    fs::create_directories(dest);
    {
        UlCfg src;
        CHECK(src.load(base.string()));
        const CopyResult cr = copyGame(base.string(), src, 0, dest.string(), false);
        CHECK(cr.ok);
        CHECK(cr.copiedBytes == gameTotalSize(base.string(), id, serial, 1));
        CHECK(fs::exists(dest / ("ul." + id + "." + serial + ".00")));

        UlCfg dcfg;
        CHECK(dcfg.load(dest.string()));
        CHECK(dcfg.records.size() == 1);
        CHECK(dcfg.records[0].nameStr() == "Ops Game");

        // exporting from the destination reproduces the source image
        CHECK(exportIso(dest.string(), dcfg.records[0],
                        (dest / "back.iso").string()));
        CHECK(filesEqual(iso, dest / "back.iso"));

        // copying the same game again must collide
        const CopyResult cr2 = copyGame(base.string(), src, 0, dest.string(), false);
        CHECK(!cr2.ok);
    }

    // ---- inject (same-size replace + mismatch rejection), on the copy ----
    {
        std::vector<uint8_t> repl(3 * 2048, 0xCC);
        FILE* f = std::fopen((dest / "repl.bin").string().c_str(), "wb");
        std::fwrite(repl.data(), 1, repl.size(), f);
        std::fclose(f);
        const InjectResult ir = injectGameFile(dest.string(), id, serial, 1,
                                               "movies/intro.pss",
                                               (dest / "repl.bin").string());
        CHECK(ir.ok);
        CHECK(ir.bytes == 3 * 2048);

        // wrong size must be refused
        std::vector<uint8_t> bad(128, 0);
        f = std::fopen((dest / "bad.bin").string().c_str(), "wb");
        std::fwrite(bad.data(), 1, bad.size(), f);
        std::fclose(f);
        const InjectResult ir2 = injectGameFile(dest.string(), id, serial, 1,
                                                "MOVIES\\INTRO.PSS",
                                                (dest / "bad.bin").string());
        CHECK(!ir2.ok);
        CHECK(ir2.error.find("size mismatch") != std::string::npos);

        // the injected bytes are really in the stream now
        GameStream gs;
        CHECK(gs.open(dest.string(), id, serial, 1));
        std::vector<uint8_t> got(3 * 2048);
        CHECK(gs.read(22ull * 2048, got.data(), got.size()) == got.size());
        bool allCC = true;
        for (uint8_t b : got) if (b != 0xCC) { allCC = false; break; }
        CHECK(allCC);
    }

    // ---- move to a third drive ---------------------------------------------
    const fs::path dest2 = fs::temp_directory_path() / "usbutils_gameops_dest2";
    fs::remove_all(dest2);
    fs::create_directories(dest2);
    {
        UlCfg src;
        CHECK(src.load(base.string()));
        CHECK(src.records.size() == 1);
        const CopyResult cr = copyGame(base.string(), src, 0, dest2.string(), true);
        CHECK(cr.ok);
        CHECK(src.records.empty());                       // record removed
        CHECK(!fs::exists(base / ("ul." + id + "." + serial + ".00")));
        CHECK(fs::exists(dest2 / ("ul." + id + "." + serial + ".00")));
    }

    // ---- correct code 666.66 -------------------------------------------------
    // simulate a record corrupted by a third-party tool: serial replaced by
    // the magic broken marker and the chunk file renamed accordingly
    const std::string brokenSerial = "666.66";
    {
        UlCfg src;
        CHECK(src.load(dest2.string()));
        CHECK(src.records.size() == 1);
        const std::string gid = gameIdHex(src.records[0].nameStr());
        std::error_code ec;
        fs::rename(dest2 / ("ul." + gid + "." + serial + ".00"),
                   dest2 / ("ul." + gid + "." + brokenSerial + ".00"), ec);
        CHECK(!ec);
        src.records[0].setSerial(brokenSerial);
        CHECK(src.save());

        // fake rip backups under the broken serial — the fix must carry
        // both sidecars along with the chunks (audit 002, R2)
        const std::string ripBroken  = "ul." + gid + "." + brokenSerial + ".00.rip";
        const std::string aripBroken = "ul." + gid + "." + brokenSerial + ".00.arip";
        { FILE* f = std::fopen((dest2 / ripBroken).c_str(), "wb");
          std::fputs("RIPDATA", f); std::fclose(f); }
        { FILE* f = std::fopen((dest2 / aripBroken).c_str(), "wb");
          std::fputs("ARIPDATA", f); std::fclose(f); }

        // dry run: sees the break, proposes the real serial
        UlCfg dry;
        CHECK(dry.load(dest2.string()));
        const auto fixes = fixBrokenSerials(dest2.string(), dry, false);
        CHECK(fixes.size() == 1);
        CHECK(fixes.size() == 1 && fixes[0].newSerial == "SLUS_202.65");
        CHECK(fixes.size() == 1 && !fixes[0].applied);
        CHECK(fs::exists(dest2 / ("ul." + gid + "." + brokenSerial + ".00")));

        // apply: serial fixed, chunks renamed back
        const auto fixes2 = fixBrokenSerials(dest2.string(), dry, true);
        CHECK(fixes2.size() == 1 && fixes2[0].applied);
        CHECK(dry.records[0].serialStr() == "SLUS_202.65");
        CHECK(fs::exists(dest2 / ("ul." + gid + "." + serial + ".00")));
        CHECK(!fs::exists(dest2 / ("ul." + gid + "." + brokenSerial + ".00")));
        CHECK(fs::exists(dest2 / ("ul." + gid + "." + serial + ".00.rip")));
        CHECK(fs::exists(dest2 / ("ul." + gid + "." + serial + ".00.arip")));
        CHECK(!fs::exists(dest2 / ripBroken));
        CHECK(!fs::exists(dest2 / aripBroken));
        CHECK(checkStructure(dest2.string(), gid, serial, 1).ok());

        // and the data is untouched
        CHECK(exportIso(dest2.string(), dry.records[0],
                        (dest2 / "back2.iso").string()));
        CHECK(filesEqual(iso, dest2 / "back2.iso"));
    }

    // ---- general report -------------------------------------------------------
    {
        UlCfg src;
        CHECK(src.load(dest2.string()));
        const std::string rep = generateReport(dest2.string(), src);
        CHECK(rep.find("Games total: 1") != std::string::npos);
        CHECK(rep.find("Ops Game") != std::string::npos);
        CHECK(rep.find("structure OK") != std::string::npos);
        CHECK(rep.find("none") != std::string::npos);       // broken serials
    }

    fs::remove_all(base);
    fs::remove_all(dest);
    fs::remove_all(dest2);
}

// ---------------------------------------------------------------------------
// 12. update: version comparison
// ---------------------------------------------------------------------------

static void testSortAndClean() {
    const fs::path base = fs::temp_directory_path() / "usbutils_sortclean";
    fs::remove_all(base);
    fs::create_directories(base);

    // three tiny games, installed out of alphabetical order
    const char* names[3] = {"Charlie Game", "alpha game", "Bravo Game"};
    std::string ids[3];
    for (int i = 0; i < 3; ++i) {
        const fs::path iso = base / (std::string("g") + std::to_string(i) + ".iso");
        writeTestIso(iso, "SLUS_200.0" + std::string(1, char('1' + i)), 2048 * 2);
        InstallOptions opt;
        opt.gameName = names[i];
        const InstallResult r = installIso(iso.string(), base.string(), opt);
        CHECK(r.ok);
        ids[i] = r.idHex;
        fs::remove(iso);
    }
    UlCfg cfg;
    CHECK(cfg.load(base.string()));
    CHECK(cfg.records.size() == 3);
    if (cfg.records.size() != 3) { fs::remove_all(base); return; }
    CHECK(cfg.records[0].nameStr() == "Charlie Game");   // install order

    // ---- sort --------------------------------------------------------------
    size_t moved = 0;
    CHECK(sortGames(base.string(), &moved));
    CHECK(moved == 3);                                   // all changed place
    CHECK(cfg.load(base.string()));
    CHECK(cfg.records[0].nameStr() == "alpha game");     // case-insensitive
    CHECK(cfg.records[1].nameStr() == "Bravo Game");
    CHECK(cfg.records[2].nameStr() == "Charlie Game");
    // idempotent: second run reports nothing to move
    CHECK(sortGames(base.string(), &moved));
    CHECK(moved == 0);

    // ---- clean: nothing obsolete yet ---------------------------------------
    DriveSpaceReport rep = scanDriveSpace(base.string());
    CHECK(rep.games == 3);
    CHECK(rep.obsoleteFiles == 0);
    CHECK(rep.gameFiles == 3);                           // one chunk each

    // ---- clean: stray chunk + rip sidecar protection ------------------------
    // a rip sidecar of game[0] (either flavor) must count as a game file...
    const std::string rip = "ul." + ids[0] + ".SLUS_200.01.00.rip";
    const std::string arip = "ul." + ids[0] + ".SLUS_200.01.00.arip";
    { FILE* f = std::fopen((base / rip).c_str(), "wb"); std::fputc(1, f); std::fclose(f); }
    { FILE* f = std::fopen((base / arip).c_str(), "wb"); std::fputc(1, f); std::fclose(f); }
    // ...while an unreferenced chunk from a deleted game is obsolete
    const std::string stray = "ul.DEADBEEF.SLUS_999.99.00";
    { FILE* f = std::fopen((base / stray).c_str(), "wb");
      std::fwrite("junk", 1, 4, f); std::fclose(f); }

    rep = scanDriveSpace(base.string());
    CHECK(rep.obsoleteFiles == 1);
    CHECK(rep.obsoleteBytes == 4);
    CHECK(rep.gameFiles == 5);                     // 3 chunks + 2 sidecars
    CHECK(rep.obsolete.size() == 1 && rep.obsolete[0] == stray);

    // restore space deletes the stray file only
    CHECK(restoreSpace(base.string(), rep) == 1);
    CHECK(!fs::exists(base / stray));
    CHECK(fs::exists(base / rip));                 // sidecars survived
    CHECK(fs::exists(base / arip));
    rep = scanDriveSpace(base.string());
    CHECK(rep.obsoleteFiles == 0);

    // ---- clean: zero-size detection -----------------------------------------
    { FILE* f = std::fopen((base / "ul.CAFE0000.SLUS_000.00.01").c_str(), "wb");
      std::fclose(f); }                                   // empty file
    rep = scanDriveSpace(base.string());
    CHECK(rep.zeroSizeFiles == 1);
    CHECK(rep.obsoleteFiles == 1);
    CHECK(restoreSpace(base.string(), rep) == 1);

    fs::remove_all(base);
}

// ---------------------------------------------------------------------------
// IsoImageDoc — "Open ISO to [Explore/Modify]": open, modify, save, reopen
// ---------------------------------------------------------------------------
static void testIsoEdit() {
    namespace fs = std::filesystem;
    const fs::path base = fs::temp_directory_path() / "usbutils_isoedit";
    fs::create_directories(base);
    const fs::path src = base / "src.iso";
    writeTestIso(src, "SLUS_202.65", 2048 * 2);

    // ---- hostile input: a 222-byte identifier must be dropped ------------
    // It passes the reader-side "fits in recLen" check, but the writer
    // cannot round-trip it (33+222+1 = 256 overflows the 1-byte record
    // length) — keeping it would emit a len=0 record on save.
    {
        const fs::path hostile = base / "hostile.iso";
        writeTestIso(hostile, "SLUS_202.65", 2048, /*withExtras*/ false);
        FILE* f = std::fopen(hostile.string().c_str(), "r+b");
        CHECK(f);
        if (f) {
            std::vector<uint8_t> rec(255, 0);
            rec[0]  = 255;                        // record length
            rec[32] = 222;                        // identifier length
            std::memset(rec.data() + 33, 'A', 222);
            // root records end at 34 + 34 + 46 = 114 in sector 18
            std::fseek(f, static_cast<long>(18ull * 2048 + 114), SEEK_SET);
            CHECK(std::fwrite(rec.data(), 1, rec.size(), f) == rec.size());
            std::fclose(f);
        }
        IsoImageDoc hdoc;
        CHECK(hdoc.open(hostile.string()));
        size_t longNames = 0, cnfSeen = 0;
        hdoc.forEachNode([&](const IsoNode* n, int) {
            if (n->name.find("AAAA") != std::string::npos) ++longNames;
            if (n->name == "SYSTEM.CNF") ++cnfSeen;
        });
        CHECK(longNames == 0);                   // the 222-byte name is gone
        CHECK(cnfSeen == 1);                     // the rest of the tree stays
    }

    IsoImageDoc doc;
    CHECK(doc.open(src.string()));
    CHECK(doc.volumeId() == "TEST_VOLUME");
    CHECK(doc.root() && doc.root()->isDir);
    // root children: SYSTEM.CNF, MOVIES, IOPRP300.IMG
    CHECK(doc.root()->children.size() == 3);
    CHECK(!doc.dirty());

    // ---- explore: extract SYSTEM.CNF and verify ---------------------------
    const IsoNode* cnf = nullptr;
    const IsoNode* movies = nullptr;
    for (const auto& c : doc.root()->children) {
        if (c->name == "SYSTEM.CNF") cnf = c.get();
        if (c->name == "MOVIES")     movies = c.get();
    }
    CHECK(cnf && movies);
    CHECK(cnf->size() == 96);
    CHECK(movies->children.size() == 1);            // INTRO.PSS
    CHECK(movies->children[0]->size() == 3 * 2048);

    const fs::path extracted = base / "system.cnf";
    CHECK(doc.extractTo(cnf, extracted.string()));
    {
        FILE* f = std::fopen(extracted.string().c_str(), "rb");
        CHECK(f);
        if (f) {
            char buf[97] = {};
            std::fread(buf, 1, 96, f);
            std::fclose(f);
            CHECK(std::strstr(buf, "BOOT2") != nullptr);
            CHECK(std::strstr(buf, "SLUS_202.65") != nullptr);
        }
    }

    // ---- modify: add a directory + a file, delete IOPRP -------------------
    IsoNode* newDir = doc.addDir(doc.root(), "MODS");
    CHECK(newDir);
    const fs::path payload = base / "payload.bin";
    {
        std::vector<uint8_t> data(5000);
        for (size_t i = 0; i < data.size(); ++i) data[i] = uint8_t(i * 7);
        FILE* f = std::fopen(payload.string().c_str(), "wb");
        std::fwrite(data.data(), 1, data.size(), f);
        std::fclose(f);
    }
    IsoNode* added = doc.addFile(newDir, payload.string(), "DUMMY.BIN");
    CHECK(added);
    CHECK(added->isoName == "DUMMY.BIN;1");
    CHECK(added->size() == 5000);
    CHECK(doc.dirty());

    IsoNode* ioprp = nullptr;
    for (const auto& c : doc.root()->children)
        if (c->name == "IOPRP300.IMG") ioprp = c.get();
    CHECK(ioprp);
    CHECK(doc.removeChild(doc.root(), ioprp));

    // rename SYSTEM.CNF -> SYSTEM.CNF (identity through sanitiser)
    CHECK(doc.renameNode(added, "dummy.bin"));
    CHECK(added->isoName == "DUMMY.BIN;1");          // uppercased

    // ---- save + reopen ------------------------------------------------------
    const fs::path out = base / "mod.iso";
    std::string err;
    CHECK(doc.saveAs(out.string(), nullptr, &err));
    if (!std::filesystem::exists(out)) CHECK(false);

    // reopen with the plain reader: serial + new structure must be intact
    IsoInfo info = readIsoInfo(out.string());
    CHECK(info.valid);
    CHECK(info.serial == "SLUS_202.65");

    std::map<std::string, uint64_t> entries;
    {
        FILE* wf = std::fopen(out.string().c_str(), "rb");
        CHECK(wf != nullptr);
        if (wf) {
            const IsoReadFn read = [wf](uint64_t off, void* dst, size_t n) {
                return usbutils::seek64(wf, static_cast<int64_t>(off))
                    && std::fread(dst, 1, n, wf) == n;
            };
            const bool walked = walkIso(read, [&](const IsoEntry& e) {
                entries[e.path] = e.size;
                return true;
            });
            CHECK(walked);
            std::fclose(wf);
        }
    }
    CHECK(entries.count("SYSTEM.CNF") == 1);
    CHECK(entries.count("MOVIES\\INTRO.PSS") == 1);
    CHECK(entries["MOVIES\\INTRO.PSS"] == 3 * 2048);
    CHECK(entries.count("MODS\\DUMMY.BIN") == 1);
    CHECK(entries["MODS\\DUMMY.BIN"] == 5000);
    CHECK(entries.count("IOPRP300.IMG") == 0);       // deleted

    // file content survived the rewrite (extract through a fresh doc)
    IsoImageDoc doc2;
    CHECK(doc2.open(out.string()));
    const IsoNode* dummy = nullptr;
    for (const auto& c : doc2.root()->children)
        if (c->name == "MODS") dummy = c->children.empty() ? nullptr : c->children[0].get();
    CHECK(dummy);
    if (dummy) {
        const fs::path dummyOut = base / "dummy.out";
        CHECK(doc2.extractTo(dummy, dummyOut.string()));
        FILE* f = std::fopen(dummyOut.string().c_str(), "rb");
        CHECK(f);
        if (f) {                                      // verify pattern i*7
            bool match = true;
            for (size_t i = 0; i < 5000; ++i) {
                int b = std::fgetc(f);
                if (b != int(uint8_t(i * 7))) { match = false; break; }
            }
            std::fclose(f);
            CHECK(match);
        }
    }

    fs::remove_all(base);
}

// ---------------------------------------------------------------------------
// IsoImageDoc — multi-extent (>4 GiB) file records must merge into ONE node
// in both on-disk forms: the ECMA-119 form (0x80 on every record except the
// last) and the legacy form our own pre-0.0.7 writer produced (0x80 on every
// record including the last).
// ---------------------------------------------------------------------------
static void testIsoEditMultiExtent() {
    namespace fs = std::filesystem;
    const uint32_t sector = 2048;

    for (int form = 0; form < 2; ++form) {
        const bool legacyLastFlag = (form == 1);   // last record also 0x80
        const fs::path base =
            fs::temp_directory_path() / "usbutils_multiext";
        fs::create_directories(base);
        const fs::path src = base / "multi.iso";

        // image skeleton: PVD + root dir + two data extents at LBA 20/21
        std::vector<uint8_t> img(16 * sector, 0);
        uint8_t pvd[sector] = {};
        pvd[0] = 1;
        std::memcpy(pvd + 1, "CD001", 5);
        pvd[6] = 1;
        std::memset(pvd + 40, ' ', 32);
        std::memcpy(pvd + 40, "MULTIEXT", 8);
        const uint32_t totalSectors = 24;
        pvd[80] = totalSectors & 0xFF;
        pvd[81] = (totalSectors >> 8) & 0xFF;
        pvd[82] = (totalSectors >> 16) & 0xFF;
        pvd[83] = (totalSectors >> 24) & 0xFF;
        uint8_t* root = pvd + 156;
        root[0] = 34; root[1] = 0;
        const uint32_t rootLba = 18, rootSize = sector;
        std::memcpy(root + 2, &rootLba, 4);
        std::memcpy(root + 10, &rootSize, 4);
        root[18] = 120;
        root[25] = 2;
        root[28] = 1; root[30] = 1;
        root[32] = 1;
        img.insert(img.end(), pvd, pvd + sector);
        img.resize(18 * sector, 0);

        // root directory: "." / ".." + BIG.BIN;1 split over two sections
        std::vector<uint8_t> dir(sector, 0);
        size_t off = 0;
        off += putDirRecord(dir, off, rootLba, rootSize, 2, std::string(1, '\0'));
        off += putDirRecord(dir, off, rootLba, rootSize, 2, std::string(1, '\1'));
        off += putDirRecord(dir, off, 20, sector, 0x80, "BIG.BIN;1");
        off += putDirRecord(dir, off, 21, 2 * sector,
                            legacyLastFlag ? 0x80 : 0x00, "BIG.BIN;1");
        img.insert(img.end(), dir.begin(), dir.end());
        img.resize(20 * sector, 0);

        // extent payloads with distinct patterns
        for (uint32_t i = 0; i < sector; ++i)
            img.push_back(static_cast<uint8_t>(i & 0xFF));           // LBA 20
        for (uint32_t i = 0; i < 2 * sector; ++i)
            img.push_back(static_cast<uint8_t>((i * 3 + 1) & 0xFF)); // LBA 21

        {
            FILE* f = std::fopen(src.string().c_str(), "wb");
            std::fwrite(img.data(), 1, img.size(), f);
            std::fclose(f);
        }

        IsoImageDoc doc;
        CHECK(doc.open(src.string()));
        CHECK(doc.root()->children.size() == 1);   // ONE node, not two
        if (doc.root()->children.size() == 1) {
            const IsoNode* big = doc.root()->children[0].get();
            CHECK(big->name == "BIG.BIN");
            CHECK(big->size() == 3 * sector);      // both sections merged
            CHECK(big->runs.size() == 2);
            CHECK(!big->isExternal());

            // extract and verify the content spans both extents in order
            const fs::path out = base / "big.out";
            CHECK(doc.extractTo(big, out.string()));
            FILE* f = std::fopen(out.string().c_str(), "rb");
            CHECK(f);
            if (f) {
                bool match = true;
                for (uint32_t i = 0; i < sector; ++i)
                    if (std::fgetc(f) != int(i & 0xFF)) { match = false; break; }
                for (uint32_t i = 0; i < 2 * sector; ++i)
                    if (std::fgetc(f) != int((i * 3 + 1) & 0xFF)) { match = false; break; }
                std::fclose(f);
                CHECK(match);
            }
        }
        fs::remove_all(base);
    }
}

// ---------------------------------------------------------------------------
// support-file registry ("Useful acquired")
// ---------------------------------------------------------------------------
static void testSupportFiles() {
    const auto& reg = supportFileRegistry();
    CHECK(reg.size() == 10);                        // 6 usbutil.* + 4 IOPRP
    size_t ioprp = 0, required = 0;
    for (const auto& f : reg) {
        if (f.role == SupportRole::IoprpImage) ++ioprp;
        if (f.required) ++required;
    }
    CHECK(ioprp == 4);
    CHECK(required == 4);                           // PS2LOAD, dni, dna, ljs

    const auto st = checkSupportFiles({"../data"}); // repo data/ from build dir
    size_t found = 0, verified = 0;
    for (const auto& s : st) {
        if (s.found) ++found;
        if (s.verified) ++verified;
    }
    CHECK(found == 10);                             // repo ships the full set
    CHECK(verified == 10);                          // byte-identical originals

    // findSupportFile
    CHECK(!findSupportFile("usbutil.dna", {"../data"}).empty());
    CHECK(findSupportFile("NO_SUCH_FILE.XYZ", {"../data"}).empty());
}

static void testVersionCompare() {
    CHECK(isVersionNewer("0.0.2", "v0.0.3"));
    CHECK(isVersionNewer("0.0.2", "0.0.3"));
    CHECK(isVersionNewer("0.0.9", "v0.0.10"));
    CHECK(isVersionNewer("0.0.2", "1.0.0"));
    CHECK(!isVersionNewer("0.0.3", "v0.0.3"));
    CHECK(!isVersionNewer("0.0.3", "v0.0.2"));
    CHECK(!isVersionNewer("0.0.3", "V0.0.3"));
    CHECK(!isVersionNewer("1.2.3", "1.2.3"));
    CHECK(isVersionNewer("1.2", "1.2.1"));
    CHECK(!isVersionNewer("1.2.1", "1.2"));
}

// ---------------------------------------------------------------------------
// iso shrink (scene-style rebuild: relink duplicates, collapse zero files,
// compact, truncate)
// ---------------------------------------------------------------------------

/// Full-structure ISO9660 image: descriptor terminator, L/M + optional
/// path tables, a DUMMIES subdirectory holding two byte-identical files
/// (DUP1/DUP2), two all-zero dummy files of different sizes, a unique
/// file, a bigger file, an unreferenced 2-sector gap and 6 sectors of
/// trailing slack. 47 sectors — the shrink must compact it to 36.
static void writeShrinkIso(const fs::path& p, const std::string& serial) {
    const uint32_t sector = 2048;
    const uint32_t rootLba = 22, dummiesLba = 23, cnfLba = 24;
    const uint32_t dup1Lba = 25, dup2Lba = 29;        // 2 sectors each
    const uint32_t dummy1Lba = 31;                    // 3 sectors (zero)
    const uint32_t dummy2Lba = 34;                    // 1 sector  (zero)
    const uint32_t uniqueLba = 35;                    // 100 bytes
    const uint32_t bigLba = 36;                       // 5 sectors
    const uint32_t totalSectors = 47;

    std::vector<uint8_t> img(16 * sector, 0);

    auto put32le = [](uint8_t* d, uint32_t v) {
        d[0] = uint8_t(v); d[1] = uint8_t(v >> 8);
        d[2] = uint8_t(v >> 16); d[3] = uint8_t(v >> 24);
    };
    auto put32be = [](uint8_t* d, uint32_t v) {
        d[0] = uint8_t(v >> 24); d[1] = uint8_t(v >> 16);
        d[2] = uint8_t(v >> 8); d[3] = uint8_t(v);
    };
    auto putPt = [](std::vector<uint8_t>& t, bool be, uint8_t di,
                    uint32_t lba, const char* name) {
        t.push_back(di); t.push_back(0);
        if (be) for (int i = 3; i >= 0; --i) t.push_back(uint8_t(lba >> (8 * i)));
        else     for (int i = 0; i < 4; ++i)  t.push_back(uint8_t(lba >> (8 * i)));
        if (be) { t.push_back(0); t.push_back(1); }   // parent index 1 (BE)
        else    { t.push_back(1); t.push_back(0); }   // parent index 1 (LE)
        for (uint8_t i = 0; i < di; ++i) t.push_back(uint8_t(name[i]));
        if (di & 1) t.push_back(0);
    };

    // ---- PVD at 16 --------------------------------------------------------
    uint8_t pvd[sector] = {};
    pvd[0] = 1;
    std::memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    std::memset(pvd + 40, ' ', 32);
    std::memcpy(pvd + 40, "SHRINK_TEST", 11);
    put32le(pvd + 80, totalSectors);
    put32be(pvd + 84, totalSectors);
    pvd[128] = 0x00; pvd[129] = 0x08;                // block size 2048 (LE)
    pvd[130] = 0x08; pvd[131] = 0x00;                // ... and (BE)
    put32le(pvd + 132, 26);                          // path table size
    put32be(pvd + 136, 26);
    put32le(pvd + 140, 18);                          // L table
    put32le(pvd + 144, 20);                          // optional L
    put32be(pvd + 148, 19);                          // M table
    put32be(pvd + 152, 21);                          // optional M
    uint8_t* root = pvd + 156;
    root[0] = 34;
    put32le(root + 2, rootLba);
    put32le(root + 10, sector);
    root[18] = 120; root[25] = 2; root[28] = 1; root[30] = 1; root[32] = 1;
    img.insert(img.end(), pvd, pvd + sector);

    // sector 17: terminator; 18-21: path tables (L, M, optL, optM)
    {
        uint8_t term[sector] = {};
        term[0] = 255;
        std::memcpy(term + 1, "CD001", 5);
        term[6] = 1;
        img.insert(img.end(), term, term + sector);
    }
    std::vector<uint8_t> ptL, ptM;
    putPt(ptL, false, 1, rootLba, "\0");
    putPt(ptL, false, 7, dummiesLba, "DUMMIES");
    putPt(ptM, true, 1, rootLba, "\0");
    putPt(ptM, true, 7, dummiesLba, "DUMMIES");
    CHECK(ptL.size() == 26 && ptM.size() == 26);
    for (int s = 0; s < 2; ++s) {
        std::vector<uint8_t> pad(sector, 0);
        std::memcpy(pad.data(), ptL.data(), ptL.size());
        img.insert(img.end(), pad.begin(), pad.end());
        std::fill(pad.begin(), pad.end(), 0);
        std::memcpy(pad.data(), ptM.data(), ptM.size());
        img.insert(img.end(), pad.begin(), pad.end());
    }
    CHECK(img.size() == 22ull * sector);

    // ---- root directory at 22 ---------------------------------------------
    {
        std::vector<uint8_t> dir(sector, 0);
        size_t off = 0;
        off += putDirRecord(dir, off, rootLba, sector, 2, std::string(1, '\0'));
        off += putDirRecord(dir, off, rootLba, sector, 2, std::string(1, '\1'));
        off += putDirRecord(dir, off, cnfLba, 96, 0, "SYSTEM.CNF;1");
        off += putDirRecord(dir, off, dummiesLba, sector, 2, "DUMMIES");
        img.insert(img.end(), dir.begin(), dir.end());
    }

    // ---- DUMMIES at 23 ----------------------------------------------------
    {
        std::vector<uint8_t> dir(sector, 0);
        size_t off = 0;
        off += putDirRecord(dir, off, dummiesLba, sector, 2, std::string(1, '\0'));
        off += putDirRecord(dir, off, rootLba, sector, 2, std::string(1, '\1'));
        off += putDirRecord(dir, off, dup1Lba, 2 * sector, 0, "DUP1.BIN;1");
        off += putDirRecord(dir, off, dup2Lba, 2 * sector, 0, "DUP2.BIN;1");
        off += putDirRecord(dir, off, dummy1Lba, 3 * sector, 0, "DUMMY1.DAT;1");
        off += putDirRecord(dir, off, dummy2Lba, sector, 0, "DUMMY2.DAT;1");
        off += putDirRecord(dir, off, uniqueLba, 100, 0, "UNIQUE.DAT;1");
        off += putDirRecord(dir, off, bigLba, 5 * sector, 0, "BIG.DAT;1");
        img.insert(img.end(), dir.begin(), dir.end());
    }

    // ---- files ------------------------------------------------------------
    img.resize(cnfLba * sector, 0);
    std::string cnf = "BOOT2 = cdrom0:\\" + serial + ";1\nVER = 1.00\n";
    cnf.resize(96, ' ');
    img.insert(img.end(), cnf.begin(), cnf.end());

    img.resize(dup1Lba * sector, 0);
    std::vector<uint8_t> dup(2 * sector);
    for (size_t i = 0; i < dup.size(); ++i)
        dup[i] = uint8_t((i * 17 + 3) & 0xFF);
    img.insert(img.end(), dup.begin(), dup.end());

    img.resize(dup2Lba * sector, 0);                 // sectors 27-28 stay zero (gap)
    img.insert(img.end(), dup.begin(), dup.end());

    img.resize(dummy1Lba * sector, 0);
    img.insert(img.end(), 3ull * sector, 0);         // DUMMY1 zeros
    img.insert(img.end(), 1ull * sector, 0);         // DUMMY2 zeros

    img.resize(uniqueLba * sector, 0);
    std::vector<uint8_t> uniq(100);
    for (size_t i = 0; i < uniq.size(); ++i)
        uniq[i] = uint8_t((i * 29 + 7) & 0xFF);
    img.insert(img.end(), uniq.begin(), uniq.end());

    img.resize(bigLba * sector, 0);
    std::vector<uint8_t> big(5 * sector);
    for (size_t i = 0; i < big.size(); ++i)
        big[i] = uint8_t((i * 11 + 13) & 0xFF);
    img.insert(img.end(), big.begin(), big.end());

    img.resize(totalSectors * sector, 0xCC);         // trailing slack

    FILE* f = std::fopen(p.string().c_str(), "wb");
    std::fwrite(img.data(), 1, img.size(), f);
    std::fclose(f);
}

/// Walk a game's stream into a path -> entry map of its FILES.
static std::map<std::string, IsoEntry> walkGameFiles(GameStream& s) {
    std::map<std::string, IsoEntry> out;
    const IsoReadFn read = [&s](uint64_t off, void* dst, size_t n) {
        return s.read(off, dst, n) == n;
    };
    walkIso(read, [&](const IsoEntry& e) {
        if (!e.isDir) out[e.path] = e;
        return true;
    });
    return out;
}

static std::vector<uint8_t> readRange(GameStream& s, uint64_t off, size_t n) {
    std::vector<uint8_t> buf(n);
    if (s.read(off, buf.data(), n) != n) buf.clear();
    return buf;
}

static void testShrink() {
    const fs::path base = fs::temp_directory_path() / "usbutils_shrink";
    fs::remove_all(base);
    fs::create_directories(base);
    const fs::path iso = base / "game.iso";
    writeShrinkIso(iso, "SLUS_202.65");
    CHECK(fs::file_size(iso) == 47ull * 2048);

    InstallOptions opt;
    opt.gameName = "Shrink Me";
    const InstallResult ir = installIso(iso.string(), base.string(), opt);
    CHECK(ir.ok);
    if (!ir.ok) { fs::remove_all(base); return; }

    // ---- analysis ---------------------------------------------------------
    ShrinkInfo info = analyzeShrink(base.string(), ir.idHex, ir.serial,
                                    ir.parts);
    CHECK(info.ok);
    CHECK(!info.hasRipBackups);
    CHECK(!info.ps2LoadInjected);
    CHECK(info.currentBytes == 47ull * 2048);
    CHECK(info.duplicateBytes == 2ull * 2048);       // DUP2 == DUP1
    CHECK(info.duplicateFiles == 1);
    CHECK(info.zeroBytes == 2048);                   // DUMMY2 -> DUMMY1
    CHECK(info.zeroFiles == 1);
    CHECK(info.finalBytes == 36ull * 2048);
    CHECK(info.gapBytes == 8ull * 2048);             // 2-sector gap + 6 slack
    CHECK(info.savings() == 11ull * 2048);

    // ---- apply ------------------------------------------------------------
    const ShrinkResult r = shrinkGame(base.string(), ir.idHex, ir.serial,
                                      ir.parts, /*discardBackups*/ false);
    CHECK(r.ok);
    CHECK(r.verified);
    CHECK(r.oldBytes == 47ull * 2048);
    CHECK(r.newBytes == 36ull * 2048);
    CHECK(r.oldParts == 1 && r.newParts == 1);
    if (!r.ok) { fs::remove_all(base); return; }

    GameStream s;
    CHECK(s.open(base.string(), ir.idHex, ir.serial, 1));
    CHECK(s.size() == 36ull * 2048);

    // every file still there, same size
    const auto files = walkGameFiles(s);
    CHECK(files.size() == 7);
    CHECK(files.count("SYSTEM.CNF") && files.at("SYSTEM.CNF").size == 96);
    CHECK(files.count("DUMMIES\\DUP1.BIN")
          && files.at("DUMMIES\\DUP1.BIN").size == 2ull * 2048);
    CHECK(files.count("DUMMIES\\DUP2.BIN")
          && files.at("DUMMIES\\DUP2.BIN").size == 2ull * 2048);
    CHECK(files.count("DUMMIES\\DUMMY1.DAT")
          && files.at("DUMMIES\\DUMMY1.DAT").size == 3ull * 2048);
    CHECK(files.count("DUMMIES\\DUMMY2.DAT")
          && files.at("DUMMIES\\DUMMY2.DAT").size == 2048);
    CHECK(files.count("DUMMIES\\UNIQUE.DAT")
          && files.at("DUMMIES\\UNIQUE.DAT").size == 100);
    CHECK(files.count("DUMMIES\\BIG.DAT")
          && files.at("DUMMIES\\BIG.DAT").size == 5ull * 2048);

    // relinks: DUP2 shares DUP1's extent, DUMMY2 shares DUMMY1's
    CHECK(files.at("DUMMIES\\DUP2.BIN").lba
          == files.at("DUMMIES\\DUP1.BIN").lba);
    CHECK(files.at("DUMMIES\\DUMMY2.DAT").lba
          == files.at("DUMMIES\\DUMMY1.DAT").lba);

    const auto dup1 = readRange(
        s, uint64_t(files.at("DUMMIES\\DUP1.BIN").lba) * 2048, 2 * 2048);
    const auto dup2 = readRange(
        s, uint64_t(files.at("DUMMIES\\DUP2.BIN").lba) * 2048, 2 * 2048);
    CHECK(dup1.size() == 2 * 2048 && dup1 == dup2);
    CHECK(dup1[0] == 3 && dup1[1] == 20 && dup1[2] == 37);   // original pattern
    const auto d2 = readRange(
        s, uint64_t(files.at("DUMMIES\\DUMMY2.DAT").lba) * 2048, 2048);
    bool zeros = !d2.empty();
    for (uint8_t b : d2) if (b) zeros = false;
    CHECK(zeros);
    const auto cnf = readRange(
        s, uint64_t(files.at("SYSTEM.CNF").lba) * 2048, 96);
    CHECK(cnf.size() == 96
          && std::memcmp(cnf.data(), "BOOT2 = cdr", 11) == 0);
    const auto uniq = readRange(
        s, uint64_t(files.at("DUMMIES\\UNIQUE.DAT").lba) * 2048, 100);
    CHECK(uniq.size() == 100 && uniq[0] == 7 && uniq[1] == 36);

    // every extent inside the new volume
    for (const auto& kv : files)
        CHECK(kv.second.lba + uint32_t((kv.second.size + 2047) / 2048) <= 36);

    // PVD: volume space + root record + path table pointers
    {
        const auto pvd = readRange(s, 16ull * 2048, 2048);
        CHECK(pvd.size() == 2048);
        CHECK(pvd[80] == 36 && pvd[81] == 0 && pvd[82] == 0 && pvd[83] == 0);
        CHECK(pvd[84] == 0 && pvd[85] == 0 && pvd[86] == 0 && pvd[87] == 36);
        CHECK(pvd[158] == 22 && pvd[159] == 0);          // root LBA (LE)
        CHECK(pvd[140] == 18 && pvd[144] == 20);         // L/opt-L table LBAs
        CHECK(pvd[148] == 0 && pvd[149] == 0 && pvd[150] == 0 && pvd[151] == 19);
        CHECK(pvd[152] == 0 && pvd[153] == 0 && pvd[154] == 0 && pvd[155] == 21);

        // path tables: root + DUMMIES entries point at the new dir LBAs
        const auto ptl = readRange(s, 18ull * 2048, 2048);
        CHECK(ptl.size() == 2048);
        CHECK(ptl[0] == 1);                              // root: di = 1
        CHECK(ptl[2] == 22);                             // root LBA (LE)
        CHECK(ptl[10] == 7);                             // DUMMIES: di = 7
        CHECK(ptl[12] == 23);                            // DUMMIES LBA (LE)
        const auto ptm = readRange(s, 19ull * 2048, 2048);
        CHECK(ptm.size() == 2048);
        CHECK(ptm[12] == 0 && ptm[13] == 0 && ptm[14] == 0 && ptm[15] == 23);
        const auto optl = readRange(s, 20ull * 2048, 2048);
        CHECK(optl.size() == 2048 && optl[12] == 23);    // optional = copy
    }

    // a second shrink has nothing left to take — and the analysis must not
    // count already-relinked copies as reclaimable (uint64 underflow guard)
    ShrinkInfo again2 = analyzeShrink(base.string(), ir.idHex, ir.serial, 1);
    CHECK(again2.ok);
    CHECK(again2.savings() == 0);
    CHECK(again2.duplicateBytes == 0);
    CHECK(again2.zeroBytes == 0);
    CHECK(again2.gapBytes == 0);
    const ShrinkResult again = shrinkGame(base.string(), ir.idHex, ir.serial,
                                          1, false);
    CHECK(again.ok);
    CHECK(again.newBytes == again.oldBytes);

    fs::remove_all(base);
}

static void testShrinkParts() {
    const fs::path base = fs::temp_directory_path() / "usbutils_shrink_parts";
    fs::remove_all(base);
    fs::create_directories(base);
    const fs::path iso = base / "game.iso";
    writeShrinkIso(iso, "SLUS_203.99");

    // hand-split the 47-sector image into 10-sector chunk files
    const std::string name = "Chunked Game";
    const std::string id = gameIdHex(name), serial = "SLUS_203.99";
    {
        FILE* f = std::fopen(iso.string().c_str(), "rb");
        CHECK(f != nullptr);
        std::vector<uint8_t> buf(10ull * 2048);
        for (uint32_t part = 0; part < 5; ++part) {
            const size_t want = part < 4
                ? buf.size() : size_t(47ull * 2048 - 4ull * buf.size());
            CHECK(std::fread(buf.data(), 1, want, f) == want);
            FILE* o = std::fopen(
                (base / chunkFileName(id, serial, part)).string().c_str(),
                "wb");
            std::fwrite(buf.data(), 1, want, o);
            std::fclose(o);
        }
        std::fclose(f);

        UlCfg cfg;
        cfg.dir = base.string();
        UlCfgRecord rec;
        rec.setName(name);
        rec.setSerial(serial);
        rec.parts = 5;
        rec.media = kMediaDvd;
        cfg.records = {rec};
        CHECK(cfg.save());
    }

    const ShrinkResult r = shrinkGame(base.string(), id, serial, 5, false);
    CHECK(r.ok);
    CHECK(r.verified);
    CHECK(r.oldBytes == 47ull * 2048);
    CHECK(r.newBytes == 36ull * 2048);
    CHECK(r.oldParts == 5);
    CHECK(r.newParts == 4);
    if (!r.ok) { fs::remove_all(base); return; }

    CHECK(fs::file_size(base / chunkFileName(id, serial, 0)) == 10ull * 2048);
    CHECK(fs::file_size(base / chunkFileName(id, serial, 3)) == 12288);
    CHECK(!fs::exists(base / chunkFileName(id, serial, 4)));

    UlCfg cfg;
    CHECK(cfg.load(base.string()));
    CHECK(cfg.records.size() == 1 && cfg.records[0].parts == 4);

    GameStream s;
    CHECK(s.open(base.string(), id, serial, 4));
    CHECK(s.size() == 36ull * 2048);
    const auto files = walkGameFiles(s);
    CHECK(files.size() == 7);
    const auto d1 = readRange(
        s, uint64_t(files.at("DUMMIES\\DUP1.BIN").lba) * 2048, 2 * 2048);
    const auto d2 = readRange(
        s, uint64_t(files.at("DUMMIES\\DUP2.BIN").lba) * 2048, 2 * 2048);
    CHECK(d1 == d2);

    fs::remove_all(base);
}

static void testShrinkRipInterplay() {
    const fs::path base = fs::temp_directory_path() / "usbutils_shrink_rip";
    fs::remove_all(base);
    fs::create_directories(base);
    const fs::path iso = base / "game.iso";

    std::vector<uint8_t> intro(3ull * 2048), logo(2ull * 2048);
    for (size_t i = 0; i < intro.size(); ++i)
        intro[i] = uint8_t((i * 13 + 5) & 0xFF);
    for (size_t i = 0; i < logo.size(); ++i)
        logo[i] = uint8_t((i * 7 + 9) & 0xFF);
    writeRipIso(iso, "SLUS_204.77", {{"INTRO.PSS", intro},
                                     {"LOGO.PSS", logo}});

    InstallOptions opt;
    opt.gameName = "Rip Shrink";
    const InstallResult ir = installIso(iso.string(), base.string(), opt);
    CHECK(ir.ok);
    if (!ir.ok) { fs::remove_all(base); return; }

    // rip with backup -> shrink must refuse (sidecar offsets would go stale)
    {
        auto vids = scanVideos(base.string(), ir.idHex, ir.serial, ir.parts);
        CHECK(vids.size() == 2);
        const RipResult rr = ripVideos(base.string(), ir.idHex, ir.serial,
                                       ir.parts, vids, /*makeBackup*/ true);
        CHECK(rr.ok && rr.rippedCount == 2);

        ShrinkInfo info = analyzeShrink(base.string(), ir.idHex, ir.serial,
                                        ir.parts);
        CHECK(info.ok);
        CHECK(info.hasRipBackups);

        const ShrinkResult r = shrinkGame(base.string(), ir.idHex, ir.serial,
                                          ir.parts, false);
        CHECK(!r.ok);
        CHECK(r.error.find("backup") != std::string::npos);
    }

    // restore first -> shrink works, media stay original
    {
        const RipResult rr = restoreVideos(base.string(), ir.idHex, ir.serial,
                                           ir.parts);
        CHECK(rr.ok);
        const ShrinkResult r = shrinkGame(base.string(), ir.idHex, ir.serial,
                                          ir.parts, false);
        CHECK(r.ok);
        CHECK(r.verified);
        CHECK(r.newBytes < r.oldBytes);

        auto vids = scanVideos(base.string(), ir.idHex, ir.serial, ir.parts);
        CHECK(vids.size() == 2);
        bool anyBlanked = false;
        uint64_t total = 0;
        for (const auto& v : vids) {
            anyBlanked = anyBlanked || v.blanked;
            total += v.size;
        }
        CHECK(!anyBlanked);
        CHECK(total == 5ull * 2048);

        GameStream s;
        CHECK(s.open(base.string(), ir.idHex, ir.serial, ir.parts));
        const auto files = walkGameFiles(s);
        CHECK(files.count("MOVIES\\INTRO.PSS"));
        const auto back = readRange(
            s, uint64_t(files.at("MOVIES\\INTRO.PSS").lba) * 2048, 3 * 2048);
        CHECK(back.size() == 3 * 2048 && back[0] == 5 && back[1] == 18);
    }

    // rip again, then discard the backups while shrinking: media stay
    // blanked, the sidecar is gone and unrip reports no backup
    {
        auto vids = scanVideos(base.string(), ir.idHex, ir.serial, ir.parts);
        const RipResult rr = ripVideos(base.string(), ir.idHex, ir.serial,
                                       ir.parts, vids, true);
        CHECK(rr.ok && rr.rippedCount == 2);
        CHECK(videoBackupSize(base.string(), ir.idHex, ir.serial) > 0);

        const ShrinkResult r = shrinkGame(base.string(), ir.idHex, ir.serial,
                                          ir.parts, /*discardBackups*/ true);
        CHECK(r.ok);
        CHECK(r.verified);
        CHECK(r.newBytes < r.oldBytes);
        CHECK(videoBackupSize(base.string(), ir.idHex, ir.serial) == 0);

        auto vids2 = scanVideos(base.string(), ir.idHex, ir.serial, ir.parts);
        CHECK(vids2.size() == 2);
        size_t blanked = 0;
        for (const auto& v : vids2)
            if (v.blanked) ++blanked;
        CHECK(blanked == 2);

        const RipResult un = restoreVideos(base.string(), ir.idHex, ir.serial,
                                           ir.parts);
        CHECK(!un.ok);
    }

    fs::remove_all(base);
}

static void testShrinkRefusals() {
    const fs::path base = fs::temp_directory_path() / "usbutils_shrink_ref";
    fs::remove_all(base);
    fs::create_directories(base);
    const std::string serial = "SLUS_205.11";

    const fs::path proto = base / "proto.iso";
    writeShrinkIso(proto, serial);
    std::vector<uint8_t> img;
    {
        FILE* f = std::fopen(proto.string().c_str(), "rb");
        CHECK(f != nullptr);
        img.resize(static_cast<size_t>(fs::file_size(proto)));
        CHECK(std::fread(img.data(), 1, img.size(), f) == img.size());
        std::fclose(f);
    }

    auto install = [&](const std::vector<uint8_t>& bytes, const char* name) {
        const fs::path p = base / (std::string(name) + ".iso");
        FILE* f = std::fopen(p.string().c_str(), "wb");
        std::fwrite(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
        InstallOptions opt;
        opt.gameName = name;
        return installIso(p.string(), base.string(), opt);
    };

    // find a record inside the DUMMIES directory sector by name
    auto findRecord = [&img](const char* want) -> size_t {
        uint8_t* dir = img.data() + 23ull * 2048;
        size_t off = 0;
        while (off + 1 < 2048) {
            const uint8_t recLen = dir[off];
            if (recLen == 0) break;
            const uint8_t nameLen = dir[off + 32];
            if (recLen < 34 || size_t(nameLen) + 33 > recLen) break;
            if (nameLen >= std::strlen(want)
                && std::strncmp(reinterpret_cast<const char*>(dir + off + 33),
                                want, std::strlen(want)) == 0)
                return 23ull * 2048 + off;
            off += recLen;
        }
        return SIZE_MAX;
    };

    // a) supplementary volume descriptor (Joliet)
    {
        std::vector<uint8_t> v = img;
        v[17 * 2048] = 2;                     // CD001 descriptor of type 2
        const InstallResult ir = install(v, "SVD Game");
        CHECK(ir.ok);
        if (ir.ok) {
            const ShrinkInfo info = analyzeShrink(base.string(), ir.idHex,
                                                  ir.serial, ir.parts);
            CHECK(!info.ok);
            CHECK(info.error.find("supplementary") != std::string::npos);
        }
    }

    // b) UDF descriptors in the set
    {
        std::vector<uint8_t> v = img;
        std::memcpy(v.data() + 17 * 2048 + 1, "BEA01", 5);
        const InstallResult ir = install(v, "UDF Game");
        CHECK(ir.ok);
        if (ir.ok) {
            const ShrinkInfo info = analyzeShrink(base.string(), ir.idHex,
                                                  ir.serial, ir.parts);
            CHECK(!info.ok);
            CHECK(info.error.find("UDF") != std::string::npos);
        }
    }

    // c) Rock Ridge system-use bytes past a record's pad
    {
        std::vector<uint8_t> v = img;
        const size_t rec = findRecord("BIG.DAT");
        CHECK(rec != SIZE_MAX);
        if (rec != SIZE_MAX) {
            // grow the record by two bytes and fill its system-use area
            const uint8_t nameLen = v[rec + 32];
            size_t su = 33 + nameLen;
            if (su % 2) ++su;
            v[rec] = uint8_t(v[rec] + 2);     // BIG.DAT;1 is the last record
            v[rec + su] = 'R';
            v[rec + su + 1] = 'R';
        }
        const InstallResult ir = install(v, "RR Game");
        CHECK(ir.ok);
        if (ir.ok) {
            const ShrinkInfo info = analyzeShrink(base.string(), ir.idHex,
                                                  ir.serial, ir.parts);
            CHECK(!info.ok);
            CHECK(info.error.find("Rock Ridge") != std::string::npos);
        }
    }

    // d) interleaved record (file unit size != 0)
    {
        std::vector<uint8_t> v = img;
        const size_t rec = findRecord("DUP1.BIN");
        CHECK(rec != SIZE_MAX);
        if (rec != SIZE_MAX) v[rec + 26] = 2;
        const InstallResult ir = install(v, "ILV Game");
        CHECK(ir.ok);
        if (ir.ok) {
            const ShrinkInfo info = analyzeShrink(base.string(), ir.idHex,
                                                  ir.serial, ir.parts);
            CHECK(!info.ok);
            CHECK(info.error.find("interleaved") != std::string::npos);
        }
    }

    fs::remove_all(base);
}

static void testShrinkCfgFailure() {
    const fs::path base = fs::temp_directory_path() / "usbutils_shrink_cfg";
    fs::remove_all(base);
    fs::create_directories(base);
    const fs::path iso = base / "game.iso";
    writeShrinkIso(iso, "SLUS_206.44");

    const std::string name = "Cfg Fail";
    const std::string id = gameIdHex(name), serial = "SLUS_206.44";
    {
        // hand-split the 47-sector image into 10-sector chunk files
        FILE* f = std::fopen(iso.string().c_str(), "rb");
        CHECK(f != nullptr);
        std::vector<uint8_t> buf(10ull * 2048);
        for (uint32_t part = 0; part < 5; ++part) {
            const size_t want = part < 4
                ? buf.size() : size_t(47ull * 2048 - 4ull * buf.size());
            CHECK(std::fread(buf.data(), 1, want, f) == want);
            FILE* o = std::fopen(
                (base / chunkFileName(id, serial, part)).string().c_str(),
                "wb");
            std::fwrite(buf.data(), 1, want, o);
            std::fclose(o);
        }
        std::fclose(f);

        UlCfg cfg;
        cfg.dir = base.string();
        UlCfgRecord rec;
        rec.setName(name);
        rec.setSerial(serial);
        rec.parts = 5;
        rec.media = kMediaDvd;
        cfg.records = {rec};
        CHECK(cfg.save());
    }

    // ul.cfg cannot be written (directory read-only): the rewrite must
    // stop after the cut and keep every chunk file on disk
    fs::permissions(base, fs::perms::owner_read | fs::perms::owner_exec
                              | fs::perms::group_read | fs::perms::group_exec
                              | fs::perms::others_read | fs::perms::others_exec);
    const ShrinkResult r = shrinkGame(base.string(), id, serial, 5, false);
    fs::permissions(base, fs::perms::owner_all);
    CHECK(!r.ok);
    CHECK(r.error.find("ul.cfg") != std::string::npos);
    CHECK(r.error.find("still on disk") != std::string::npos);
    CHECK(r.newBytes == 36ull * 2048);
    CHECK(r.newParts == 4);
    CHECK(r.oldParts == 5);

    // the loader's chunk files are all still there — the game keeps booting
    for (uint32_t i = 0; i < 5; ++i)
        CHECK(fs::exists(base / chunkFileName(id, serial, i)));

    // the user fixes the parts field by hand and drops the surplus file
    {
        UlCfg cfg;
        CHECK(cfg.load(base.string()));
        CHECK(cfg.records.size() == 1 && cfg.records[0].parts == 5);
        cfg.records[0].parts = 4;
        CHECK(cfg.save());
        CHECK(fs::remove(base / chunkFileName(id, serial, 4)));
    }

    GameStream s;
    CHECK(s.open(base.string(), id, serial, 4));
    CHECK(s.size() == 36ull * 2048);
    const auto files = walkGameFiles(s);
    CHECK(files.size() == 7);
    const auto d1 = readRange(
        s, uint64_t(files.at("DUMMIES\\DUP1.BIN").lba) * 2048, 2 * 2048);
    const auto d2 = readRange(
        s, uint64_t(files.at("DUMMIES\\DUP2.BIN").lba) * 2048, 2 * 2048);
    CHECK(d1.size() == 2 * 2048 && d1 == d2);

    // ---- the record-disappeared variant stages the real ordering ------
    // (a writable directory lets the pre-fix code DELETE the surplus
    // chunks before it notices the record is gone; with the fix every
    // chunk file is still on disk when the error is reported)
    {
        const fs::path base2 = fs::temp_directory_path() / "usbutils_shrink_cfg2";
        fs::remove_all(base2);
        fs::create_directories(base2);
        const fs::path iso2 = base2 / "game.iso";
        writeShrinkIso(iso2, "SLUS_206.45");

        const std::string name2 = "Cfg Gone";
        const std::string id2 = gameIdHex(name2), serial2 = "SLUS_206.45";
        {
            FILE* f = std::fopen(iso2.string().c_str(), "rb");
            CHECK(f != nullptr);
            std::vector<uint8_t> buf(10ull * 2048);
            for (uint32_t part = 0; part < 5; ++part) {
                const size_t want = part < 4
                    ? buf.size() : size_t(47ull * 2048 - 4ull * buf.size());
                CHECK(std::fread(buf.data(), 1, want, f) == want);
                FILE* o = std::fopen(
                    (base2 / chunkFileName(id2, serial2, part)).string().c_str(),
                    "wb");
                std::fwrite(buf.data(), 1, want, o);
                std::fclose(o);
            }
            std::fclose(f);

            // ul.cfg that lacks this game's record
            UlCfg cfg;
            cfg.dir = base2.string();
            UlCfgRecord other;
            other.setName("Other Game");
            other.setSerial("SLUS_999.99");
            other.parts = 1;
            other.media = kMediaDvd;
            cfg.records = {other};
            CHECK(cfg.save());
        }

        const ShrinkResult r2 = shrinkGame(base2.string(), id2, serial2, 5, false);
        CHECK(!r2.ok);
        CHECK(r2.error.find("still on disk") != std::string::npos);
        CHECK(r2.error.find("parts 4") != std::string::npos);
        for (uint32_t i = 0; i < 5; ++i)
            CHECK(fs::exists(base2 / chunkFileName(id2, serial2, i)));
        CHECK(fs::file_size(base2 / chunkFileName(id2, serial2, 3)) == 12288);

        fs::remove_all(base2);
    }

    fs::remove_all(base);
}

static void testShrinkDiscardBlocked() {
    const fs::path base = fs::temp_directory_path() / "usbutils_shrink_disc";
    fs::remove_all(base);
    fs::create_directories(base);
    const fs::path iso = base / "game.iso";

    std::vector<uint8_t> intro(3ull * 2048), logo(2ull * 2048);
    for (size_t i = 0; i < intro.size(); ++i)
        intro[i] = uint8_t((i * 13 + 5) & 0xFF);
    for (size_t i = 0; i < logo.size(); ++i)
        logo[i] = uint8_t((i * 7 + 9) & 0xFF);
    writeRipIso(iso, "SLUS_207.88", {{"INTRO.PSS", intro},
                                     {"LOGO.PSS", logo}});

    InstallOptions opt;
    opt.gameName = "Discard Blocked";
    const InstallResult ir = installIso(iso.string(), base.string(), opt);
    CHECK(ir.ok);
    if (!ir.ok) { fs::remove_all(base); return; }

    auto vids = scanVideos(base.string(), ir.idHex, ir.serial, ir.parts);
    CHECK(vids.size() == 2);
    const RipResult rr = ripVideos(base.string(), ir.idHex, ir.serial,
                                   ir.parts, vids, /*makeBackup*/ true);
    CHECK(rr.ok && rr.rippedCount == 2);
    CHECK(videoBackupSize(base.string(), ir.idHex, ir.serial) > 0);

    // the sidecar cannot be deleted (directory read-only): --discard-backups
    // must refuse instead of shrinking past a backup it could not remove
    fs::permissions(base, fs::perms::owner_read | fs::perms::owner_exec
                              | fs::perms::group_read | fs::perms::group_exec
                              | fs::perms::others_read | fs::perms::others_exec);
    const ShrinkResult r = shrinkGame(base.string(), ir.idHex, ir.serial,
                                      ir.parts, /*discardBackups*/ true);
    fs::permissions(base, fs::perms::owner_all);
    CHECK(!r.ok);
    CHECK(r.error.find("could not be deleted") != std::string::npos);

    // nothing was touched: the sidecar survives, the game still holds
    // zeros, and the backup still restores byte for byte
    CHECK(videoBackupSize(base.string(), ir.idHex, ir.serial) > 0);
    {
        auto vids2 = scanVideos(base.string(), ir.idHex, ir.serial, ir.parts);
        CHECK(vids2.size() == 2);
        size_t blanked = 0;
        for (const auto& v : vids2)
            if (v.blanked) ++blanked;
        CHECK(blanked == 2);
    }
    const RipResult un = restoreVideos(base.string(), ir.idHex, ir.serial,
                                       ir.parts);
    CHECK(un.ok);
    {
        auto vids3 = scanVideos(base.string(), ir.idHex, ir.serial, ir.parts);
        CHECK(vids3.size() == 2);
        bool anyBlanked = false;
        for (const auto& v : vids3)
            anyBlanked = anyBlanked || v.blanked;
        CHECK(!anyBlanked);
    }

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
    testRenameSafety();
    testInstallCancel();
    testVideoRip();
    testAudioRip();
    testSidecarCompression();
    testIoprpLocate();
    testPs2Load();
    testGameOps();
    testSortAndClean();
    testVersionCompare();
    testIsoEdit();
    testIsoEditMultiExtent();
    testSupportFiles();
    testShrink();
    testShrinkParts();
    testShrinkRipInterplay();
    testShrinkRefusals();
    testShrinkCfgFailure();
    testShrinkDiscardBlocked();
    std::printf(g_failed ? "\n%d FAILURE(S)\n" : "\nALL TESTS PASSED\n", g_failed);
    return g_failed ? 1 : 0;
}
