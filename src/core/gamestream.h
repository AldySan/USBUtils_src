// SPDX-License-Identifier: MIT
/*
 * GameStream — random-access read/write over a game's chunk files.
 *
 * The USBExtreme format stores a game as consecutive 1 GiB chunk files
 * ("ul.<ID>.<SERIAL>.NN", written by SplitISO @0x3A112F). Logically they
 * form one contiguous byte stream; this class presents it as such so the
 * ISO walker, the video ripper and the DNAS patcher can treat an
 * installed game exactly like a plain ISO file.
 *
 * Chunk boundaries are derived from the ACTUAL on-disk file sizes (not
 * assumed to be 1 GiB), so streams produced by other tools or recovered
 * games with unusual splits still resolve correctly.
 */
#pragma once

#include <string>
#include <cstdint>
#include <vector>

namespace usbutils {

class GameStream {
public:
    /// Open the chunk sequence `ul.<idHex>.<serial>.00..NN-1` in `dir`.
    /// Missing chunks are tolerated as zero-length (size() reflects only
    /// the bytes actually present).
    bool open(const std::string& dir, const std::string& idHex,
              const std::string& serial, uint32_t parts);

    uint64_t size()  const { return total_; }
    uint32_t parts() const { return parts_; }
    bool     valid() const { return parts_ > 0; }

    /// Read up to `n` bytes at absolute stream offset (may span chunks).
    /// Returns the number of bytes actually read.
    size_t read(uint64_t offset, void* dst, size_t n);

    /// Write `n` bytes at absolute stream offset (may span chunks).
    /// Returns false on any write error.
    bool write(uint64_t offset, const void* src, size_t n);

    /// Append `n` bytes at the current end of the stream, growing the
    /// LAST chunk file (never creates a new part — ul.cfg stays valid).
    /// `limit` bounds how far the stream may grow: appends that would
    /// push the last chunk past `parts * 1 GiB` are refused, because
    /// loaders address data beyond that point through a chunk file the
    /// game does not have. Returns the absolute offset the bytes were
    /// written at, or UINT64_MAX on error.
    uint64_t append(const void* src, size_t n, uint64_t limit);

    /// Shrink the stream to `newSize` bytes by trimming the last chunk
    /// file. Only cuts into the final chunk (real games never shrink
    /// across a chunk boundary). Returns false on error.
    bool truncate(uint64_t newSize);

    /// Total size of all chunk files of a game (sum of file sizes).
    static uint64_t totalSize(const std::string& dir, const std::string& idHex,
                              const std::string& serial, uint32_t parts);

private:
    std::string chunkPath_(uint32_t index) const;
    /// Resolve an absolute offset to (chunk index, in-chunk offset,
    /// bytes available in that chunk). Returns false past the end.
    bool locate_(uint64_t offset, uint32_t& chunk, uint64_t& inChunk,
                 uint64_t& avail) const;

    std::string dir_;
    std::string idHex_;
    std::string serial_;
    uint32_t    parts_ = 0;
    std::vector<uint64_t> starts_;   ///< cumulative start offset of each chunk
    uint64_t    total_ = 0;
};

} // namespace usbutils
