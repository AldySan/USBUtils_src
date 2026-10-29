// SPDX-License-Identifier: MIT
/*
 * USBUtils — open reimplementation of USBUtil v2.2 (PS2 USBExtreme manager)
 * Copyright (c) 2025 AldySan
 * Based on the reverse-engineering of "USBUtil v2.2 rev1.0" by ISEKO (2009).
 *
 * update — "Check for update" support for the CLI and the GUI.
 *
 * The latest published release is queried from the GitHub REST API
 * (https://api.github.com/repos/AldySan/USBUtils_src/releases/latest) with
 * ZERO extra dependencies:
 *   - Windows: WinHTTP (ships with every Windows since XP)
 *   - Linux/macOS: the `curl` (or `wget`) binary present on virtually
 *     every desktop system
 * Failure is always silent-and-degradable: when offline, the caller simply
 * reports "could not check".
 */
#pragma once

#include <string>
#include <cstdint>

namespace usbutils {

/// The project's GitHub releases page.
extern const char* kProjectReleasesUrl;

struct ReleaseInfo {
    bool        ok = false;      ///< release info could be fetched
    std::string tag;             ///< e.g. "v0.0.3" ("" on failure)
    std::string url;             ///< release page URL
    std::string error;           ///< human-readable failure reason
};

/// Fetch the latest release from GitHub (timeout ~10 s).
ReleaseInfo fetchLatestRelease();

/// True when `tag` (e.g. "v0.0.3") is a newer version than `current`
/// (e.g. "0.0.2"). Leading 'v'/'V' is ignored; components are compared
/// numerically ("0.0.10" > "0.0.9").
bool isVersionNewer(const std::string& current, const std::string& tag);

} // namespace usbutils
