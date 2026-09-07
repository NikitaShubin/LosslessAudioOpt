#pragma once
#include <string>

#include "optimize.h"

// contract — канонические имена и разбор значений контракта демона (режим,
// верификация, пути). Единственная точка определения строк "optimize"/"restore",
// "all"/"winner"/"none" и чтения JSON-полей RPC. Монолит не использует.
namespace dsvc {

// Режим задачи: канонические строки и связь с optimize::JobMode.
inline optimize::JobMode parse_mode(const std::string& s, bool* ok = nullptr) {
    if (ok) *ok = true;
    if (s == "restore") return optimize::JobMode::Restore;
    if (s == "optimize") return optimize::JobMode::Optimize;
    if (ok) *ok = false;
    return optimize::JobMode::Optimize;
}

inline const char* mode_str(optimize::JobMode m) {
    return m == optimize::JobMode::Restore ? "restore" : "optimize";
}

// Режим верификации кандидатов.
inline optimize::Verify parse_verify(const std::string& s, bool* ok = nullptr) {
    if (ok) *ok = true;
    if (s == "all") return optimize::Verify::All;
    if (s == "winner") return optimize::Verify::Winner;
    if (s == "none") return optimize::Verify::None;
    if (ok) *ok = false;
    return optimize::Verify::Winner;
}

inline const char* verify_str(optimize::Verify v) {
    switch (v) {
        case optimize::Verify::All: return "all";
        case optimize::Verify::Winner: return "winner";
        default: return "none";
    }
}

// Нормализация пути для дедупликации и движка: единый разделитель '/',
// без хвостовых разделителей (каталоги). Пустое значение пустым и остаётся.
inline std::string normalize_path(const std::string& p) {
    std::string out;
    out.reserve(p.size());
    for (char c : p) out += (c == '\\') ? '/' : c;
    while (out.size() > 1 && out.back() == '/') out.pop_back();
    return out;
}

}  // namespace dsvc