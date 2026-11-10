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
    /// Chunk files stay open for the object's lifetime: every read/write
    /// used to fopen/fseek/fclose per call, which cost an open+close pair
    /// on each 2 KiB directory read during ISO walks and defeated the
    /// kernel's sequential read-ahead on the underlying device.
    ~GameStream();
    GameStream() = default;
    GameStream(const GameStream&) = delete;
    GameStream& operator=(const GameStream&) = delete;

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

    /// Number of chunk files the first `bytes` of the stream live in,
    /// derived from the ACTUAL on-disk chunk sizes (1 for a size inside
    /// the first chunk). Never exceeds parts().
    uint32_t chunkCountFor(uint64_t bytes) const;

    /// Shrink the stream to `newSize` bytes, deleting whole surplus chunk
    /// files when the new size no longer needs them (unlike truncate(),
    /// which refuses to cut past the final chunk). `removed` (optional)
    /// receives how many chunk FILES were deleted. The ul.cfg record is
    /// NOT touched — callers must keep its parts field in sync.
    /// Equivalent to shrinkTrim() followed by deleteSurplus(); prefer the
    /// two separate calls when something must be made durable in between
    /// (the ul.cfg parts field — see isoshrink.cpp).
    bool shrinkTo(uint64_t newSize, uint32_t* removed = nullptr);

    /// Cut the stream to `newSize` bytes WITHOUT deleting anything: the
    /// last kept chunk file is trimmed and the stream's accounting
    /// switches to the kept chunks, but the surplus chunk files stay on
    /// disk. A caller that fails between this and deleteSurplus() leaves
    /// every chunk file in place — the loader then opens parts it never
    /// reads, which is safe, instead of missing parts it needs.
    bool shrinkTrim(uint64_t newSize);

    /// Delete the surplus chunk files `keep .. hadParts-1` after the
    /// ul.cfg record was updated. A file that cannot be removed (in use
    /// on Windows, foreign permissions) stays behind as an obsolete
    /// "ul.*" file the drive-space cleanup can delete later. Returns how
    /// many files were actually removed.
    uint32_t deleteSurplus(uint32_t keep, uint32_t hadParts);

    /// Total size of all chunk files of a game (sum of file sizes).
    static uint64_t totalSize(const std::string& dir, const std::string& idHex,
                              const std::string& serial, uint32_t parts);

private:
    std::string chunkPath_(uint32_t index) const;
    /// Resolve an absolute offset to (chunk index, in-chunk offset,
    /// bytes available in that chunk). Returns false past the end.
    bool locate_(uint64_t offset, uint32_t& chunk, uint64_t& inChunk,
                 uint64_t& avail) const;
    /// Cached handle for `chunk`; opens lazily. Read requests accept a
    /// read-capable handle ("rb"/"r+b"), write requests a write-capable
    /// one ("r+b"/"wb"); anything else is reopened in the needed mode.
    /// Returns null when the chunk cannot be opened.
    std::FILE* handle_(uint32_t chunk, bool forWrite);
    /// Drop the cached handle of `chunk` (after an I/O error, or before
    /// external truncation).
    void drop_(uint32_t chunk);
    void closeAll_();

    enum HandleMode : uint8_t { kClosed = 0, kRead, kReadWrite, kWriteNew };

    std::string dir_;
    std::string idHex_;
    std::string serial_;
    uint32_t    parts_ = 0;
    std::vector<uint64_t> starts_;   ///< cumulative start offset of each chunk
    uint64_t    total_ = 0;
    std::vector<std::FILE*> files_;  ///< lazily opened chunk handles
    std::vector<uint8_t>    mode_;   ///< HandleMode of each files_[] entry
};

} // namespace usbutils
