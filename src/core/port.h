// SPDX-License-Identifier: MIT
#pragma once
#include <cstdio>
#include <cstdint>

namespace usbutils {

/// Portable 64-bit file-position query (MSVC _ftelli64 / glibc ftello64).
inline int64_t tell64(std::FILE* f) {
#if defined(_MSC_VER)
    return _ftelli64(f);
#elif defined(__unix__) || defined(__APPLE__)
    return static_cast<int64_t>(ftello(f));
#else
    return static_cast<int64_t>(std::ftell(f));
#endif
}

} // namespace usbutils
