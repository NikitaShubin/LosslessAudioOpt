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

constexpr size_t kPctCol = 6;

// Одна строка статуса = один файл. Во время обработки строка — полоса на всю
// ширину консоли (зелёная заливка по прогрессу, дальше серый фон, текст поверх).
// По завершении — простая строка: счётчик, имя файла, победитель, процент.
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
std::vector<Row> g_rows;   // индекс = порядковый номер файла
size_t g_visible = 0;      // сколько строк статуса занимает блок (растёт к низу)
bool g_overflow = false;   // блок заполнил почти весь экран — обычный построчный вывод
std::chrono::steady_clock::time_point g_last;
DWORD g_orig_mode = 0;
bool g_orig_mode_valid = false;

int g_width = 80;
int g_height = 25;
bool g_size_valid = false;

constexpr std::chrono::milliseconds kTickMin(100);

std::wstring u2w(const std::string& s) { return util::u2w(s); }
std::string w2u(const std::wstring& s) { return util::w2u(s); }

// Ширина поля счётчика по числу файлов (1..).
size_t counter_width() {
    size_t w = 1, t = g_total;
    while (t >= 10) {
        t /= 10;
        w++;
    }
    return w;
}

std::string counter(size_t idx) {
    size_t w = counter_width();
    char buf[64];
    snprintf(buf, sizeof(buf), "[%*zu/%zu]", (int)w, idx + 1, g_total);
    return buf;
}

std::string truncate(const std::wstring& w, size_t width) {
    if (w.size() <= width) return w2u(w);
    if (width >= 3) return w2u(w.substr(0, width - 3) + L"...");
    return w2u(w.substr(0, width));
}

std::string pct_col(double pct) {
    char buf[32];
    if (pct < 0) return std::string(kPctCol - 1, ' ') + "\xe2\x80\x94";  // —
    snprintf(buf, sizeof(buf), "%5.1f%%", pct);
    return std::string(buf);
}

// Текст строки без цвета/фона, обрезанный до W символов.
std::string row_text(const Row& r, size_t idx, int W) {
    std::string s = counter(idx) + " " + r.label + "  ";
    if (r.finished) {
        s += r.winner + " " + pct_col(r.pct);
    } else {
        s += pct_col(100.0 * (double)r.done / (double)(r.total ? r.total : 1));
    }
    return truncate(u2w(s), (size_t)W);
}

// Полоса прогресса на всю ширину W: зелёная заливка (42) до done/total,
// дальше серый фон (100); текст белым поверх. Текст статичен (кроме процента).
// Пробелы в тексте пишутся как обычные символы (а не пропускаются): иначе курсор
// не продвигается и текст склеивается без пробелов.
std::string strip_row(const Row& r, size_t idx, int W) {
    double f = r.total > 0 ? (double)r.done / (double)r.total : 0.0;
    int filled = (int)(f * W + 0.5);
    if (filled > W) filled = W;
    if (filled < 0) filled = 0;
    std::wstring body = u2w(row_text(r, idx, W));
    std::string s;
    s += "\x1b[42m" + std::string((size_t)filled, ' ');
    s += "\x1b[100m" + std::string((size_t)(W - filled), ' ');
    s += "\x1b[0m\r";
    // Текст поверх заливки: одна смена цвета на границе заполнения, все символы
    // (включая пробелы) пишутся — иначе пробелы пропадают и процент «прилипает».
    size_t split = body.size() < (size_t)filled ? body.size() : (size_t)filled;
    if (split > 0) s += "\x1b[37;42m" + w2u(body.substr(0, split));
    if (split < body.size()) s += "\x1b[37;100m" + w2u(body.substr(split));
    s += "\x1b[0m";
    return s;
}

// Строка результата: без фона, стирает предыдущее содержимое строки.
std::string finished_row(const Row& r, size_t idx, int W) {
    return "\x1b[2K" + row_text(r, idx, W);
}

std::string pos(int row, int col) {
    return "\x1b[" + std::to_string(row) + ";" + std::to_string(col) + "H";
}

// Последняя строка scroll-региона лога (низ региона = верх блока статуса).
int scroll_bottom() {
    int b = g_height - (int)g_visible;
    if (b < 1) b = 1;
    return b;
}

// Установить scroll-регион [1..scroll_bottom()]: лог скроллится над блоком,
// блок остаётся закреплённым у низа экрана.
void set_region() {
    out::text(stdout, "\x1b[1;" + std::to_string(scroll_bottom()) + "r");
}

// Перерисовать блок статусных строк и поставить курсор в зону лога.
void draw_locked() {
    if (!g_size_valid || g_overflow) return;
    set_region();
    int top = g_height - (int)g_visible + 1;
    for (size_t i = 0; i < g_visible; i++) {
        const Row& r = g_rows[i];
        std::string line = pos(top + (int)i, 1);
        line += r.finished ? finished_row(r, i, g_width) : strip_row(r, i, g_width);
        out::text(stdout, line);
    }
    out::text(stdout, pos(scroll_bottom(), 1));
    g_last = std::chrono::steady_clock::now();
}

