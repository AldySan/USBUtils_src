// SPDX-License-Identifier: MIT
#pragma once
#include <cstdio>
#include <cstdint>

namespace usbutils {

/// Portable 64-bit file-position query (Windows _ftelli64 / POSIX ftello).
inline int64_t tell64(std::FILE* f) {
#if defined(_WIN32)
    // MSVC, clang-cl and MinGW-w64 all declare _ftelli64; plain ftell()
    // takes a 32-bit `long` (LLP64) and fails for anything over 2 GiB.
    return _ftelli64(f);
#elif defined(__unix__) || defined(__APPLE__)
    return static_cast<int64_t>(ftello(f));
#else
    return static_cast<int64_t>(std::ftell(f));
#endif
}

/// Portable 64-bit seek. Plain fseek() takes a 32-bit `long` on Windows
/// (LLP64), which silently breaks on DVD-sized images (> 2 GiB) — every
/// large-offset seek in the codebase must go through this helper.
inline bool seek64(std::FILE* f, int64_t off, int whence = SEEK_SET) {
#if defined(_WIN32)
    // _WIN32 (not _MSC_VER): our shipped Windows binaries are built with
    // MinGW-w64, which defines neither _MSC_VER nor __unix__ — the generic
    // std::fseek() branch below truncates offsets to 32 bits there, which
    // broke installing / exploring / patching every DVD-size ISO on
    // Windows. _fseeki64 is available in all three toolchains.
    return _fseeki64(f, off, whence) == 0;
#elif defined(__unix__) || defined(__APPLE__)
    return fseeko(f, static_cast<off_t>(off), whence) == 0;
#else
    return std::fseek(f, static_cast<long>(off), whence) == 0;
#endif
}

} // namespace usbutils
