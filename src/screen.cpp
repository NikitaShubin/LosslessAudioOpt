#include "screen.h"

#include <string>
#include <vector>

#include "out.h"
#include "util.h"

namespace screen {

namespace {

const Cell kDefault;  // ch=' ', fg=7, bg=0, bold=false

}  // namespace

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
    while (i < s.size() && col < width_) {
        if (col >= 0) {
            Cell& c = next_[(size_t)row * width_ + col];
            c.ch = s[i];
            c.fg = fg;
            c.bg = bg;
            c.bold = bold;
        }
        i++;
        col++;
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

    for (int r = 0; r < height_; r++) {
        for (int c = 0; c < width_; c++) {
            size_t i = (size_t)r * width_ + c;
            const Cell& n = next_[i];
            const Cell& p = cur_[i];
            if (n.ch == p.ch && n.fg == p.fg && n.bg == p.bg && n.bold == p.bold)
                continue;
            move(r, c);
            attrs(n);
            out += util::w2u(std::wstring(1, n.ch));
            vcol++;
            cur_[i] = n;
        }
    }
    out += "\x1b[0m";
    out::text(stdout, out);
}

void cursor(bool show) {
    out::text(stdout, show ? "\x1b[?25h" : "\x1b[?25l");
}

}  // namespace screen