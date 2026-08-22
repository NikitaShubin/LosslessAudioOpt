#include "util.h"

#include <windows.h>

#include <cstring>
#include <filesystem>
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

std::string process_id() {
    return std::to_string((unsigned long)GetCurrentProcessId());
}

std::string machine_cpu() {
    std::wstring out;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0,
                      KEY_READ, &key) == ERROR_SUCCESS) {
        wchar_t buf[256];
        DWORD size = sizeof(buf);
        if (RegQueryValueExW(key, L"ProcessorNameString", nullptr, nullptr,
                             (LPBYTE)buf, &size) == ERROR_SUCCESS && size > 0)
            out = buf;
        RegCloseKey(key);
    }
    return out.empty() ? std::string("unknown") : w2u(out);
}

std::string machine_host() {
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(buf, &size)) return w2u(buf);
    return std::string("unknown");
}

std::string machine_id() {
    std::string s = machine_cpu() + " | " + machine_host();
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    const char* hx = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 0; i < 16; i++) out[i] = hx[(h >> (60 - i * 4)) & 0xf];
    return out;
}

void set_thread_below_normal() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
}

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
    fs::remove(fs::u8path(p), ec);
    if (!ec) return true;
    // Антивирус/индексатор могут короткое время удерживать свежезаписанный
    // файл (ERROR_SHARING_VIOLATION); повторяем с нарастающей паузой.
    for (int attempt = 0; attempt < 6; attempt++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150 * (attempt + 1)));
        ec.clear();
        fs::remove(fs::u8path(p), ec);
        if (!ec) return true;
    }
    return false;
}

// Перекодирует ANSI-байты (кодовая страница cp) в UTF-8; пустая строка при ошибке.
std::string ansi_to_utf8(const std::string& s, UINT cp) {
    std::wstring w = utf8_to_cp(s, cp);
    return w.empty() ? std::string() : utf8_from_cp(w, CP_UTF8);
}

std::string ec_text(const std::error_code& ec) {
    if (!ec) return {};
    std::string m = ec.message();
    if (m.empty()) return m;
    std::string conv = ansi_to_utf8(m, GetACP());
    if (!conv.empty()) return conv;
    return sanitize_utf8(m);
}

uint64_t file_size(const std::string& p) {
    std::error_code ec;
    auto sz = fs::file_size(fs::u8path(p), ec);
    return ec ? 0 : (uint64_t)sz;
}

namespace {
// Открывает файл НЕнаследуемым дескриптором с FILE_SHARE_DELETE. CreateFileW без
// SECURITY_ATTRIBUTES создаёт не-наследуемые хендлы, поэтому кодеки-потомки
// (спавн с bInheritHandles=TRUE) не могут унаследовать наши файловые дескрипторы
// и держать транзитные .llao-tmp. FILE_SHARE_DELETE дополнительно гарантирует,
// что переименование/удаление не срывается даже если файл кто-то держит.
HANDLE open_shared(const std::string& p, DWORD access, DWORD creation) {
    return CreateFileW(util::u2w(p).c_str(), access,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                       creation, FILE_ATTRIBUTE_NORMAL, nullptr);
}
}  // namespace

std::vector<uint8_t> read_file(const std::string& p) {
    std::vector<uint8_t> data;
    HANDLE h = open_shared(p, GENERIC_READ, OPEN_EXISTING);
    if (h == INVALID_HANDLE_VALUE) return data;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0) {
        CloseHandle(h);
        return data;
    }
    data.resize((size_t)sz.QuadPart);
    DWORD total = 0;
    while (total < data.size()) {
        DWORD rd = 0;
        DWORD chunk = (DWORD)std::min<size_t>(data.size() - total, 0x7FFFFFFF);
        if (!ReadFile(h, data.data() + total, chunk, &rd, nullptr) || rd == 0) {
            data.clear();
            break;
        }
        total += rd;
    }
    CloseHandle(h);
    return data;
}

