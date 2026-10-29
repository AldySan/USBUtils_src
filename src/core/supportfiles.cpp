// SPDX-License-Identifier: MIT
#include "supportfiles.h"
#include "port.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#if !defined(_WIN32)
#include <unistd.h>
#else
#include <windows.h>
#endif

// Lightweight SHA-256 (public-domain style, compact) — used only to verify
// the shipped data files against the originals. Kept inline so the core
// library keeps having zero external dependencies.
namespace {

class Sha256 {
public:
    Sha256() { reset(); }
    void reset() {
        h_[0]=0x6a09e667u; h_[1]=0xbb67ae85u; h_[2]=0x3c6ef372u; h_[3]=0xa54ff53au;
        h_[4]=0x510e527fu; h_[5]=0x9b05688cu; h_[6]=0x1f83d9abu; h_[7]=0x5be0cd19u;
        len_ = 0; bufLen_ = 0;
    }
    void update(const void* data, size_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        len_ += uint64_t(n) * 8u;
        while (n) {
            const size_t take = std::min(n, size_t(64) - bufLen_);
            std::memcpy(buf_ + bufLen_, p, take);
            bufLen_ += take; p += take; n -= take;
            if (bufLen_ == 64) { block(buf_); bufLen_ = 0; }
        }
    }
    void final(uint8_t out[32]) {
        const uint64_t bitLen = len_;          // capture BEFORE padding
        const uint8_t pad = 0x80;
        update(&pad, 1);
        const uint8_t zero = 0;
        while (bufLen_ != 56) update(&zero, 1);
        uint8_t lenb[8];
        for (int i = 0; i < 8; ++i) lenb[i] = uint8_t(bitLen >> (56 - 8 * i));
        std::memcpy(buf_ + 56, lenb, 8);       // raw: must not touch len_
        block(buf_);
        bufLen_ = 0;
        for (int i = 0; i < 8; ++i) {
            out[4*i+0]=uint8_t(h_[i]>>24); out[4*i+1]=uint8_t(h_[i]>>16);
            out[4*i+2]=uint8_t(h_[i]>>8);  out[4*i+3]=uint8_t(h_[i]);
        }
    }
private:
    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const uint8_t* p) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(p[4*i])<<24)|(uint32_t(p[4*i+1])<<16)|
                   (uint32_t(p[4*i+2])<<8)|uint32_t(p[4*i+3]);
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15]>>3);
            const uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2]>>10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h_[0],b=h_[1],c=h_[2],d=h_[3],e=h_[4],f=h_[5],g=h_[6],h=h_[7];
        static const uint32_t K[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = h + S1 + ch + K[i] + w[i];
            const uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
            const uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + mj;
            h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h_[0]+=a;h_[1]+=b;h_[2]+=c;h_[3]+=d;h_[4]+=e;h_[5]+=f;h_[6]+=g;h_[7]+=h;
    }
    uint32_t h_[8];
    uint64_t len_;
    uint8_t  buf_[64];
    size_t   bufLen_;
};

std::string toHex(const uint8_t* d, size_t n) {
    static const char* hex = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s += hex[d[i] >> 4];
        s += hex[d[i] & 15];
    }
    return s;
}

/// Streamed SHA-256 of a file (empty string on error). Capped at 8 GiB.
std::string fileSha256(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    Sha256 sha;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) sha.update(buf, n);
    const bool err = std::ferror(f) != 0;
    std::fclose(f);
    if (err) return {};
    uint8_t out[32];
    sha.final(out);
    return toHex(out, 32);
}

bool fileExistsWithSize(const std::string& path, uint64_t* sizeOut) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    const bool ok = usbutils::seek64(f, 0, SEEK_END);
    const int64_t sz = ok ? usbutils::tell64(f) : -1;
    std::fclose(f);
    if (sz < 0) return false;
    if (sizeOut) *sizeOut = uint64_t(sz);
    return true;
}

std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    const char sep =
#if defined(_WIN32)
        '\\';
#else
        '/';
#endif
    std::string s = a;
    if (s.back() == '/' || s.back() == '\\') s.pop_back();
    if (b.front() == '/' || b.front() == '\\') return s + b;
    return s + sep + b;
}

} // namespace