// Гарантирует, что файл idx попал в блок (строки видны в порядке файлов).
// Пропущенные файлы приходят сразу в end_file без begin_file.
void ensure_visible_locked(size_t idx) {
    while (g_visible <= idx) {
        if ((int)g_visible >= g_height - 1) {  // блок заполнил почти весь экран
            g_overflow = true;
            out::text(stdout, "\x1b[r" + pos(g_height, 1));  // обычный скролл
            return;
        }
        g_visible++;
    }
}

// Курсор в последнюю строку scroll-региона и вывод строки лога.
void log_line(const std::string& line) {
    out::text(stdout, pos(scroll_bottom(), 1));
    out::text(stdout, line);
}

}  // namespace

void init(size_t total_files, bool no_status) {
    std::lock_guard<std::mutex> lk(g_m);
    g_total = total_files;
    g_rows.assign(total_files, Row{});
    g_init = true;
    g_interactive = false;
    g_visible = 0;
    g_overflow = false;
    g_width = 80;
    g_height = 25;
    g_size_valid = false;
    if (no_status) return;

    const char* force = getenv("LLAO_STATUS_FORCE");
    bool forced = force && force[0] == '1';
    const char* sz = getenv("LLAO_STATUS_SIZE");
    if (sz && *sz) {
        int W = 0, H = 0;
        if (sscanf(sz, "%dx%d", &W, &H) == 2 && W > 0 && H > 0) {
            g_width = W;
            g_height = H;
            g_size_valid = true;
        }
    }

    if (!forced) {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (h == INVALID_HANDLE_VALUE || h == nullptr) return;
        DWORD mode = 0;
        if (!GetConsoleMode(h, &mode)) return;  // stdout не консоль (pipe/файл)
        DWORD new_mode = mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        if (!SetConsoleMode(h, new_mode)) return;  // VT не поддерживается
        g_orig_mode = mode;
        g_orig_mode_valid = true;
    }

    if (!g_size_valid) {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        if (h != INVALID_HANDLE_VALUE && h != nullptr && GetConsoleScreenBufferInfo(h, &csbi)) {
            g_width = csbi.srWindow.Right - csbi.srWindow.Left + 1;
            g_height = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
            g_size_valid = true;
        }
    }

    g_interactive = true;
}

bool interactive() { return g_interactive; }

void log(const std::string& line) {
    if (!g_interactive) {
        out::text(stdout, line);
        return;
    }
    std::lock_guard<std::mutex> lk(g_m);
    if (!g_size_valid || g_overflow) {
        out::text(stdout, line);
        return;
    }
    log_line(line);
}

void error(const std::string& line) {
    if (!g_interactive) {
        out::text(stderr, line);
        return;
    }
    std::lock_guard<std::mutex> lk(g_m);
    if (!g_size_valid || g_overflow) {
        out::text(stderr, line);
        return;
    }
    log_line(line);
}

void begin_file(size_t idx, const std::string& label, size_t total_tasks) {
    if (!g_interactive) return;
    std::lock_guard<std::mutex> lk(g_m);
    if (idx >= g_total) return;
    g_rows[idx].label = label;
    g_rows[idx].total = total_tasks;
    g_rows[idx].done = 0;
    g_rows[idx].finished = false;
    if (!g_size_valid || g_overflow) return;
    ensure_visible_locked(idx);
    if (g_overflow) {
        out::text(stdout, row_text(g_rows[idx], idx, g_width) + "\n");
        return;
    }
    draw_locked();
}

void tick(size_t idx, size_t done) {
    if (!g_interactive) return;
    std::lock_guard<std::mutex> lk(g_m);
    if (idx >= g_total) return;
    if (g_rows[idx].label.empty()) return;
    if (!g_size_valid || g_overflow) return;
    g_rows[idx].done = done;
    auto now = std::chrono::steady_clock::now();
    if (now - g_last < kTickMin) return;
    draw_locked();
}

void end_file(size_t idx, const std::string& label, const std::string& winner, double pct) {
    if (!g_interactive) return;
    std::lock_guard<std::mutex> lk(g_m);
    if (idx >= g_total) return;
    g_rows[idx].label = label;
    g_rows[idx].finished = true;
    g_rows[idx].winner = winner;
    g_rows[idx].pct = pct;
    if (!g_size_valid || g_overflow) return;
    ensure_visible_locked(idx);
    if (g_overflow) {
        out::text(stdout, row_text(g_rows[idx], idx, g_width) + "\n");
        return;
    }
    draw_locked();
}

void shutdown() {
    std::lock_guard<std::mutex> lk(g_m);
    if (!g_interactive) return;
    if (g_size_valid && !g_overflow) {
        out::text(stdout, "\x1b[0m");
        out::text(stdout, "\x1b[r");                       // весь экран снова скроллится
        out::text(stdout, pos(g_height, 1));               // курсор вниз
        out::text(stdout, "\r\n");
    } else {
        out::text(stdout, "\x1b[0m\n");
    }
    if (g_orig_mode_valid) {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (h != INVALID_HANDLE_VALUE && h != nullptr) SetConsoleMode(h, g_orig_mode);
    }
    g_interactive = false;
    g_visible = 0;
    g_overflow = false;
}

}  // namespace status
