// SPDX-License-Identifier: MIT
#include "usbdevice.h"
#include "iso9660.h"
#include "port.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <sys/statvfs.h>
  #include <dirent.h>
  #include <pwd.h>
#endif

namespace usbutils {

static bool fileExists(const std::string& p) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

bool probeUlCfg(const std::string& root) {
    std::string p = root;
    if (!p.empty() && p.back() != '/' && p.back() != '\\') p += "/";
    return fileExists(p + "ul.cfg");
}

bool fsSpace(const std::string& path, uint64_t& freeBytes,
             uint64_t& totalBytes) {
    freeBytes = totalBytes = 0;
    if (path.empty()) return false;
#if defined(_WIN32)
    ULARGE_INTEGER freeB{}, totalB{};
    std::string root = path;
    if (root.size() > 1 && root[1] == ':') root.resize(3);   // "X:\\"
    if (!GetDiskFreeSpaceExA(root.c_str(), &freeB, &totalB, nullptr))
        return false;
    freeBytes  = freeB.QuadPart;
    totalBytes = totalB.QuadPart;
    return true;
#else
    struct statvfs st{};
    if (statvfs(path.c_str(), &st) != 0) return false;
    totalBytes = static_cast<uint64_t>(st.f_blocks) * st.f_frsize;
    freeBytes  = static_cast<uint64_t>(st.f_bavail) * st.f_frsize;
    return true;
#endif
}

#if defined(_WIN32)

std::vector<UsbDrive> listDrives() {
    std::vector<UsbDrive> out;
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        std::string root;
        root += static_cast<char>('A' + i);
        root += ":\\";
        if (GetDriveTypeA(root.c_str()) != DRIVE_REMOVABLE &&
            GetDriveTypeA(root.c_str()) != DRIVE_FIXED)
            continue;
        ULARGE_INTEGER freeBytes{}, totalBytes{};
        char vol[64] = {};
        char fs[32] = {};
        GetDiskFreeSpaceExA(root.c_str(), &freeBytes, &totalBytes, nullptr);
        GetVolumeInformationA(root.c_str(), vol, sizeof(vol),
                               nullptr, nullptr, nullptr, fs, sizeof(fs));
        UsbDrive d;
        d.root = root;
        d.label = vol;
        d.totalBytes = totalBytes.QuadPart;
        d.freeBytes = freeBytes.QuadPart;
        d.fat32 = _stricmp(fs, "FAT32") == 0;
        d.hasUlCfg = probeUlCfg(root);
        out.push_back(std::move(d));
    }
    return out;
}

#else // POSIX

#if defined(__linux__)
/// Real filesystem type of a mount point, looked up in /proc/self/mounts.
/// Mount-point fields escape space/tab/newline/backslash as \040/\011/
/// \012/\134 — escape our path the same way so labels with spaces still
/// match. Returns "" when the mount is not listed.
static std::string mountsFsType(const std::string& root) {
    std::string trimmed = root;
    if (!trimmed.empty() && trimmed.back() == '/')
        trimmed.pop_back();                        // mounts entries have no trailing '/'
    std::string escaped;
    for (char c : trimmed) {
        if (c == ' ')       escaped += "\\040";
        else if (c == '\t') escaped += "\\011";
        else if (c == '\n') escaped += "\\012";
        else if (c == '\\') escaped += "\\134";
        else                escaped += c;
    }

    FILE* f = std::fopen("/proc/self/mounts", "r");
    if (!f) return {};
    std::string found;
    char line[4096];
    while (std::fgets(line, sizeof(line), f)) {
        if (!std::strchr(line, '\n')) {            // very long line: drain it
            int c2;
            while ((c2 = std::fgetc(f)) != EOF && c2 != '\n') {}
            continue;
        }
        // "<device> <mountpoint> <fstype> <options> ..."
        char dev[1024] = {}, mnt[2048] = {}, fst[64] = {};
        if (std::sscanf(line, "%1023s %2047s %63s", dev, mnt, fst) == 3) {
            if (mnt == escaped || mnt == trimmed) {
                found = fst;
                break;
            }
        }
    }
    std::fclose(f);
    return found;
}
#endif // __linux__

static void addMount(std::vector<UsbDrive>& out, const std::string& mp) {
    if (mp.empty()) return;
    std::string root = mp;
    if (root.back() != '/') root += '/';

    struct statvfs st{};
    if (statvfs(root.c_str(), &st) != 0) return;
    if (st.f_blocks == 0) return;

    UsbDrive d;
    d.root = root;
    d.totalBytes = static_cast<uint64_t>(st.f_blocks) * st.f_frsize;
    d.freeBytes = static_cast<uint64_t>(st.f_bavail) * st.f_frsize;

    // Filesystem type. statvfs has no portable fstype field, so probe
    // /proc/self/mounts on Linux; elsewhere report "unknown" rather than
    // wrongly claiming FAT32 for every drive.
#if defined(__linux__)
    const std::string fst = mountsFsType(root);
    d.fat32 = fst == "vfat" || fst == "msdos" || fst == "umsdos";
#else
    d.fat32 = false;
#endif
    d.hasUlCfg = probeUlCfg(root);

    // Volume label: Linux exposes it as /media/<label> basename.
    const size_t slash = root.substr(0, root.size() - 1).rfind('/');
    if (slash != std::string::npos)
        d.label = root.substr(slash + 1, root.size() - slash - 2);

    out.push_back(std::move(d));
}