namespace usbutils {

// ---------------------------------------------------------------------------
// registry
// ---------------------------------------------------------------------------

const std::vector<SupportFile>& supportFileRegistry() {
    static const std::vector<SupportFile> kReg = {
        // role,              fileName,               required, feature,                                 size,      sha256
        { SupportRole::Ps2Load,  "PS2LOAD.ELF",          true,  "PS2Load inject",
          218252ull, "50a418bdd965889bd67bc8ca68c64900e2bbe88f3930c51af61ef6c51cff3e46" },
        { SupportRole::DnasIndex,"usbutil.dni",          true,  "DNAS patch (package index)",
          2104ull,   "69b85962ec09526f5b2f7e6c54e8bc4bd216fc059e6cf294b9122e0a6eac652c" },
        { SupportRole::DnasData, "usbutil.dna",          true,  "DNAS patch (IOPRP images)",
          5686213ull,"5f9101608719fb3f7c480b4357ea95669c47c24c9d2cee372e5df8b5922b1351" },
        { SupportRole::GameNotes,"usbutil.ljs",          true,  "Game compatibility notes",
          34419ull,  "2110c7225587ba95b0270dd32c1dfb1f862399507ed367a9ef84d889d3da747b" },
        { SupportRole::ToolConfig,"usbutil.aso",         false, "Tool settings persistence",
          171ull,    "92c6ec278340f220ac8637ce23e4d2932f6f0be191f91956cde1beff3ea00830" },
        { SupportRole::Recovery, "usbutil.rcn",          false, "Recovery-game notes",
          8079ull,   "fe3d284df4f9419067a2256c5bacb4cda896fad9c30eaccfdee352382d0d028d" },
        { SupportRole::IoprpImage,"SCEE_DD/IOPRP16.IMG", false, "Standalone IOPRP images",
          91909ull,  "520d74545b4e24dc25ab0d01c660da271c199080d0834b38a20114caeadc991d" },
        { SupportRole::IoprpImage,"SCEE_DD/IOPRP165.IMG",false, "Standalone IOPRP images",
          98901ull,  "9a3f9d337a23c71ade451e95f7fd03e038f4cc4b21d47b3032c57591c83a4557" },
        { SupportRole::IoprpImage,"SCEE_DD/IOPRP20.IMG", false, "Standalone IOPRP images",
          109809ull, "8613460cac02cf87a65f23446b86a04a9072a1faca796b05e1d67b910a31316a" },
        { SupportRole::IoprpImage,"SCEE_DD/IOPRP202.IMG",false, "Standalone IOPRP images",
          110993ull, "e4857da724efb48213d4cb805637b461846767e4dd659905c32d9a007f5f90cc" },
    };
    return kReg;
}

// ---------------------------------------------------------------------------
// search paths
// ---------------------------------------------------------------------------

std::string executableDir() {
    // /proc/self/exe (Linux), PEs get argv[0]-relative probing via cwd
    char buf[1024];
#if defined(_WIN32)
    const DWORD n = GetModuleFileNameA(nullptr, buf, DWORD(sizeof(buf) - 1));
    if (n == 0 || n >= sizeof(buf) - 1) return ".";
    buf[n] = '\0';
#else
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return ".";
    buf[n] = '\0';
#endif
    std::string s(buf);
    const size_t cut = s.find_last_of("/\\");
    return (cut == std::string::npos) ? "." : s.substr(0, cut);
}

std::vector<std::string> supportSearchPaths(const std::vector<std::string>& extraRoots) {
    std::vector<std::string> roots = extraRoots;
    const std::string exeDir = executableDir();
    const char* cwd = std::getenv("USBUTILS_DATA");
    if (cwd && *cwd) roots.push_back(cwd);

    // exe dir and its usual layouts
    roots.push_back(exeDir + "/data");
    roots.push_back(exeDir);
    roots.push_back(exeDir + "/..");
    roots.push_back(exeDir + "/../data");
    // working directory (repo checkouts, debug runs)
    roots.push_back("data");
    roots.push_back(".");

    // de-dup while preserving order
    std::vector<std::string> out;
    for (auto& r : roots) {
        if (r.empty()) continue;
        if (std::find(out.begin(), out.end(), r) == out.end())
            out.push_back(r);
    }
    return out;
}

// ---------------------------------------------------------------------------
// checks
// ---------------------------------------------------------------------------

std::vector<SupportStatus> checkSupportFiles(const std::vector<std::string>& extraRoots) {
    const std::vector<std::string> roots = supportSearchPaths(extraRoots);
    std::vector<SupportStatus> out;
    for (const SupportFile& sf : supportFileRegistry()) {
        SupportStatus st;
        st.file = sf;
        for (const std::string& root : roots) {
            const std::string cand = joinPath(root, sf.fileName);
            if (fileExistsWithSize(cand, &st.size)) {
                st.found = true;
                st.foundPath = cand;
                break;
            }
        }
        if (st.found && *sf.sha256) {
            // size shortcut first (cheap), full hash on match
            st.verified = (st.size == sf.expectedSize) &&
                          (fileSha256(st.foundPath) == sf.sha256);
        }
        out.push_back(std::move(st));
    }
    return out;
}

std::string findSupportFile(const char* fileName,
                            const std::vector<std::string>& extraRoots) {
    const std::vector<std::string> roots = supportSearchPaths(extraRoots);
    for (const std::string& root : roots) {
        const std::string cand = joinPath(root, fileName);
        uint64_t sz = 0;
        if (fileExistsWithSize(cand, &sz)) return cand;
    }
    return {};
}

} // namespace usbutils
