// SPDX-License-Identifier: MIT
/*
 * USBUtils — open reimplementation of USBUtil v2.2 (PS2 USBExtreme manager)
 * Copyright (c) 2025 AldySan
 * Based on the reverse-engineering of "USBUtil v2.2 rev1.0" by ISEKO (2009).
 */
#include "update.h"

#include "port.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

#ifndef USBUTILS_VERSION
#define USBUTILS_VERSION "0.0.0"
#endif

namespace usbutils {

const char* kProjectReleasesUrl =
    "https://github.com/AldySan/USBUtils_src/releases";

namespace {

const char* kApiUrl =
    "https://api.github.com/repos/AldySan/USBUtils_src/releases/latest";
const char* kAtomUrl =
    "https://github.com/AldySan/USBUtils_src/releases.atom";
const char* kUserAgent = "usbutils/" USBUTILS_VERSION;
constexpr size_t kMaxResponse = 64 * 1024;    // release JSON is ~2-5 KB

// -------------------------------------------------------------------------
// transport
// -------------------------------------------------------------------------

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

std::string httpGet(const char* url, int timeoutSec) {
    // split "https://api.github.com/.../latest" into host + path
    std::string u = url;
    const bool tls = u.rfind("https://", 0) == 0;
    const size_t scheme = u.find("://") + 3;
    const size_t slash = u.find('/', scheme);
    const std::string host = u.substr(scheme, slash - scheme);
    const std::string path = slash == std::string::npos ? "/" : u.substr(slash);

    std::string out;
    HINTERNET net = WinHttpOpen(L"usbutils", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!net) return out;
    WinHttpSetTimeouts(net, timeoutSec * 1000, timeoutSec * 1000,
                       timeoutSec * 1000, timeoutSec * 1000);

    std::wstring whost(host.begin(), host.end());
    std::wstring wpath(path.begin(), path.end());
    HINTERNET conn = WinHttpConnect(net, whost.c_str(),
                                    INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (conn) {
        HINTERNET req = WinHttpOpenRequest(conn, L"GET", wpath.c_str(),
                                           nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           WINHTTP_FLAG_SECURE);
        if (req) {
            if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(req, nullptr)) {
                DWORD size = 0;
                do {
                    size = 0;
                    if (!WinHttpQueryDataAvailable(req, &size)) break;
                    if (size == 0) break;
                    if (out.size() + size > kMaxResponse) size =
                        static_cast<DWORD>(kMaxResponse - out.size());
                    std::vector<char> buf(size);
                    DWORD read = 0;
                    if (!WinHttpReadData(req, buf.data(), size, &read)) break;
                    out.append(buf.data(), read);
                } while (size > 0 && out.size() < kMaxResponse);
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(conn);
    }
    WinHttpCloseHandle(net);
    return out;
}

#else  // POSIX — prefer the curl binary, fall back to wget

/// Run `cmd`, capture stdout (up to kMaxResponse). Empty string on failure.
std::string capture(const std::string& cmd) {
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return {};
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0 &&
           out.size() < kMaxResponse)
        out.append(buf, n);
    pclose(p);
    return out;
}

std::string httpGet(const char* url, int timeoutSec) {
    std::string out = capture("curl -s -L -m " + std::to_string(timeoutSec) +
                              " -H 'User-Agent: " + kUserAgent +
                              "' '" + url + "' 2>/dev/null");
    if (out.empty())
        out = capture("wget -q -T " + std::to_string(timeoutSec) +
                      " -O - '" + url + "' 2>/dev/null");
    return out;
}

#endif

// -------------------------------------------------------------------------
// minimal JSON field extraction: find "key":"value"
// -------------------------------------------------------------------------

std::string jsonStringValue(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t p = json.find(needle);
    if (p == std::string::npos) return {};
    p = json.find(':', p + needle.size());
    if (p == std::string::npos) return {};
    ++p;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
    if (p >= json.size() || json[p] != '"') return {};
    ++p;
    std::string value;
    while (p < json.size() && json[p] != '"') {
        if (json[p] == '\\' && p + 1 < json.size()) {
            ++p;                                    // escaped char: keep next
            if (json[p] != '"') value += '\\';      // keep other escapes
        }
        value += json[p++];
    }
    return value;
}

} // namespace

ReleaseInfo fetchLatestRelease() {
    ReleaseInfo info;

    // ---- primary: the REST API ------------------------------------------
    const std::string body = httpGet(kApiUrl, 10);
    if (!body.empty()) {
        info.tag = jsonStringValue(body, "tag_name");
        info.url = jsonStringValue(body, "html_url");
        if (!info.tag.empty()) {
            if (info.url.empty()) info.url = kProjectReleasesUrl;
            info.ok = true;
            return info;
        }
    }

    // ---- fallback: the releases.atom feed (no API rate limits) ----------
    // First <entry> <id> looks like "tag:github.com,2008:Repository/.../v0.0.2"
    const std::string atom = httpGet(kAtomUrl, 10);
    const size_t entry = atom.find("<entry>");
    if (entry != std::string::npos) {
        const size_t idBegin = atom.find("<id>", entry);
        const size_t idEnd   = atom.find("</id>", idBegin);
        if (idBegin != std::string::npos && idEnd != std::string::npos) {
            const std::string idVal = atom.substr(idBegin + 4, idEnd - idBegin - 4);
            const size_t slash = idVal.rfind('/');
            if (slash != std::string::npos && slash + 1 < idVal.size()) {
                info.tag = idVal.substr(slash + 1);
                info.url = kProjectReleasesUrl;
                info.ok = true;
                return info;
            }
        }
    }

    info.error = body.find("rate limit") != std::string::npos
                     ? "GitHub API rate limit hit, and the release feed was "
                       "unreachable too"
                     : "no reply from GitHub (offline or blocked)";
    return info;
}

bool isVersionNewer(const std::string& current, const std::string& tag) {
    auto clean = [](std::string v) {
        if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
        return v;
    };
    const std::string a = clean(current), b = clean(tag);

    auto split = [](const std::string& s) {
        std::vector<int> parts;
        size_t p = 0;
        while (p <= s.size()) {
            size_t d = s.find('.', p);
            const std::string tok = s.substr(p, d == std::string::npos
                                                 ? std::string::npos : d - p);
            parts.push_back(tok.empty() ? 0 : std::atoi(tok.c_str()));
            if (d == std::string::npos) break;
            p = d + 1;
        }
        return parts;
    };

    const std::vector<int> va = split(a), vb = split(b);
    const size_t n = std::max(va.size(), vb.size());
    for (size_t i = 0; i < n; ++i) {
        const int x = i < va.size() ? va[i] : 0;
        const int y = i < vb.size() ? vb[i] : 0;
        if (y > x) return true;
        if (y < x) return false;
    }
    return false;
}

} // namespace usbutils
