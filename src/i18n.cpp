#include "i18n.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "util.h"

namespace json = nlohmann;

namespace i18n {

namespace {

std::unordered_map<std::string, std::string> g_table;
std::string g_code = "en";

std::string auto_detect() {
    const char* env = std::getenv("LANG");
    if (env && *env) {
        std::string s(env);
        size_t n = s.find_first_of("._@");
        std::string base = util::to_lower(s.substr(0, n));
        if (base == "ru") return "ru";
        if (base == "en") return "en";
    }
    LANGID lid = GetUserDefaultUILanguage();
    if (PRIMARYLANGID(lid) == LANG_RUSSIAN) return "ru";
    return "en";
}

void load_catalog(const std::string& code) {
    if (code == "en") return;  // английский — база, каталог не нужен
    std::string path = util::join_path(util::join_path(util::exe_dir(), "lang"), code + ".json");
    std::string text = util::read_text(path);
    if (text.empty()) return;
    try {
        json::json j = json::json::parse(text);
        if (!j.is_object()) return;
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.value().is_string()) g_table[it.key()] = it.value().get<std::string>();
        }
    } catch (...) {
        // Битый каталог не блокируем: остаёмся на английской базе.
    }
}

}  // namespace

void init(const std::string& cli_flag, const std::string& settings_lang) {
    std::string code;
    std::string cf = util::to_lower(cli_flag);
    if (cf == "ru" || cf == "en") code = cf;
    if (code.empty()) {
        const char* env = std::getenv("LLAO_LANG");
        std::string ev = env ? util::to_lower(env) : "";
        if (ev == "ru" || ev == "en") code = ev;
    }
    if (code.empty()) {
        std::string sv = util::to_lower(settings_lang);
        if (sv == "ru" || sv == "en") code = sv;
    }
    if (code.empty()) code = auto_detect();
    g_code = code;
    load_catalog(code);
}

std::string code() { return g_code; }

std::string str(const std::string& key) {
    auto it = g_table.find(key);
    if (it != g_table.end()) return it->second;
    return key;
}

std::string vfmt(const std::string& f, va_list ap) {
    int cap = 256;
    for (;;) {
        std::string buf(cap, '\0');
        va_list cp;
        va_copy(cp, ap);
        int n = vsnprintf(&buf[0], cap, f.c_str(), cp);
        va_end(cp);
        if (n >= 0 && n < cap) {
            buf.resize(n);
            return buf;
        }
        cap = (n >= 0) ? n + 1 : cap * 2;
        if (cap > (1 << 24)) return f;
    }
}

std::string fmt(const char* key, ...) {
    std::string f = str(key);
    va_list ap;
    va_start(ap, key);
    std::string out = vfmt(f, ap);
    va_end(ap);
    return out;
}

}  // namespace i18n