bool write_file(const std::string& p, const std::vector<uint8_t>& data) {
    HANDLE h = open_shared(p, GENERIC_WRITE, CREATE_ALWAYS);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    DWORD total = 0;
    while (total < data.size()) {
        DWORD wr = 0;
        DWORD chunk = (DWORD)std::min<size_t>(data.size() - total, 0x7FFFFFFF);
        if (!WriteFile(h, data.data() + total, chunk, &wr, nullptr) || wr == 0) {
            ok = false;
            break;
        }
        total += wr;
    }
    if (!CloseHandle(h)) ok = false;
    return ok;
}

std::string read_text(const std::string& p) {
    auto d = read_file(p);
    return std::string(d.begin(), d.end());
}

bool write_text(const std::string& p, const std::string& s) {
    return write_file(p, std::vector<uint8_t>(s.begin(), s.end()));
}

bool copy_file(const std::string& src, const std::string& dst) {
    HANDLE hs = open_shared(src, GENERIC_READ, OPEN_EXISTING);
    if (hs == INVALID_HANDLE_VALUE) return false;
    HANDLE hd = open_shared(dst, GENERIC_WRITE, CREATE_ALWAYS);
    if (hd == INVALID_HANDLE_VALUE) {
        CloseHandle(hs);
        return false;
    }
    std::vector<char> buf(1 << 16);
    bool ok = true;
    for (;;) {
        DWORD rd = 0;
        if (!ReadFile(hs, buf.data(), (DWORD)buf.size(), &rd, nullptr) || rd == 0) break;
        DWORD off = 0;
        while (off < rd) {
            DWORD wr = 0;
            if (!WriteFile(hd, buf.data() + off, rd - off, &wr, nullptr) || wr == 0) {
                ok = false;
                break;
            }
            off += wr;
        }
        if (!ok) break;
    }
    CloseHandle(hs);
    if (!CloseHandle(hd)) ok = false;
    return ok;
}

ReplaceResult replace_file(const std::string& original, const std::string& tmp,
                           const std::string& backup, const std::string& final_name) {
    ReplaceResult res;
    auto rename_retry = [](const std::string& from, const std::string& to) -> std::string {
        std::error_code ec;
        // Внешний процесс (обычно антивирус real-time scan) может удерживать
        // свежезаписанный файл несколько секунд — ретраи с нарастающей паузой
        // суммарно ~12 с, иначе rename срывается с ERROR_SHARING_VIOLATION.
        for (int attempt = 0; attempt < 15; attempt++) {
            ec.clear();
            fs::rename(fs::u8path(from), fs::u8path(to), ec);
            if (!ec) return {};
            if (attempt < 8)
                std::this_thread::sleep_for(std::chrono::milliseconds(100 * (attempt + 1)));
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        }
        return ec_text(ec);
    };

    // Зачищаем свой же backup от прерванного предыдущего запуска.
    std::error_code ec;
    fs::remove(fs::u8path(backup), ec);

    const std::string target = final_name.empty() ? original : final_name;

    std::string err = rename_retry(original, backup);
    if (!err.empty()) {
        res.error = "could not move the original to " + base_name(backup) + ": " + err;
        return res;
    }

    err = rename_retry(tmp, target);
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

std::string normalize_output(const std::string& s) {
    // Шаг 1: \r → \n (возврат каретки = новая страница вывода)
    std::string step1;
    step1.reserve(s.size());
    for (char c : s) {
        if (c == '\r') step1 += '\n';
        else step1 += c;
    }
    // Шаг 2: удалить все управляющие символы кроме \n и \t
    std::string step2;
    step2.reserve(step1.size());
    for (char c : step1) {
        unsigned char uc = (unsigned char)c;
        if (uc == '\n' || uc == '\t' || uc >= 0x20) step2 += c;
    }
    // Шаг 3: схлопнуть последовательные пустые строки в одну
    std::string out;
    out.reserve(step2.size());
    bool prev_nl = false;
    for (char c : step2) {
        if (c == '\n') {
            if (!prev_nl) out += c;
            prev_nl = true;
        } else {
            out += c;
            prev_nl = false;
        }
    }
    // Шаг 4: trim
    size_t start = out.find_first_not_of(" \t\n");
    if (start == std::string::npos) return {};
    size_t end = out.find_last_not_of(" \t\n");
    return out.substr(start, end - start + 1);
}

}  // namespace util
