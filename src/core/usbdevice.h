// SPDX-License-Identifier: MIT
/*
 * USB device discovery — cross-platform replacement for the original's
 * DiskNotifier / DeviceNotifier / Devices units (Windows WM_DEVICECHANGE).
 */
#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace usbutils {

struct UsbDrive {
    std::string root;         ///< mount point ("/media/usb0", "E:\\", ...)
    std::string label;        ///< volume label when available
    uint64_t    totalBytes = 0;
    uint64_t    freeBytes = 0;
    bool        fat32 = false;///< detected filesystem (FAT32 preferred)
    bool        hasUlCfg = false;
};

/// Enumerate removable drives. On Windows this uses GetLogicalDrives +
/// GetDriveTypeA; on Linux/macOS it scans mount points under /media,
/// /run/media/$USER and /Volumes.
std::vector<UsbDrive> listDrives();

/// An optical (CD/DVD) drive with a data disc inserted — the source for
/// "Create GAME from CD/DVD".
struct OpticalDrive {
    std::string devicePath;   ///< raw device ("/dev/sr0" or "\\\\.\\D:")
    std::string display;      ///< friendly name ("DVD Drive (D:)", "/dev/sr0")
    std::string volumeId;     ///< ISO9660 volume id (empty until probed)
    uint64_t    dataBytes = 0;///< exact ISO9660 data size (volumeSectors*2048)
    bool        discPresent = false;
    bool        isPs2 = false;     ///< SYSTEM.CNF found (heuristic probe)
    std::string serial;      ///< e.g. "SLUS_202.65" when isPs2
};

/// Enumerate optical drives and probe each for an ISO9660 disc.
/// Probing opens the raw device and reads sector 16 (PVD) — drives with
/// no disc are listed with discPresent = false.
std::vector<OpticalDrive> listOpticalDrives();

/// True when the drive root contains ul.cfg (already a game drive).
bool probeUlCfg(const std::string& root);

/// Free / total bytes of the filesystem containing `path`.
/// Returns false when the path is not reachable.
bool fsSpace(const std::string& path, uint64_t& freeBytes,
             uint64_t& totalBytes);

} // namespace usbutils
