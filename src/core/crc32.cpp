// SPDX-License-Identifier: MIT
#include "crc32.h"

namespace usbutils {
namespace {

constexpr uint32_t kPoly = 0xEDB88320u;   // reflected ISO-HDLC polynomial

struct Table {
    uint32_t v[256];
};

constexpr Table makeTable() {
    Table t{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1u) ? (kPoly ^ (c >> 1)) : (c >> 1);
        t.v[i] = c;
    }
    return t;
}

constexpr Table kTable = makeTable();

} // namespace

uint32_t crc32(uint32_t crc, const void* data, size_t len) {
    crc ^= 0xFFFFFFFFu;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i)
        crc = kTable.v[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

} // namespace usbutils
