// SPDX-License-Identifier: MIT
#include "iso9660.h"

#include <cstdio>
#include <cstring>

namespace usbutils {

namespace {

constexpr uint32_t kSectorSize = 2048;

#pragma pack(push, 1)
struct IsoDirEntry {           // ISO9660 directory record (standard layout)
    uint8_t  len;              // 0
    uint8_t  extLen;           // 1
    uint32_t lbaLE, lbaBE;     // 2, 6
    uint32_t sizeLE, sizeBE;   // 10, 14   (32-bit!)
    uint8_t  flags;            // 18
    uint8_t  unitSize;         // 19
    uint8_t  gapSize;          // 20
    uint16_t volSeqLE, volSeqBE; // 21, 23
    uint8_t  nameLen;          // 25
    // 26..32: datetime (7 bytes); 33..: file identifier
};
#pragma pack(pop)

struct FileHandle {
    FILE* f = nullptr;
    ~FileHandle() { if (f) std::fclose(f); }
};

bool readAt(FILE* f, uint64_t off, void* dst, size_t n) {
    return std::fseek(f, static_cast<long>(off), SEEK_SET) == 0
        && std::fread(dst, 1, n, f) == n;
}

} // namespace

bool readSector(const std::string& path, uint32_t lba, uint8_t* out2048) {
    FileHandle fh;
    fh.f = std::fopen(path.c_str(), "rb");
    if (!fh.f) return false;
    return readAt(fh.f, static_cast<uint64_t>(lba) * kSectorSize, out2048, kSectorSize);
}

std::string normalizeSerial(const std::string& raw) {
    std::string s = raw;
    // strip ISO9660 version suffix ";1" and a trailing ".1"
    if (s.size() > 2 && s.compare(s.size() - 2, 2, ";1") == 0)
        s = s.substr(0, s.size() - 2);
    // Original checked char 11 for '.' or '1' and replaced it with '0'
    // (0x3A1072..0x3A109C): pad short serials to 11 chars with '0'.
    if (s.size() < 11) {
        while (s.size() < 11) s += '0';
    } else if (s.size() > 11) {
        s = s.substr(0, 11);
    }
    return s;
}

IsoInfo readIsoInfo(const std::string& path) {
    IsoInfo info;
    FileHandle fh;
    fh.f = std::fopen(path.c_str(), "rb");
    if (!fh.f) return info;

    // ---- Primary Volume Descriptor (sector 16) --------------------------
    uint8_t pvd[kSectorSize];
    if (!readAt(fh.f, 16ull * kSectorSize, pvd, kSectorSize)) return info;
    if (std::memcmp(pvd + 1, "CD001", 5) != 0) return info;   // not ISO9660

    info.volumeSize = static_cast<uint32_t>(pvd[80] | (pvd[81] << 8) |
                       (pvd[82] << 16) | (pvd[83] << 24));
    info.volumeId.assign(reinterpret_cast<char*>(pvd + 40), 32);
    while (!info.volumeId.empty() && info.volumeId.back() == ' ')
        info.volumeId.pop_back();

    // ---- Root directory record (PVD offset 156) -------------------------
    const IsoDirEntry* root =
        reinterpret_cast<const IsoDirEntry*>(pvd + 156);
    const uint32_t rootLba = root->lbaLE;

    // ---- Walk root directory records looking for SYSTEM.CNF -------------
    // The original located it via the ISO directory (LBA*2048, size<=255)
    // then read the file — see ParseSystemCnf @0x3A0F10.
    uint8_t dir[kSectorSize * 4];
    uint32_t dirLen = root->sizeLE;
    if (dirLen > sizeof(dir)) dirLen = sizeof(dir);
    if (!readAt(fh.f, static_cast<uint64_t>(rootLba) * kSectorSize, dir, dirLen))
        return info;

    uint32_t sysLba = 0;
    uint32_t sysSize = 0;
    for (uint32_t off = 0; off + 1 < dirLen;) {
        const uint8_t recLen = dir[off];
        if (recLen == 0) {                          // pad to sector boundary
            off = (off / kSectorSize + 1) * kSectorSize;
            if (off >= dirLen) break;
            continue;
        }
        const IsoDirEntry* e = reinterpret_cast<const IsoDirEntry*>(dir + off);
        // Directory names carry the ISO9660 version suffix ("SYSTEM.CNF;1");
        // strip it (and the trailing pad byte) before matching.
        std::string name(reinterpret_cast<const char*>(dir + off + 33), e->nameLen);
        if (name.size() > 0 && name.back() == '\0') name.pop_back();   // pad byte
        const size_t semi = name.find(';');
        if (semi != std::string::npos) name = name.substr(0, semi);
        // Compare case-insensitively ("system.cnf" ref @0xA5430)
        std::string lower;
        for (char c : name) lower += static_cast<char>(
            (c >= 'A' && c <= 'Z') ? c + 32 : c);
        if (lower == "system.cnf") {
            sysLba = e->lbaLE;
            sysSize = e->sizeLE;
            break;
        }
        off += recLen;
    }

    if (sysLba == 0) return info;                   // "not a PS2 disc"

    // ---- Read SYSTEM.CNF (capped at 255 like the original) --------------
    if (sysSize > 255) sysSize = 255;
    std::string cnf(sysSize, '\0');
    if (!readAt(fh.f, static_cast<uint64_t>(sysLba) * kSectorSize, &cnf[0], sysSize))
        return info;
    info.systemCnf = cnf;

    // ---- BOOT2 line ------------------------------------------------------
    // Typical: BOOT2 = cdrom0:\SLUS_202.65;1
    std::string elf;
    const size_t b = cnf.find("BOOT2");
    if (b != std::string::npos) {
        size_t eq = cnf.find('=', b);
        if (eq != std::string::npos) {
            size_t start = cnf.find('\\', eq);       // path separator scan
            if (start == std::string::npos) start = eq + 1;
            size_t end = start + 1;
            while (end < cnf.size() && cnf[end] != '\r' && cnf[end] != '\n'
                   && cnf[end] != ' ' && cnf[end] != ';')
                ++end;
            elf = cnf.substr(start + 1 < cnf.size() ? start + 1 : start,
                             end - (start + 1 < cnf.size() ? start + 1 : start));
        }
    }
    info.bootElf = elf;
    info.serial = normalizeSerial(elf);
    info.valid = !info.serial.empty();
    return info;
}

} // namespace usbutils
