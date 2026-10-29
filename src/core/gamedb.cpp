// SPDX-License-Identifier: MIT
#include "gamedb.h"

#include <cstdio>
#include <cctype>
#include "port.h"
#include <cstdint>

namespace usbutils {

static std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e-1] == ' ' || s[e-1] == '\t' || s[e-1] == '\r')) --e;
    return s.substr(b, e - b);
}

/// Parse one ljs line: optional leading '*', game name, optional
/// parenthesised compatibility note. Appends to `out`.
static void parseLjsLine(std::string line, std::vector<GameDbEntry>& out) {
    std::string s = trim(line);
    if (s.empty()) return;

    GameDbEntry e;
    if (s[0] == '*') { e.starred = true; s = trim(s.substr(1)); }
    // notes live in parentheses, e.g. "187 Ride or Die (cambia CDVDSTM.IRX + PS2LOAD)"
    const size_t op = s.rfind('(');
    if (op != std::string::npos && !s.empty() && s.back() == ')') {
        e.notes = s.substr(op + 1, s.size() - op - 2);
        s = trim(s.substr(0, op));
    }
    e.name = s;
    if (!e.name.empty()) out.push_back(std::move(e));
}

std::vector<GameDbEntry> loadGameDb(const std::string& ljsPath) {
    std::vector<GameDbEntry> out;
    FILE* f = std::fopen(ljsPath.c_str(), "rb");
    if (!f) return out;

    std::string line;
    int c;
    while ((c = std::fgetc(f)) != EOF) {
        if (c == '\n') {
            parseLjsLine(line, out);
            line.clear();
        } else {
            line += static_cast<char>(c);
        }
    }
    parseLjsLine(line, out);              // last line without trailing newline
    std::fclose(f);
    return out;
}

// ---------------------------------------------------------------------------
// rcn codec — see docs/formats/recovery_rcn.md
//   entry(i) = [len(raw) = L+1][L bytes XOR (len ^ index)]
//   data     = serial(11) + name
// ---------------------------------------------------------------------------

static bool looksLikeSerial(const std::string& s) {
    if (s.size() < 11) return false;
    for (size_t i = 0; i < 4; ++i)
        if (!std::isalnum(static_cast<unsigned char>(s[i]))) return false;
    if (s[4] != '_') return false;
    for (size_t i = 5; i < 11; ++i)
        if (!std::isdigit(static_cast<unsigned char>(s[i])) && s[i] != '.') return false;
    return true;
}

std::vector<RecoveryEntry> loadRecoveryDb(const std::string& rcnPath) {
    std::vector<RecoveryEntry> out;
    FILE* f = std::fopen(rcnPath.c_str(), "rb");
    if (!f) return out;

    std::vector<uint8_t> d;
    int c;
    while ((c = std::fgetc(f)) != EOF) d.push_back(static_cast<uint8_t>(c));
    std::fclose(f);

    size_t p = 0;
    size_t idx = 0;
    while (p < d.size()) {
        const uint8_t len = d[p];
        if (len < 13 || p + len > d.size()) break;
        bool matched = false;
        for (size_t j = idx; j < idx + 64 && !matched; ++j) {
            const uint8_t key = static_cast<uint8_t>(len ^ j);
            std::string dec(len - 1, '\0');
            for (size_t k = 0; k + 1 < len; ++k)
                dec[k] = static_cast<char>(d[p + 1 + k] ^ key);
            if (looksLikeSerial(dec.substr(0, 11))) {
                RecoveryEntry e;
                e.serial = dec.substr(0, 11);
                e.name   = dec.substr(11);
                out.push_back(std::move(e));
                p += len;
                idx = j + 1;
                matched = true;
            }
        }
        if (!matched) {          // unparseable record: skip one byte
            ++p;
            ++idx;
        }
    }
    return out;
}

bool saveRecoveryDb(const std::string& rcnPath,
                    const std::vector<RecoveryEntry>& games) {
    FILE* f = std::fopen(rcnPath.c_str(), "wb");
    if (!f) return false;
    bool ok = true;
    for (size_t i = 0; i < games.size() && ok; ++i) {
        std::string s = games[i].serial + games[i].name;
        // one rcn record: len byte + payload; cap at 255 total
        if (s.size() > 254) s.resize(254);
        const uint8_t len = static_cast<uint8_t>(s.size() + 1);
        const uint8_t key = static_cast<uint8_t>(len ^ i);
        if (std::fputc(len, f) == EOF) { ok = false; break; }
        for (char ch : s)
            if (std::fputc(static_cast<uint8_t>(ch) ^ key, f) == EOF) { ok = false; break; }
    }
    std::fclose(f);
    return ok;
}

} // namespace usbutils
