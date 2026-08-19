#include "screen.h"

#include <string>
#include <vector>

#include "out.h"
#include "util.h"

namespace screen {

namespace {

const Cell kDefault;  // ch=' ', fg=7, bg=0, bold=false

}  // namespace

int disp_width(wchar_t c) {
    // East Asian Wide (W) / Fullwidth (F) — упрощённые диапазоны (без зеркальных
    // и редких). Всё остальное — узкие символы (1 колонка).
    if (c < 0x1100) return 1;
    if (c <= 0x115F) return 2;                       // Hangul Jamo
    if (c >= 0x2E80 && c <= 0x303E) return 2;        // CJK Radicals .. CJK Symbols
    if (c >= 0x3041 && c <= 0x33FF) return 2;        // Hiragana..CJK Compat
    if (c >= 0x3400 && c <= 0x4DBF) return 2;        // CJK Ext A
    if (c >= 0x4E00 && c <= 0x9FFF) return 2;        // CJK Unified
    if (c >= 0xA000 && c <= 0xA4CF) return 2;        // Yi
    if (c >= 0xAC00 && c <= 0xD7A3) return 2;        // Hangul Syllables
    if (c >= 0xF900 && c <= 0xFAFF) return 2;        // CJK Compat Ideographs
    if (c >= 0xFE30 && c <= 0xFE4F) return 2;        // CJK Compat Forms
    if (c >= 0xFF00 && c <= 0xFF60) return 2;        // Fullwidth Forms
    if (c >= 0xFFE0 && c <= 0xFFE6) return 2;        // Fullwidth Signs
    return 1;
}

std::wstring truncate_cols(const std::wstring& s, int width) {
    if (width <= 0) return L"";
    std::wstring out;
    int col = 0;
    for (wchar_t ch : s) {
        int w = disp_width(ch);
        if (col + w > width) break;
        out += ch;
        col += w;
    }
    return out;
}

void Buffer::resize(int width, int height) {
    if (width == width_ && height == height_) return;
    width_ = width;
    height_ = height;
    // Экран очищается целиком: прежний кадр другого размера уже не влезает,
    // и дифф с ним был бы бессмысленным (см. описание в screen.h).
    out::text(stdout, "\x1b[2J\x1b[1;1H");
    cur_.assign((size_t)width_ * height_, kDefault);
    next_.assign((size_t)width_ * height_, kDefault);
}

Cell& Buffer::cell(int row, int col) {
    static Cell dummy;
    if (row < 0 || row >= height_ || col < 0 || col >= width_) return dummy;
    return next_[(size_t)row * width_ + col];
}

void Buffer::fill(int row, int col, int len, wchar_t ch, uint8_t fg, uint8_t bg,
                  bool bold) {
    if (row < 0 || row >= height_) return;
    if (col < 0) {
        len += col;
        col = 0;
    }
    if (len <= 0) return;
    if (col >= width_) return;
    if (col + len > width_) len = width_ - col;
    for (int i = 0; i < len; i++) {
        Cell& c = next_[(size_t)row * width_ + col + i];
        c.ch = ch;
        c.fg = fg;
        c.bg = bg;
        c.bold = bold;
    }
}

void Buffer::text(int row, int col, const std::wstring& s, uint8_t fg, uint8_t bg,
                  bool bold) {
    if (row < 0 || row >= height_) return;
    size_t i = 0;
    int x = col;
    while (i < s.size() && x < width_) {
        int w = disp_width(s[i]);
        if (x + w > width_) break;  // не влезает целиком — дальше не рисуем
        if (x >= 0) {
            Cell& c = next_[(size_t)row * width_ + x];
            c.ch = s[i];
            c.fg = fg;
            c.bg = bg;
            c.bold = bold;
        }
        i++;
        x += w;
    }
}

void Buffer::clear(uint8_t fg, uint8_t bg) {
    for (size_t i = 0; i < next_.size(); i++) {
        next_[i].ch = L' ';
        next_[i].fg = fg;
        next_[i].bg = bg;
        next_[i].bold = false;
    }
}

void Buffer::flush() {
    std::string out;
    int vrow = -1, vcol = -1;  // виртуальная позиция курсора (0-based)
    int cf = -1, cb = -1;      // атрибуты, выставленные на экране
    bool cbold = false;

    auto move = [&](int r, int c) {
        if (vrow == r && vcol == c) return;
        out += "\x1b[" + std::to_string(r + 1) + ";" + std::to_string(c + 1) + "H";
        vrow = r;
        vcol = c;
    };
    auto attrs = [&](const Cell& n) {
        if (cf == n.fg && cb == n.bg && cbold == n.bold) return;
        std::string a;
        if (n.bold) a += "1;";
        a += "38;5;" + std::to_string(n.fg) + ";48;5;" + std::to_string(n.bg);
        out += "\x1b[" + a + "m";
        cf = n.fg;
        cb = n.bg;
        cbold = n.bold;
    };

    // Широкие символы (CJK) занимают две колонки терминала. Считаем колонки, а
    // не клетки: широкий символ клетки c покрывает колонки c и c+1, поэтому
    // после вывода обе колонки синхронизируются с кадром, а следующая клетка
    // не выводится отдельно. Когда широкий символ заменяется узким, правая
    // половина старого глифа остаётся на экране — принудительно перерисовываем
    // следующую колонку.
    for (int r = 0; r < height_; r++) {
        bool force_next = false;  // надо перерисовать следующую колонку (хвост узкого)
        int c = 0;
        while (c < width_) {
            size_t i = (size_t)r * width_ + c;
            const Cell& n = next_[i];
            const Cell& p = cur_[i];
            int nw = disp_width(n.ch);
            int pw = disp_width(p.ch);
            if (nw == 2) {
                // Широкий символ: совпал с кадром, если и он, и его правая
                // половина (c+1) уже на экране в нужном виде.
                bool same = n.ch == p.ch && n.fg == p.fg && n.bg == p.bg &&
                            n.bold == p.bold && !force_next;
                if (!same) {
                    move(r, c);
                    attrs(n);
                    out += util::w2u(std::wstring(1, n.ch));
                    vcol = c + 2;
                    cur_[i] = n;
                    if (c + 1 < width_) cur_[i + 1] = next_[i + 1];
                }
                force_next = false;
                c += 2;
                continue;
            }
            // Узкий символ.
            bool same = n.ch == p.ch && n.fg == p.fg && n.bg == p.bg &&
                        n.bold == p.bold && !force_next;
            if (!same) {
                move(r, c);
                attrs(n);
                out += util::w2u(std::wstring(1, n.ch));
                vcol = c + 1;
                cur_[i] = n;
            }
            force_next = pw == 2;  // был широкий — правую половину перерисуем
            c += 1;
        }
    }
    out += "\x1b[0m";
    out::text(stdout, out);
}

void cursor(bool show) {
    out::text(stdout, show ? "\x1b[?25h" : "\x1b[?25l");
}

}  // namespace screen