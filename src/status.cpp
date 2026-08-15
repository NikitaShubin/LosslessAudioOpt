#include "status.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "out.h"
#include "util.h"

namespace status {

namespace {

constexpr size_t kNameCol = 40;
constexpr size_t kWinCol = 20;
constexpr size_t kPctCol = 6;

struct Row {
    std::string label;
    size_t total = 0;
    size_t done = 0;
    bool finished = false;
    std::string winner;
    double pct = 0;  // <0 = прочерк
};

std::mutex g_m;
bool g_init = false;
bool g_interactive = false;
size_t g_total = 0;
std::vector<Row> g_rows;  // индекс = порядковый номер файла
size_t g_drawn = 0;       // сколько строк статуса нарисовано
std::chrono::steady_clock::time_point g_last;
DWORD g_orig_mode = 0;
bool g_orig_mode_valid = false;

// Ширина поля счётчика по числу файлов (1..).
size_t counter_width() {
    size_t w = 1, t = g_total;
    while (t >= 10) {
        t /= 10;
        w++;
    }
    return w;
}

std::wstring u2w(const std::string& s) { return util::u2w(s); }
std::string w2u(const std::wstring& s) { return util::w2u(s); }

// Выравнивание строки по ширине (счёт в широких символах). Слева.
std::string pad_left(const std::string& s, size_t width) {
    std::wstring w = u2w(s);
    if (w.size() >= width) {
        w = w.substr(0, width);
        return w2u(w);
    }
    return s + std::string(width - w.size(), ' ');
}

// Выравнивание строки по ширине (счёт в широких символах). Справа.
std::string pad_right(const std::string& s, size_t width) {
    std::wstring w = u2w(s);
    if (w.size() >= width) {
        w = w.substr(0, width);
        return w2u(w);
    }
    return std::string(width - w.size(), ' ') + s;
}

std::string truncate(const std::wstring& w, size_t width) {
    if (w.size() <= width) return w2u(w);
    if (width >= 3) return w2u(w.substr(0, width - 3) + L"...");
    return w2u(w.substr(0, width));
}

// Подняться на начало блока статусных строк и встать на колонку 0.
void move_to_top() {
    if (g_drawn == 0) return;
    out::text(stdout, "\x1b[" + std::to_string(g_drawn) + "A\r");
}

// Нарисовать блок строк от текущей позиции курсора (очистив снизу).
void draw_from_cursor() {
    std::string s;
    s += "\x1b[0J";
    for (size_t i = 0; i < g_total; i++) {
        const Row& r = g_rows[i];
        if (r.label.empty()) continue;  // файл ещё не начат
        s += counter(i);
        s += "  ";
        s += pad_name(r.label);
        s += "  ";
        if (r.finished) {
            s += pad_left(r.winner, kWinCol);
            s += " ";
            s += pct_col(r.pct);
        } else {
            size_t filled = r.total > 0 ? (r.done * kWinCol) / r.total : 0;
            if (filled > kWinCol) filled = kWinCol;
            s += "\x1b[42m" + std::string(filled, ' ');
            s += "\x1b[100m" + std::string(kWinCol - filled, ' ');
            s += "\x1b[0m";
            s += " ";
            s += pct_col(100.0 * (double)r.done / (double)(r.total ? r.total : 1));
        }
        if (i + 1 < g_total) s += "\n";
    }
    if (!s.empty()) out::text(stdout, s);
    g_drawn = 0;
    for (size_t i = 0; i < g_total; i++)
        if (!g_rows[i].label.empty()) g_drawn++;
    g_last = std::chrono::steady_clock::now();
}

void draw() {
    move_to_top();
    draw_from_cursor();
}

}  // namespace

void init(size_t total_files, bool no_status) {
    std::lock_guard<std::mutex> lk(g_m);
    g_total = total_files;
    g_rows.assign(total_files, Row{});
    g_init = true;
    g_interactive = false;
    g_drawn = 0;
    if (no_status) return;

    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (h == INVALID_HANDLE_VALUE || h == nullptr) return;
    if (!GetConsoleMode(h, &mode)) return;  // stdout не консоль (pipe/файл)
    DWORD new_mode = mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    if (!SetConsoleMode(h, new_mode)) return;  // VT не поддерживается
    g_orig_mode = mode;
    g_orig_mode_valid = true;
    g_interactive = true;
}

bool interactive() { return g_interactive; }

std::string counter(size_t idx) {
    size_t w = counter_width();
    char buf[64];
    snprintf(buf, sizeof(buf), "[%*zu/%zu]", (int)w, idx + 1, g_total);
    return buf;
}

std::string pad_name(const std::string& name) {
    std::wstring w = u2w(name);
    if (w.size() <= kNameCol) return w2u(w) + std::string(kNameCol - w.size(), ' ');
    return truncate(w, kNameCol);
}

std::string win_col(const std::string& s) { return pad_left(s, kWinCol); }

std::string pct_col(double pct) {
    char buf[32];
    if (pct < 0) {
        std::wstring w = u2w("—");
        return std::string(kPctCol - (w.size() < kPctCol ? w.size() : 0), ' ') + "—";
    }
    snprintf(buf, sizeof(buf), "%5.1f%%", pct);
    return std::string(buf);
}

void log(const std::string& line) {
    if (!g_interactive) {
        out::text(stdout, line);
        return;
    }
    std::lock_guard<std::mutex> lk(g_m);
    move_to_top();
    out::text(stdout, line);
    draw_from_cursor();
}

void error(const std::string& line) {
    if (!g_interactive) {
        out::text(stderr, line);
        return;
    }
    std::lock_guard<std::mutex> lk(g_m);
    move_to_top();
    out::text(stderr, line);
    draw_from_cursor();
}

void begin_file(size_t idx, const std::string& label, size_t total_tasks) {
    if (!g_interactive) return;
    std::lock_guard<std::mutex> lk(g_m);
    if (idx >= g_total) return;
    g_rows[idx].label = label;
    g_rows[idx].total = total_tasks;
    g_rows[idx].done = 0;
    g_rows[idx].finished = false;
    draw();
}

void tick(size_t idx, size_t done) {
    if (!g_interactive) return;
    std::lock_guard<std::mutex> lk(g_m);
    if (idx >= g_total) return;
    if (g_rows[idx].label.empty()) return;
    g_rows[idx].done = done;
    auto now = std::chrono::steady_clock::now();
    if (now - g_last < std::chrono::milliseconds(100)) return;
    draw();
}

void end_file(size_t idx, const std::string& label, const std::string& winner, double pct) {
    if (!g_interactive) return;
    std::lock_guard<std::mutex> lk(g_m);
    if (idx >= g_total) return;
    g_rows[idx].label = label;
    g_rows[idx].finished = true;
    g_rows[idx].winner = winner;
    g_rows[idx].pct = pct;
    draw();
}

void shutdown() {
    std::lock_guard<std::mutex> lk(g_m);
    if (!g_interactive) return;
    out::text(stdout, "\x1b[0m\n");
    if (g_orig_mode_valid) {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (h != INVALID_HANDLE_VALUE && h != nullptr)
            SetConsoleMode(h, g_orig_mode);
    }
    g_interactive = false;
    g_drawn = 0;
}

}  // namespace status
