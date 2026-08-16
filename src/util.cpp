#include "util.h"

#include <windows.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <thread>

namespace fs = std::filesystem;

namespace util {

static std::string utf8_from_cp(const std::wstring& w, UINT cp) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(cp, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(n, '\0');
    WideCharToMultiByte(cp, 0, w.data(), (int)w.size(), out.data(), n, nullptr, nullptr);
    return out;
}

static std::wstring utf8_to_cp(const std::string& s, UINT cp) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(n, L'\0');
    MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

std::wstring u2w(const std::string& s) { return utf8_to_cp(s, CP_UTF8); }
std::string w2u(const std::wstring& s) { return utf8_from_cp(s, CP_UTF8); }

std::string join_path(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == '\\' || a.back() == '/') return a + b;
    return a + "\\" + b;
}

std::string dir_name(const std::string& p) {
    auto pos = p.find_last_of("\\/");
    if (pos == std::string::npos) return "";
    return p.substr(0, pos);
}

std::string base_name(const std::string& p) {
    auto pos = p.find_last_of("\\/");
    if (pos == std::string::npos) return p;
    return p.substr(pos + 1);
}

std::string to_lower(const std::string& s) {
    std::string r = s;
    for (auto& c : r) c = (char)::tolower((unsigned char)c);
    return r;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    return s.substr(a, b - a);
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string exe_path() {
    wchar_t buf[MAX_PATH * 4] = {0};
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    return n ? w2u(std::wstring(buf, n)) : std::string();
}

std::string exe_dir() { return dir_name(exe_path()); }

bool file_exists(const std::string& p) {
    std::error_code ec;
    return fs::is_regular_file(fs::u8path(p), ec) && !ec;
}

bool dir_exists(const std::string& p) {
    std::error_code ec;
    return fs::is_directory(fs::u8path(p), ec) && !ec;
}

bool mkdirs(const std::string& p) {
    std::error_code ec;
    fs::create_directories(fs::u8path(p), ec);
    return !ec;
}

bool remove_file(const std::string& p) {
    std::error_code ec;
    return fs::remove(fs::u8path(p), ec) && !ec;
}

uint64_t file_size(const std::string& p) {
    std::error_code ec;
    auto sz = fs::file_size(fs::u8path(p), ec);
    return ec ? 0 : (uint64_t)sz;
}

std::vector<uint8_t> read_file(const std::string& p) {
    std::vector<uint8_t> data;
    std::ifstream f(fs::u8path(p), std::ios::binary);
    if (!f) return data;
    f.seekg(0, std::ios::end);
    std::streamoff len = f.tellg();
    f.seekg(0, std::ios::beg);
    if (len < 0) return data;
    data.resize((size_t)len);
    if (len > 0) f.read((char*)data.data(), len);
    return data;
}

bool write_file(const std::string& p, const std::vector<uint8_t>& data) {
    std::ofstream f(fs::u8path(p), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (!data.empty()) f.write((const char*)data.data(), (std::streamsize)data.size());
    return f.good();
}

std::string read_text(const std::string& p) {
    auto d = read_file(p);
    return std::string(d.begin(), d.end());
}

bool write_text(const std::string& p, const std::string& s) {
    std::ofstream f(fs::u8path(p), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(s.data(), (std::streamsize)s.size());
    return f.good();
}

bool copy_file(const std::string& src, const std::string& dst) {
    std::error_code ec;
    fs::copy_file(fs::u8path(src), fs::u8path(dst), fs::copy_options::overwrite_existing, ec);
    return !ec;
}

ReplaceResult replace_file(const std::string& original, const std::string& tmp,
                           const std::string& backup) {
    ReplaceResult res;
    auto rename_retry = [](const std::string& from, const std::string& to) -> std::string {
        std::error_code ec;
        for (int attempt = 0; attempt < 10; attempt++) {
            ec.clear();
            fs::rename(fs::u8path(from), fs::u8path(to), ec);
            if (!ec) return {};
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return ec.message();
    };

    // Зачищаем свой же backup от прерванного предыдущего запуска.
    std::error_code ec;
    fs::remove(fs::u8path(backup), ec);

    std::string err = rename_retry(original, backup);
    if (!err.empty()) {
        res.error = "could not move the original to " + base_name(backup) + ": " + err;
        return res;
    }

    err = rename_retry(tmp, original);
    if (!err.empty()) {
        std::string rb = rename_retry(backup, original);
        if (!rb.empty()) {
            res.original_lost = true;
            res.backup = backup;
            res.error = "could not move the candidate in place (" + err +
                        ") AND the rollback failed (" + rb + "); the original is saved as " +
                        backup;
        } else {
            res.error = "could not move the candidate in place (" + err +
                        "); the original was restored";
        }
        return res;
    }

    res.ok = true;
    fs::remove(fs::u8path(backup), ec);  // неудача удаления некритична
    return res;
}

uint64_t disk_free_bytes(const std::string& path) {
    std::wstring wpath = u2w(path.empty() ? "." : path);
    ULARGE_INTEGER free_avail{}, total{}, free_total{};
    if (!GetDiskFreeSpaceExW(wpath.c_str(), &free_avail, &total, &free_total)) {
        // Путь может не существовать (каталог ещё не создан) — пробуем "." .
        wpath = u2w(".");
        if (!GetDiskFreeSpaceExW(wpath.c_str(), &free_avail, &total, &free_total)) return 0;
    }
    return free_avail.QuadPart;
}

uint64_t avail_ram_bytes() {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) return 0;
    return ms.ullAvailPhys;
}

std::string find_in_path(const std::string& name) {
    std::vector<std::string> candidates;
    std::string n = name;
    if (n.find('.') == std::string::npos) {
        candidates.push_back(n + ".exe");
        candidates.push_back(n + ".bat");
        candidates.push_back(n + ".cmd");
        candidates.push_back(n);
    } else {
        candidates.push_back(n);
    }

    char buf[32767] = {0};
    DWORD len = GetEnvironmentVariableA("PATH", buf, sizeof(buf));
    if (len == 0 || len >= sizeof(buf)) return {};

    std::string path(buf);
    size_t start = 0;
    std::vector<std::string> dirs;
    while (start < path.size()) {
        size_t sep = path.find(';', start);
        std::string dir = path.substr(start, sep == std::string::npos ? std::string::npos : sep - start);
        if (!dir.empty()) dirs.push_back(dir);
        if (sep == std::string::npos) break;
        start = sep + 1;
    }

    for (const auto& cand : candidates) {
        for (const auto& dir : dirs) {
            std::string full = join_path(dir, cand);
            if (file_exists(full)) return full;
        }
    }
    return {};
}

std::string current_os() {
#ifdef _WIN32
    return "windows";
#else
    return "linux";
#endif
}

bool is_pe(const std::string& path) {
    auto data = read_file(path);
    return data.size() >= 2 && data[0] == 'M' && data[1] == 'Z';
}

static bool fnmatch_here(const std::string& pat, size_t pi, const std::string& name, size_t ni) {
    while (pi < pat.size()) {
        char pc = pat[pi];
        if (pc == '*') {
            // Несколько '*' подряд — как один.
            while (pi < pat.size() && pat[pi] == '*') pi++;
            if (pi == pat.size()) return true;
            for (size_t k = ni; k <= name.size(); k++) {
                if (fnmatch_here(pat, pi, name, k)) return true;
            }
            return false;
        }
        if (pc == '?') {
            if (ni >= name.size()) return false;
            pi++;
            ni++;
            continue;
        }
        if (ni >= name.size() || name[ni] != pc) return false;
        pi++;
        ni++;
    }
    return ni == name.size();
}

bool fnmatch(const std::string& pattern, const std::string& name) {
    return fnmatch_here(pattern, 0, name, 0);
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t pos = s.find(sep, start);
        if (pos == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

bool from_base64(const std::string& s, std::vector<uint8_t>* out) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<int> rev(256, -1);
    for (int i = 0; i < 64; i++) rev[(unsigned char)tbl[i]] = i;
    std::vector<uint8_t> res;
    int acc = 0, nbits = 0;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        int v = rev[(unsigned char)c];
        if (v < 0) {
            if (c == '=') break;
            return false;
        }
        acc = (acc << 6) | v;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            res.push_back((uint8_t)((acc >> nbits) & 0xff));
        }
    }
    *out = std::move(res);
    return true;
}

std::string sanitize_utf8(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        size_t need;
        if (c < 0x80) {
            out += (char)c;
            i++;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            need = 2;
        } else if ((c & 0xF0) == 0xE0) {
            need = 3;
        } else if ((c & 0xF8) == 0xF0) {
            need = 4;
        } else {
            out += '?';
            i++;
            continue;
        }
        bool ok = (i + need) <= s.size();
        if (ok && need == 2 && c < 0xC2) ok = false;
        if (ok && need == 3 && c == 0xE0 && (unsigned char)s[i + 1] < 0xA0) ok = false;
        if (ok && need == 3 && c == 0xED && (unsigned char)s[i + 1] > 0x9F) ok = false;
        if (ok && need == 4 && c == 0xF0 && (unsigned char)s[i + 1] < 0x90) ok = false;
        if (ok && need == 4 && c > 0xF4) ok = false;
        for (size_t k = 1; ok && k < need; k++) {
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) ok = false;
        }
        if (!ok) {
            out += '?';
            i++;
            continue;
        }
        out.append(s, i, need);
        i += need;
    }
    return out;
}

void sanitize_json(nlohmann::json& j) {
    if (j.is_string()) {
        j = sanitize_utf8(j.get<std::string>());
    } else if (j.is_object()) {
        nlohmann::json out = nlohmann::json::object();
        for (auto& [k, v] : j.items()) out[sanitize_utf8(k)] = std::move(v);
        for (auto& [k, v] : out.items()) sanitize_json(v);
        j = std::move(out);
    } else if (j.is_array()) {
        for (auto& v : j) sanitize_json(v);
    }
}

}  // namespace util