std::vector<UsbDrive> listDrives() {
    std::vector<UsbDrive> out;
    const char* candidates[] = {
        "/media", "/run/media", "/Volumes",
    };
    for (const char* base : candidates) {
        DIR* d = opendir(base);
        if (!d) continue;
        while (struct dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            const std::string mp1 = std::string(base) + "/" + e->d_name;
            // /run/media/<user>/<label>
            DIR* sub = opendir(mp1.c_str());
            if (sub) {
                while (struct dirent* e2 = readdir(sub)) {
                    if (e2->d_name[0] == '.') continue;
                    addMount(out, mp1 + "/" + e2->d_name);
                }
                closedir(sub);
            } else {
                addMount(out, mp1);
            }
        }
        closedir(d);
    }
    return out;
}

#endif

// ---------------------------------------------------------------------------
// listOpticalDrives — enumerate CD/DVD drives and probe for ISO9660 discs
// ---------------------------------------------------------------------------

namespace {

// (helper shared by listOpticalDrives and the public probe below)

} // namespace

/// Probe one raw device: open, read sector 16 (PVD), and walk the root
/// directory for SYSTEM.CNF. All reads are 2048-aligned so raw Windows
/// device handles work.
bool probeOpticalDrive(const std::string& devicePath, OpticalDrive* out) {
    FILE* f = std::fopen(devicePath.c_str(), "rb");
    if (!f) return false;

    const IsoReadFn read = [f](uint64_t off, void* dst, size_t n) {
        return usbutils::seek64(f, static_cast<int64_t>(off))
            && std::fread(dst, 1, n, f) == n;
    };

    std::string volId;
    uint32_t volSectors = 0;
    if (!usbutils::readPvd(read, &volId, &volSectors)) {
        std::fclose(f);
        out->discPresent = true;      // there IS a disc, just not ISO9660
        return false;
    }

    out->volumeId = volId;
    out->dataBytes = uint64_t(volSectors) * 2048ull;
    out->discPresent = true;

    // SYSTEM.CNF probe (serial). All walk reads stay 2048-multiples because
    // directory extents are sector-padded on real discs.
    walkIso(read, [&](const IsoEntry& e) {
        if (!e.isDir) {
            std::string low;
            for (char c : e.name) low += char((c >= 'A' && c <= 'Z') ? c + 32 : c);
            if (low == "system.cnf") {
                // Read the WHOLE sector: raw Windows device handles require
                // sector-multiple read lengths, and the CNF text is just a
                // prefix of it (the code's own "2048-aligned reads" rule).
                char sec[2048];
                const size_t n = e.size < 255 ? size_t(e.size) : 255;
                if (read(uint64_t(e.lba) * 2048ull, sec, sizeof(sec))) {
                    out->isPs2 = true;
                    out->serial = usbutils::normalizeSerial(
                        usbutils::bootElfFromCnf(std::string(sec, n)));
                }
                return false;
            }
        }
        return true;
    });

    std::fclose(f);
    return true;
}

std::vector<OpticalDrive> listOpticalDrives() {
    std::vector<OpticalDrive> out;

#if defined(_WIN32)
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        std::string root;
        root += static_cast<char>('A' + i);
        root += ":\\";
        if (GetDriveTypeA(root.c_str()) != DRIVE_CDROM) continue;

        OpticalDrive od;
        od.devicePath = std::string("\\\\.\\") + root.substr(0, 2);
        od.display = "CD/DVD drive " + root.substr(0, 2);
        if (probeOpticalDrive(od.devicePath, &od) && !od.volumeId.empty())
            od.display += " (" + od.volumeId + ")";
        out.push_back(std::move(od));
    }
#else
    // /dev/sr0..sr31 plus the classic symlinks
    std::vector<std::string> paths;
    for (int i = 0; i < 32; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "/dev/sr%d", i);
        paths.push_back(buf);
    }
    for (const char* link : {"/dev/cdrom", "/dev/dvd", "/dev/cdrom0"}) {
        if (std::find(paths.begin(), paths.end(), link) == paths.end())
            paths.push_back(link);
    }
    for (const std::string& p : paths) {
        FILE* f = std::fopen(p.c_str(), "rb");
        if (!f) continue;
        std::fclose(f);                     // exists & openable

        OpticalDrive od;
        od.devicePath = p;
        od.display = p;
        if (probeOpticalDrive(p, &od) && !od.volumeId.empty())
            od.display += " (" + od.volumeId + ")";
        out.push_back(std::move(od));
    }
#endif
    return out;
}

} // namespace usbutils
