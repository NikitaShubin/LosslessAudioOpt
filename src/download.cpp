#include "download.h"

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

#include <filesystem>
#include <fstream>

#include "i18n.h"
#include "proc.h"
#include "util.h"

namespace download {

namespace {

#ifdef _WIN32
std::string last_error_text(const char* what) {
    DWORD err = GetLastError();
    return std::string(what) + i18n::str(": error code ") + std::to_string(err);
}

struct UrlParts {
    bool secure = false;
    INTERNET_PORT port = 80;
    std::wstring host;
    std::wstring path;
};

bool parse_url(const std::wstring& url, UrlParts* parts) {
    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t scheme[32], host[512], path[4096], extra[64];
    uc.lpszScheme = scheme;
    uc.dwSchemeLength = 32;
    uc.lpszHostName = host;
    uc.dwHostNameLength = 512;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 4096;
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 64;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;

    parts->secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    parts->port = uc.nPort ? uc.nPort : (parts->secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT);
    parts->host = std::wstring(host, uc.dwHostNameLength);
    std::wstring p(path, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength > 0 && extra[0] != 0) p += std::wstring(extra, uc.dwExtraInfoLength);
    if (p.empty()) p = L"/";
    parts->path = p;
    return true;
}

bool write_body(HINTERNET hReq, const std::string& dest, std::string* err) {
    std::ofstream f(std::filesystem::u8path(dest), std::ios::binary | std::ios::trunc);
    if (!f) {
        *err = i18n::fmt("could not create file: %s", dest.c_str());
        return false;
    }
    char buf[65536];
    DWORD n = 0;
    uint64_t total = 0;
    while (true) {
        BOOL ok = WinHttpReadData(hReq, buf, sizeof(buf), &n);
        if (!ok) {
            *err = last_error_text("WinHttpReadData") +
                   i18n::fmt(" (downloaded %s bytes)", std::to_string(total).c_str());
            return false;
        }
        if (n == 0) break;
        f.write(buf, n);
        if (!f.good()) {
            *err = i18n::fmt("file write error: %s", dest.c_str());
            return false;
        }
        total += n;
    }
    f.close();
    if (total == 0) {
        *err = i18n::str("server returned an empty response (0 bytes)");
        return false;
    }
    return true;
}

bool get_winhttp(const std::string& url, const std::string& dest, std::string* err) {
    HINTERNET hSession = WinHttpOpen(L"LLAO/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        *err = last_error_text("WinHttpOpen");
        return false;
    }

    std::wstring wurl = util::u2w(url);
    UrlParts cur;
    if (!parse_url(wurl, &cur)) {
        *err = i18n::fmt("could not parse URL: %s", url.c_str());
        WinHttpCloseHandle(hSession);
        return false;
    }

    bool ok = false;
    *err = i18n::str("unknown error");
    for (int redirect = 0; redirect < 12; redirect++) {
        HINTERNET hConn = WinHttpConnect(hSession, cur.host.c_str(), cur.port, 0);
        if (!hConn) {
            *err = last_error_text("WinHttpConnect");
            break;
        }
        HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", cur.path.c_str(), nullptr, nullptr, nullptr,
                                            cur.secure ? WINHTTP_FLAG_SECURE : 0);
        if (!hReq) {
            *err = last_error_text("WinHttpOpenRequest");
            WinHttpCloseHandle(hConn);
            break;
        }
        WinHttpSetTimeouts(hReq, 30000, 60000, 60000, 600000);
        if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0)) {
            *err = last_error_text("WinHttpSendRequest");
            WinHttpCloseHandle(hReq);
            WinHttpCloseHandle(hConn);
            break;
        }
        if (!WinHttpReceiveResponse(hReq, nullptr)) {
            *err = last_error_text("WinHttpReceiveResponse");
            WinHttpCloseHandle(hReq);
            WinHttpCloseHandle(hConn);
            break;
        }

        DWORD status = 0;
        DWORD len = sizeof(status);
        WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);

        if (status >= 300 && status < 400) {
            wchar_t loc[4096] = {0};
            DWORD loclen = sizeof(loc);
            if (WinHttpQueryHeaders(hReq, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                    loc, &loclen, WINHTTP_NO_HEADER_INDEX) && loc[0]) {
                std::wstring loc_s(loc);
                WinHttpCloseHandle(hReq);
                WinHttpCloseHandle(hConn);
                if (loc_s.compare(0, 7, L"http://") != 0 && loc_s.compare(0, 8, L"https://") != 0) {
                    std::wstring scheme = cur.secure ? L"https://" : L"http://";
                    loc_s = scheme + cur.host + (cur.port != (cur.secure ? 443 : 80) ? (L":" + std::to_wstring(cur.port)) : L"") + (loc_s.empty() || loc_s[0] != L'/' ? L"/" : L"") + loc_s;
                }
                if (parse_url(loc_s, &cur)) continue;
                *err = i18n::str("invalid Location in redirect");
                break;
            }
            *err = i18n::str("redirect without Location");
            WinHttpCloseHandle(hReq);
            WinHttpCloseHandle(hConn);
            break;
        }

        if (status != 200) {
            *err = i18n::fmt("HTTP %lu while downloading %s", (unsigned long)status, url.c_str());
            WinHttpCloseHandle(hReq);
            WinHttpCloseHandle(hConn);
            break;
        }
        ok = write_body(hReq, dest, err);
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConn);
        break;
    }

    WinHttpCloseHandle(hSession);
    if (!ok && util::file_exists(dest)) util::remove_file(dest);
    return ok;
}
#endif  // _WIN32

bool get_curl(const std::string& url, const std::string& dest, std::string* err) {
#ifdef _WIN32
    std::string curl = util::find_in_path("curl.exe");
    if (curl.empty()) {
        std::string local = util::join_path(util::exe_dir(), "bin");
        local = util::join_path(local, "curl");
        curl = util::join_path(local, "curl.exe");
        if (!util::file_exists(curl)) curl = util::join_path(util::exe_dir(), "curl.exe");
        if (!util::file_exists(curl)) curl.clear();
    }
#else
    std::string curl = util::find_in_path("curl");
#endif
    if (curl.empty()) {
        *err = i18n::str("curl.exe not found in PATH or bin/curl/");
        return false;
    }
    proc::Result r = proc::run({curl, "-f", "-L", "--retry", "2", "-sS", "-o", dest, url}, 300);
    if (!r.started || r.exit_code != 0 || !util::file_exists(dest)) {
        std::string out = util::trim(r.output);
        std::string detail = out.empty() ? "" : (i18n::str(": ") + out);
        *err = i18n::fmt("curl exited with code %d", r.exit_code) + detail;
        if (util::file_exists(dest)) util::remove_file(dest);
        return false;
    }
    return true;
}

}  // namespace

bool get(const std::string& url, const std::string& dest, std::string* err) {
#ifdef _WIN32
    std::string w_err;
    if (get_winhttp(url, dest, &w_err)) return true;
    std::string c_err;
    if (get_curl(url, dest, &c_err)) return true;
    *err = "WinHTTP: " + w_err + "; curl: " + c_err;
#else
    std::string c_err;
    if (get_curl(url, dest, &c_err)) return true;
    *err = c_err;
#endif
    return false;
}

}  // namespace download
