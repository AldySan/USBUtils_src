// SPDX-License-Identifier: MIT
/*
 * Support-file registry — powers the GUI's "Useful acquired" panel and the
 * CLI's `usbutils support` command.
 *
 * The original USBUtil distribution ships these files next to the exe
 * (verified against docs/binary_analysis.md, SHA-256):
 *
 *   PS2LOAD.ELF            PS2Load inject payload
 *   usbutil.aso            tool config (INI-like, rewritten by the tool)
 *   usbutil.dna            DNAS package blob (concatenated IOPRP images)
 *   usbutil.dni            DNAS package index
 *   usbutil.ljs            tested-game compatibility notes
 *   usbutil.rcn            recovery-game list with annotations
 *   SCEE_DD/IOPRP16.IMG    individual IOPRP images (dna sources)
 *   SCEE_DD/IOPRP165.IMG
 *   SCEE_DD/IOPRP20.IMG
 *   SCEE_DD/IOPRP202.IMG
 *
 * We ship the same set in data/ and look it up under several roots so the
 * layout works both from a repo checkout, a release tarball/zip and a
 * plain `bin/ + data/` install.
 */
#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace usbutils {

enum class SupportRole {
    Ps2Load,     ///< PS2LOAD.ELF          — "PS2Load inject"
    DnasIndex,   ///< usbutil.dni          — DNAS patch version list
    DnasData,    ///< usbutil.dna          — DNAS patch IOPRP blob
    GameNotes,   ///< usbutil.ljs          — game compatibility notes
    ToolConfig,  ///< usbutil.aso          — tool config (optional, auto-created)
    Recovery,    ///< usbutil.rcn          — recovery-game notes (optional)
    IoprpImage,  ///< SCEE_DD/IOPRP*.IMG   — standalone IOPRP images (optional)
};

struct SupportFile {
    SupportRole    role;
    const char*    fileName;    ///< relative to a support root, '/'-separated
    bool           required;    ///< a shipped feature breaks without it
    const char*    feature;     ///< human: which feature needs it
    uint64_t       expectedSize;
    const char*    sha256;      ///< of the original file, "" when N/A
};

struct SupportStatus {
    SupportFile    file;
    std::string    foundPath;   ///< first existing candidate (absolute)
    uint64_t       size = 0;
    bool           found = false;
    bool           verified = false;   ///< size+sha256 match the original
    bool           extra = false;      ///< true for IOPRP images beyond the
                                       ///< first one when listing (all map to
                                       ///< one role)
};

/// Roots that are probed, in order: explicit extra roots first, then
///   <exe dir>, <exe dir>/data, <exe dir>/.., <exe dir>/../data,
///   cwd, cwd/data
std::vector<std::string> supportSearchPaths(const std::vector<std::string>& extraRoots = {});

/// The full registry (one entry per shipped file; IOPRP images get 4
/// entries with role IoprpImage).
const std::vector<SupportFile>& supportFileRegistry();

/// Check every registry entry. IOPRP entries carry found/verified
/// individually but `feature` is shared.
std::vector<SupportStatus> checkSupportFiles(const std::vector<std::string>& extraRoots = {});

/// Locate one specific support file (empty string when missing).
std::string findSupportFile(const char* fileName,
                            const std::vector<std::string>& extraRoots = {});

/// Directory of the running executable (slash-separated, no trailing sep).
std::string executableDir();

} // namespace usbutils
