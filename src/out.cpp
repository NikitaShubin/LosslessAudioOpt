#include "out.h"

#include <windows.h>

#include <cstdarg>
#include <mutex>

#include "i18n.h"
#include "util.h"

namespace out {

namespace {

std::mutex g_mutex;

HANDLE handle_of(FILE* f) {
    return (f == stderr) ? GetStdHandle(STD_ERROR_HANDLE)
                         : GetStdHandle(STD_OUTPUT_HANDLE);
}

bool is_console(FILE* f) {
    HANDLE h = handle_of(f);
    if (h == INVALID_HANDLE_VALUE || h == nullptr) return false;
    DWORD mode = 0;
    return GetConsoleMode(h, &mode) != 0;
}

}  // namespace

void text(FILE* f, const std::string& s) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (is_console(f)) {
        std::wstring w = util::u2w(s);
        if (!w.empty()) {
            DWORD n = 0;
            WriteConsoleW(handle_of(f), w.c_str(), (DWORD)w.size(), &n, nullptr);
        }
    } else {
        fwrite(s.data(), 1, s.size(), f);
    }
    fflush(f);
}

void print(const char* key, ...) {
    std::string f = i18n::str(key);
    va_list ap;
    va_start(ap, key);
    std::string s = i18n::vfmt(f, ap);
    va_end(ap);
    text(stdout, s);
}

void error(const char* key, ...) {
    std::string f = i18n::str(key);
    va_list ap;
    va_start(ap, key);
    std::string s = i18n::vfmt(f, ap);
    va_end(ap);
    text(stderr, s);
}

void raw(FILE* f, const std::string& s) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!s.empty()) fwrite(s.data(), 1, s.size(), f);
    fflush(f);
}

}  // namespace out
