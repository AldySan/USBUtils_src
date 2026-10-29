// SPDX-License-Identifier: MIT
#include "usbdevice.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

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

    // Filesystem type: statvfs f_basetype is not portable; use a heuristic
    // plus presence of FAT markers (any file > 4 GiB ⇒ not FAT32).
    d.fat32 = d.totalBytes > 0;  // refined below
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

} // namespace usbutils
