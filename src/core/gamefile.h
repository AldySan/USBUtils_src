// SPDX-License-Identifier: MIT
/*
 * Chunk file naming / scanning / size accounting.
 * Derived from: SplitISO @0x3A112F, chunk name concat @0x3A1308,
 * ScanULPattern @0x3A2B77 ("ul.*.00"), WriteChunkFile @0x39CF44.
 */
#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <functional>

namespace usbutils {

/// Maximum bytes in one chunk file (0x40000000, global @0x3BFE7C).
constexpr uint64_t kChunkSize = 0x40000000ull;
/// Images <= 700 MB are treated as CD media (global @0x3BFE84).
constexpr uint64_t kCdThreshold = 734003200ull;
/// CD / DVD media codes written into ul.cfg byte 48.
enum Media : uint8_t { kMediaCd = 0x12, kMediaDvd = 0x14 };

/// Join a directory and a file name ('/' separator; tolerant of a
/// trailing '/' or '\\'). Shared by all chunk-file accessors.
std::string joinPath(const std::string& dir, const std::string& file);

/// Number of chunks for a given total size (ceil division, 0x3A11C2).
uint32_t chunkCount(uint64_t totalSize);

/// Media code by size rule (GetMediaType @0x39D980 step 3).
uint8_t mediaForSize(uint64_t totalSize);

/// "ul.<ID>.<SERIAL>.NN" — NN is two digits (original: ".0"+Chr(i)).
std::string chunkFileName(const std::string& idHex, const std::string& serial,
                          uint32_t index);

/// Scan a directory for first-chunk files (FindFirst "ul.*.00" pattern).
/// Returns every file matching "ul.*.00"; fill idHex/serial out-params.
struct FoundChunk {
    std::string fileName;     ///< full path
    std::string idHex;        ///< 8 hex digits (between first two dots)
    std::string serial;       ///< remainder before ".00"
};
std::vector<FoundChunk> scanFirstChunks(const std::string& dir);

/// Total size of all chunks of a game (sum of file sizes).
uint64_t gameTotalSize(const std::string& dir, const std::string& idHex,
                       const std::string& serial, uint32_t parts);

/// Delete all chunk files of a game; returns count removed.
int deleteChunks(const std::string& dir, const std::string& idHex,
                 const std::string& serial, uint32_t parts);

/// Rename chunks after a game rename (oldId -> newId).
bool renameChunks(const std::string& dir, const std::string& oldIdHex,
                  const std::string& newIdHex, const std::string& serial,
                  uint32_t parts);

/// Progress callback: (bytes done, bytes total). Shared by every
/// long-running engine operation.
using ProgressFn = std::function<void(uint64_t done, uint64_t total)>;

/// Structure check: verify all parts exist and sum sizes.
struct StructureReport {
    uint32_t partsPresent = 0;
    uint64_t totalBytes = 0;
    std::vector<uint32_t> missing;   ///< missing part indexes
    bool ok() const { return missing.empty(); }
};
StructureReport checkStructure(const std::string& dir, const std::string& idHex,
                               const std::string& serial, uint32_t parts);

} // namespace usbutils
