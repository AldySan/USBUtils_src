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

/// True when the drive root contains ul.cfg (already a game drive).
bool probeUlCfg(const std::string& root);

/// Free / total bytes of the filesystem containing `path`.
/// Returns false when the path is not reachable.
bool fsSpace(const std::string& path, uint64_t& freeBytes,
             uint64_t& totalBytes);

} // namespace usbutils
