#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace screen {

// Одна клетка экрана: символ + атрибуты (индексы ANSI 256-палитры).
struct Cell {
    wchar_t ch = L' ';
    uint8_t fg = 7;     // цвет текста
    uint8_t bg = 0;     // цвет фона
    bool bold = false;
};

// Offscreen-буфер экрана: кадр компонуется в next_ (cell/fill/text/clear),
// flush() диффит его с предыдущим кадром (cur_) и выводит через VT только
// изменившиеся клетки — без заливки строк пробелами и полных перерисовок.
// Не потокобезопасен: вызывать только из потока отрисовки.
class Buffer {
public:
    // Изменение размера очищает экран: старое содержимое (другого размера)
    // не пересчитывается, проще начать с чистого листа.
    void resize(int width, int height);
    int width() const { return width_; }
    int height() const { return height_; }

    // Клетка компонуемого кадра (за пределами кадра — временная заглушка).
    Cell& cell(int row, int col);
    // Заливка прямоугольной области одним символом и цветом.
    void fill(int row, int col, int len, wchar_t ch, uint8_t fg, uint8_t bg,
              bool bold = false);
    // Текст (широкие символы) с обрезкой по правому краю кадра.
    void text(int row, int col, const std::wstring& s, uint8_t fg, uint8_t bg,
              bool bold = false);
    // Очистка всего кадра (фон/fg).
    void clear(uint8_t fg, uint8_t bg);

    // Выводит изменившиеся клетки VT-последовательностями (один вызов на
    // проход отрисовки) и принимает next_ как текущее состояние экрана.
    void flush();

private:
    int width_ = 0;
    int height_ = 0;
    std::vector<Cell> cur_;   // что реально на экране
    std::vector<Cell> next_;  // компонуемый кадр
};

// Ширина символа в колонках терминала: 1 для обычных, 2 для широких (CJK
// иероглифы, хирагана/катакана, хангыль, полной ширины формы). Несколько
// редких диапазонов East Asian Wide/F == 2; остальное — 1.
int disp_width(wchar_t c);

// Обрезает строку до width колонок терминала (широкие символы учитываются
// как две колонки; если широкий символ не влезает целиком — он отбрасывается).
std::wstring truncate_cols(const std::wstring& s, int width);

// Показать/скрыть курсор.
void cursor(bool show);

}  // namespace screen