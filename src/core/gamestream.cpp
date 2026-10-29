// SPDX-License-Identifier: MIT
#include "gamestream.h"
#include "gamefile.h"
#include "port.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

#if defined(__unix__) || defined(__APPLE__)
#  include <unistd.h>        // ::truncate
#elif defined(_WIN32)
#  include <io.h>             // _chsize / _chsize_s
#endif

namespace usbutils {

namespace {

uint64_t fileSizeOf(const std::string& p) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return 0;
    const bool ok = usbutils::seek64(f, 0, SEEK_END);
    const int64_t sz = ok ? usbutils::tell64(f) : -1;
    std::fclose(f);
    return sz < 0 ? 0 : static_cast<uint64_t>(sz);
}

} // namespace

bool GameStream::open(const std::string& dir, const std::string& idHex,
                      const std::string& serial, uint32_t parts) {
    dir_ = dir; idHex_ = idHex; serial_ = serial;
    parts_ = parts;
    starts_.clear();
    total_ = 0;

    if (parts == 0) return false;

    for (uint32_t i = 0; i < parts; ++i) {
        starts_.push_back(total_);
        total_ += fileSizeOf(chunkPath_(i));
    }
    return true;
}

std::string GameStream::chunkPath_(uint32_t index) const {
    return joinPath(dir_, chunkFileName(idHex_, serial_, index));
}

uint64_t GameStream::totalSize(const std::string& dir, const std::string& idHex,
                               const std::string& serial, uint32_t parts) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < parts; ++i)
        total += fileSizeOf(joinPath(dir, chunkFileName(idHex, serial, i)));
    return total;
}

bool GameStream::locate_(uint64_t offset, uint32_t& chunk, uint64_t& inChunk,
                         uint64_t& avail) const {
    if (offset >= total_) return false;
    // chunks are visited in order; linear scan is fine (parts <= 255)
    for (uint32_t i = 0; i < parts_; ++i) {
        const uint64_t start = starts_[i];
        const uint64_t end   = (i + 1 < parts_) ? starts_[i + 1] : total_;
        if (offset < end) {
            chunk   = i;
            inChunk = offset - start;
            avail   = end - offset;
            return true;
        }
    }
    return false;
}

size_t GameStream::read(uint64_t offset, void* dst, size_t n) {
    uint8_t* out = static_cast<uint8_t*>(dst);
    size_t got = 0;
    while (got < n) {
        uint32_t chunk = 0;
        uint64_t inChunk = 0, avail = 0;
        if (!locate_(offset + got, chunk, inChunk, avail)) break;
        const size_t want = static_cast<size_t>(
            std::min<uint64_t>(n - got, avail));
        FILE* f = std::fopen(chunkPath_(chunk).c_str(), "rb");
        if (!f) break;
        const bool ok = usbutils::seek64(f, static_cast<int64_t>(inChunk))
                     && std::fread(out + got, 1, want, f) == want;
        std::fclose(f);
        if (!ok) break;
        got += want;
    }
    return got;
}

bool GameStream::write(uint64_t offset, const void* src, size_t n) {
    const uint8_t* in = static_cast<const uint8_t*>(src);
    size_t put = 0;
    while (put < n) {
        uint32_t chunk = 0;
        uint64_t inChunk = 0, avail = 0;
        if (!locate_(offset + put, chunk, inChunk, avail)) return false;
        const size_t want = static_cast<size_t>(
            std::min<uint64_t>(n - put, avail));
        FILE* f = std::fopen(chunkPath_(chunk).c_str(), "r+b");
        if (!f) return false;
        const bool ok = usbutils::seek64(f, static_cast<int64_t>(inChunk))
                     && std::fwrite(in + put, 1, want, f) == want;
        std::fclose(f);
        if (!ok) return false;
        put += want;
    }
    return true;
}

uint64_t GameStream::append(const void* src, size_t n, uint64_t limit) {
    if (n == 0) return total_;
    if (parts_ == 0) return ~0ull;

    // Refuse to grow past the last chunk's 1 GiB addressing window.
    if (limit && total_ + n > limit) return ~0ull;

    // The append always lands in (and grows) the LAST chunk file.
    const uint32_t   last   = parts_ - 1;
    const std::string path  = chunkPath_(last);
    const uint64_t   inChunk = total_ - starts_[last];

    FILE* f = std::fopen(path.c_str(), "r+b");
    if (!f) f = std::fopen(path.c_str(), "wb");   // chunk exists per ul.cfg
    if (!f) return ~0ull;
    const bool ok = usbutils::seek64(f, static_cast<int64_t>(inChunk))
                 && std::fwrite(src, 1, n, f) == n;
    std::fclose(f);
    if (!ok) return ~0ull;

    const uint64_t at = total_;
    total_ += n;
    return at;
}

bool GameStream::truncate(uint64_t newSize) {
    if (parts_ == 0 || newSize > total_) return false;
    if (newSize == total_) return true;

    const uint32_t last = parts_ - 1;
    if (newSize <= starts_[last])
        return false;                    // would span a chunk boundary

    const std::string path = chunkPath_(last);
    const uint64_t   want  = newSize - starts_[last];

    FILE* f = std::fopen(path.c_str(), "r+b");
    if (!f) return false;
    bool ok = usbutils::seek64(f, static_cast<int64_t>(want))
            && std::fflush(f) == 0;
#if defined(_MSC_VER)
    ok = ok && _chsize_s(_fileno(f), static_cast<long>(want)) == 0;
#elif defined(_WIN32)
    ok = ok && _chsize(_fileno(f), static_cast<long>(want)) == 0;
#else
    ok = ok && ::truncate(path.c_str(), static_cast<off_t>(want)) == 0;
#endif
    std::fclose(f);
    if (!ok) return false;
    total_ = newSize;
    return true;
}

} // namespace usbutils
