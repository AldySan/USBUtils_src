// SPDX-License-Identifier: MIT
#include "lzpack.h"

#include <cstring>

namespace usbutils {

namespace {

inline uint32_t hash4(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return (v * 2654435761u) >> 16;
}

constexpr uint32_t kHashBits  = 16;
constexpr uint32_t kHashSize  = 1u << kHashBits;
constexpr uint32_t kNoPos     = 0xFFFFFFFFu;
constexpr int      kMaxChain  = 8;

void put16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
}

} // namespace

bool lzEncodeBlock(const uint8_t* src, size_t rawLen,
                   std::vector<uint8_t>& out) {
    if (rawLen == 0 || rawLen > kLzBlockSize) return false;

    std::vector<uint8_t> buf;
    buf.reserve(rawLen + rawLen / 8 + 16);

    std::vector<uint32_t> head(kHashSize, kNoPos);
    std::vector<uint32_t> prev(rawLen, kNoPos);

    size_t i = 0;
    while (i < rawLen) {
        const size_t ctrlPos = buf.size();
        buf.push_back(0);
        uint8_t ctrl = 0;

        for (int unit = 0; unit < 8 && i < rawLen; ++unit) {
            size_t bestLen = 0, bestOff = 0;
            if (i + kLzMinMatch <= rawLen) {
                const uint32_t h = hash4(src + i);
                const size_t maxLen = rawLen - i < kLzMaxMatch
                    ? rawLen - i : kLzMaxMatch;
                uint32_t j = head[h];
                int chain = kMaxChain;
                while (j != kNoPos && chain-- > 0) {
                    const size_t s = j;
                    const size_t off = i - s;
                    if (off == 0 || off > kLzMaxOffset) break;
                    size_t l = 0;
                    while (l < maxLen && src[s + l] == src[i + l]) ++l;
                    if (l > bestLen) { bestLen = l; bestOff = off; }
                    if (bestLen >= maxLen) break;
                    j = prev[s];
                }
                prev[i] = head[h];
                head[h] = static_cast<uint32_t>(i);
            }

            if (bestLen >= kLzMinMatch) {
                put16(buf, static_cast<uint16_t>(bestOff));
                buf.push_back(static_cast<uint8_t>(bestLen - kLzMinMatch));
                i += bestLen;                       // control bit stays 0
            } else {
                ctrl |= static_cast<uint8_t>(1u << unit);
                buf.push_back(src[i]);
                ++i;
            }
        }
        buf[ctrlPos] = ctrl;
    }

    if (buf.size() >= rawLen) return false;
    out.insert(out.end(), buf.begin(), buf.end());
    return true;
}

bool lzDecodeBlock(const uint8_t* src, size_t srcLen,
                   uint8_t* dst, size_t rawLen) {
    if (rawLen == 0) return false;

    size_t ip = 0, op = 0;
    while (op < rawLen) {
        if (ip >= srcLen) return false;
        const uint8_t ctrl = src[ip++];
        for (int unit = 0; unit < 8 && op < rawLen; ++unit) {
            if (ctrl & (1u << unit)) {
                if (ip >= srcLen) return false;
                dst[op++] = src[ip++];
            } else {
                if (ip + 3 > srcLen) return false;
                const uint16_t off = static_cast<uint16_t>(
                    src[ip] | (src[ip + 1] << 8));
                const size_t len = src[ip + 2] + kLzMinMatch;
                ip += 3;
                if (off == 0 || off > op || op + len > rawLen) return false;
                const size_t s = op - off;
                for (size_t k = 0; k < len; ++k)       // overlap-safe copy
                    dst[op + k] = dst[s + k];
                op += len;
            }
        }
    }
    return true;
}

} // namespace usbutils
