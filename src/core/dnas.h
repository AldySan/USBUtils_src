// SPDX-License-Identifier: MIT
/*
 * DNAS / IOPRP package handling.
 * Formats documented in docs/formats/dnas_package.md:
 *   usbutil.dni : text index  "DNAS<ver>[<size>];"
 *   usbutil.dna : concatenated IOPRP images in index order
 * Patching replaces the IOPRP image embedded in a game's chunk files.
 */
#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace usbutils {

struct DnasEntry {
    std::string version;   ///< e.g. "16", "165", "300"
    uint32_t    size = 0;  ///< bytes in the dna blob
    uint64_t    offset = 0;///< running offset inside usbutil.dna
};

class DnasPackage {
public:
    /// Load index (dniPath) — offsets computed from cumulative sizes.
    bool loadIndex(const std::string& dniPath);

    /// Set the blob path (dna). The blob is read lazily.
    void setDataPath(const std::string& dnaPath);

    const std::vector<DnasEntry>& entries() const { return entries_; }
    std::string bestFor(const std::string& consoleVer) const;

    /// Extract one IOPRP image to `outPath` (first 8 bytes are "RESET").
    bool extract(const std::string& version, const std::string& outPath) const;

    /// Patch a game: replace the embedded IOPRP image inside chunk files.
    /// `idHex`/`serial`/`parts` identify the game; `version` the replacement.
    /// Returns number of chunks modified, or -1 on error.
    int patchGame(const std::string& dir, const std::string& idHex,
                  const std::string& serial, uint32_t parts,
                  const std::string& version) const;

private:
    std::vector<DnasEntry> entries_;
    std::string dnaPath_;
    uint64_t blobSize_ = 0;

    bool findImage(const std::string& version,
                   uint64_t& off, uint32_t& size) const;
};

/// Locate the embedded IOPRP image inside a game's chunk stream.
/// Writes chunk index, in-chunk offset and length. Returns false if absent.
bool locateEmbeddedIoprp(const std::string& dir, const std::string& idHex,
                         const std::string& serial, uint32_t parts,
                         uint32_t& chunkIdx, uint64_t& offset, uint32_t& length);

} // namespace usbutils
