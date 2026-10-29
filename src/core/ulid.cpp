// SPDX-License-Identifier: MIT
#include "ulid.h"

#include <cstring>
#include <cstdio>
#include <cctype>

namespace usbutils {

// Function-local static: initialised exactly once, thread-safely (C++11
// "magic statics") — the GUI computes ids on worker threads.
static const uint32_t* crcTable() {
    static const struct Table {
        uint32_t t[256];
        Table() {
            // Reconstructed from InitCRCTable @0x398738:
            //   for i in 0..255: c = i << 24;
            //   8x { c = (c<<1) ^ (0x04C11DB7 if c&0x80000000) }
            for (uint32_t i = 0; i < 256; ++i) {
                uint32_t c = i << 24;
                for (int k = 0; k < 8; ++k) {
                    if (c & 0x80000000u) c = (c << 1) ^ 0x04C11DB7u;
                    else                 c = (c << 1);
                }
                t[i] = c;
            }
        }
    } table;
    return table.t;
}

uint32_t ulCrc32(const void* data, size_t len) {
    const uint32_t* table = crcTable();
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t crc = 0;                                  // xor ebx,ebx
    for (size_t i = 0; i < len; ++i) {
        const uint32_t idx = (p[i] ^ (crc >> 24)) & 0xFF;  // al ^ (ebx>>24)
        crc = (crc << 8) & 0xFFFFFFFFu;                    // shl ebx,8
        crc ^= table[idx];                                 // xor table
    }
    return crc;
}

static std::string trim(const std::string& s) {
    // The original 0x394E70 trims leading/trailing spaces only.
    size_t b = 0, e = s.size();
    while (b < e && s[b] == ' ') ++b;
    while (e > b && s[e - 1] == ' ') --e;
    return s.substr(b, e - b);
}

std::string gameIdHex(const std::string& gameName) {
    // Input: Trim(name) + #0   (1-char string constant #0 appended @0x3987AD)
    const std::string s = trim(gameName) + std::string(1, '\0');
    const uint32_t crc = ulCrc32(s.data(), s.size());

    char out[9];
    std::snprintf(out, sizeof(out), "%08X", crc);     // IntToHex(crc, 8)
    return std::string(out);
}

} // namespace usbutils
