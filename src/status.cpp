#include "status.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "i18n.h"
#include "out.h"
#include "proc.h"
#include "screen.h"
#include "util.h"

namespace status {

namespace {

// ---------------------------------------------------------------------------
// Палитра (индексы ANSI 256-палитры)
// ---------------------------------------------------------------------------
constexpr uint8_t kPendingBg = 236;    // тёмно-серый — задача ждёт
constexpr uint8_t kRunningBg = 208;    // оранжевый — задача выполняется сейчас
constexpr uint8_t kOkBg = 27;          // синий — задача выполнена успешно
constexpr uint8_t kFailedBg = 196;     // красный — ошибка
constexpr uint8_t kSkipBg = 240;       // средне-серый — файл не конвертирован
constexpr uint8_t kDoneBg = 28;        // зелёный — файл обработан успешно (вся строка)
constexpr uint8_t kPreppingBg = 130;   // тёмно-оранжевый — файл распаковывается (вся строка)
constexpr uint8_t kNotStartedBg = 0;   // чёрный — файл ещё не начат
constexpr uint8_t kTextFg = 15;        // ярко-белый текст на полосе
constexpr uint8_t kFooterFg = 252;    // приглушённый белый текст футера
constexpr uint8_t kFooterBg = 235;    // тёмно-серый фон футера (≠ чёрный контента)
constexpr uint8_t kScrollTrackFg = 238; // полоса скроллбара — трек
constexpr uint8_t kScrollThumbFg = 245; // полоса скроллбара — бегунок

// Состояние сегмента полосы (одного варианта).
enum class Seg { Pending, Running, Ok, Failed };

// Целое-состояние строки, заменяющее раскраску сегментов.
enum class Whole { None, Skip, Error };

// Режим полосы: Progress — сегменты упорядочены по состоянию (слева синие
// готовые, затем оранжевый активный, красные ошибки, серые ожидающие), так
// что полоса растёт слева направо; Mosaic — исходная позиционная раскраска.
enum class BarMode { Progress, Mosaic };

// Одна строка статуса = один файл. Во время обработки строка — полоса на всю
// ширину консоли, разбитая на сегменты (по одному на вариант): синий —
// выполнено, оранжевый — выполняется сейчас, красный — ошибка, тёмно-серый —
// ожидание. По завершении строка красится целиком: зелёный — успех, серый —
// skip, красный — ошибка. Во время prep — тёмно-оранжевая, до prep — чёрная.
struct Row {
    std::string label;
    bool has_tasks = false;
    size_t total = 0;
    std::vector<Seg> segs;
    bool done = false;
    Whole whole = Whole::None;
    bool active = false;  // prep или есть Running-задача (для автоследования)
    bool prepping = false;  // файл находится в фазе подготовки (распаковки)
    double win_pct = 0.0;   // процент выигрыша в сжатии готового файла
};

std::mutex g_m;
bool g_init = false;
bool g_interactive = false;
size_t g_total = 0;
std::vector<Row> g_rows;

// Накопленные диагностические строки (под g_m). В интерактивном режиме log/error
// не выводятся на экран сразу: они буферизуются, последняя ошибка показывается
// в футере, а весь накопленный список печатается после shutdown() (когда
// альтернативный буфер уже восстановлен — иначе текст пропадёт).
std::vector<std::string> g_log_lines;
std::vector<std::string> g_error_lines;
constexpr size_t kMaxBufLines = 500;

// Отдельный поток отрисовки: воркеры только меняют состояние и будят его.
std::thread g_render_thread;
std::condition_variable g_cv;
bool g_render_stop = false;
bool g_pending = false;

// Пауза для сбора пачки обновлений: воркеры будят поток отрисовки, он ждёт
// ещё kDebounce, чтобы нарисовать всё разом.
constexpr std::chrono::milliseconds kDebounce(30);
// Период опроса ввода и размера окна в потоке отрисовки: клавиатура/мышь
// обрабатываются этим интервалом, ресайз перерисовывается сразу.
constexpr std::chrono::milliseconds kInputPoll(50);

screen::Buffer g_screen;
int g_width = 80;
int g_height = 25;
bool g_size_forced = false;  // размер задан через LLAO_STATUS_SIZE — не обновлять

// Вьюпорт «прокручиваемого плейлиста»: g_scroll — индекс первой видимой строки.
// Любая ручная прокрутка (клавиатура/мышь) отключает автоследование g_follow.
int g_scroll = 0;
bool g_follow = true;  // следовать за активной полосой

// Горизонтальный сдвиг строк списка (←/→): смещает видимую часть длинных имён
// файлов. Не влияет на автоследование.
int g_hscroll = 0;

// Перетаскивание вертикального скроллбара: g_sb_drag — идёт ли перетаскивание
// (левая кнопка нажата в колонке скроллбара), g_sb_drag_off — смещение точки
// захвата бегунка (когда нажали именно на бегунок, а не на трек).
bool g_sb_drag = false;
int g_sb_drag_off = 0;

// Псевдографический вертикальный скроллбар справа: виден, когда список не
// помещается в окно (g_total > видимых строк). Занимает последнюю колонку;
// клик по нему мышью прыгает в позицию, прокрутка стрелками/колесом работает
// как обычно.
bool scrollbar_visible() {
    return g_height >= 2 && g_total > (size_t)(g_height - 1);
}

// Ширина области контента (список + полосы + футер): без последней колонки,
// когда справа рисуется скроллбар.
int content_width() {
    return scrollbar_visible() ? g_width - 1 : g_width;
}

// Режим полосы: прогрессбар по умолчанию, Tab переключает на мозаику.
BarMode g_mode = BarMode::Progress;

DWORD g_orig_mode = 0;
bool g_orig_mode_valid = false;

// Ввод консоли: дескриптор и исходный режим (восстанавливается в shutdown()).
HANDLE g_input = INVALID_HANDLE_VALUE;
DWORD g_orig_in_mode = 0;
bool g_orig_in_mode_valid = false;

std::wstring u2w(const std::string& s) { return util::u2w(s); }

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

// Длина строки в колонках терминала (широкие символы — 2 колонки).
int str_cols(const std::wstring& s) {
    int n = 0;
    for (wchar_t c : s) n += screen::disp_width(c);
    return n;
}

// Обрезка строки до width колонок (голова остаётся, в конце многоточие).
std::wstring fit(const std::wstring& w, size_t width) {
    if (w.empty() || width == 0) return L"";
    if (str_cols(w) <= (int)width) return w;
    if (width < 3) return screen::truncate_cols(w, (int)width);
    return screen::truncate_cols(w, (int)width - 3) + L"...";
}

// Обрезка строки с учётом горизонтального сдвига hscroll (в колонках). Лист
// ведёт себя как монолитная лента: сдвиг 0 — голова + многоточие в конце (если
// не помещается); сдвиг > 0 — первые 3 колонки окна закрыты маской «...»
// (скрывает начало пути), видимая часть начинается с колонки hscroll+3, а
// справа «...» добавляется, пока хвост всё ещё обрезается.
std::wstring fit_scroll(const std::wstring& w, size_t width, int hscroll) {
    if (w.empty() || width == 0) return L"";
    if (hscroll <= 0) return fit(w, width);
    if (width < 3) return screen::truncate_cols(w, (int)width);
    int start = hscroll + 3;
    int col = 0;
    size_t i = 0;
    while (i < w.size() && col < start) {
        col += screen::disp_width(w[i]);
        i++;
    }
    std::wstring sub = w.substr(i);
    int content_max = (int)width - 3;  // колонки под контент после левой маски
    bool right_mask = (long long)hscroll + (int)width < str_cols(w);
    if (right_mask && content_max >= 6) {
        std::wstring mid = screen::truncate_cols(sub, content_max - 3);
        return L"..." + mid + L"...";
    }
    return L"..." + screen::truncate_cols(sub, content_max);
}

// Высота бегунка вертикального скроллбара (в строках трека track).
int thumb_height(int track) {
    int thumb = (int)((long long)track * track / g_total);
    if (thumb < 1) thumb = 1;
    return thumb;
}

// Смещение верха бегунка по текущей позиции g_scroll (в строках трека track).
int thumb_offset(int track, int thumb) {
    int max_scroll = (int)g_total - track;
    if (max_scroll < 0) max_scroll = 0;
    if (max_scroll <= 0) return 0;
    return (int)((long long)g_scroll * (track - thumb) / max_scroll);
}

// Прыжок вьюпорта в позицию, соответствующую строке трека y. Сдвигается
// g_scroll и отключается автоследование (ручное действие).
void sb_jump(int y, int track) {
    if (y < 0) y = 0;
    int max_scroll = (int)g_total - track;
    if (max_scroll < 0) max_scroll = 0;
    g_scroll = (int)((long long)y * max_scroll / track);
    g_follow = false;
}

uint8_t seg_bg(Seg s) {
    switch (s) {
        case Seg::Running: return kRunningBg;
        case Seg::Ok: return kOkBg;
        case Seg::Failed: return kFailedBg;
        default: return kPendingBg;
    }
}

// Цвет фона колонки col полосы строки r (ширина полосы W).
// В режиме Mosaic сегмент, покрывающий колонку, вычисляется пропорционально:
// задача t занимает колонки [t*W/total, (t+1)*W/total). В режиме Progress
// полоса упорядочена по состоянию: синие (готово) слева, затем оранжевый
// (активная), красные (ошибки), серые (ожидание) — так полоса растёт слева
// направо. Если сегментов больше ширины — часть задач не получит колонки
// (это ок: полоса остаётся читаемой).
uint8_t col_bg(const Row& r, size_t col, size_t W) {
    switch (r.whole) {
        case Whole::Skip: return kSkipBg;
        case Whole::Error: return kFailedBg;
        case Whole::None:
            if (r.done) return kDoneBg;
            if (r.prepping) return kPreppingBg;
            if (!r.has_tasks || r.total == 0) return kNotStartedBg;
            {
                if (g_mode == BarMode::Mosaic) {
                    size_t t = (size_t)((uint64_t)col * r.total / W);
                    if (t >= r.segs.size()) t = r.segs.size() - 1;
                    return seg_bg(r.segs[t]);
                }
                // Progress: упорядоченная полоса.
                size_t n_ok = 0, n_run = 0, n_fail = 0;
                for (Seg s : r.segs) {
                    if (s == Seg::Ok) n_ok++;
                    else if (s == Seg::Running) n_run++;
                    else if (s == Seg::Failed) n_fail++;
                }
                size_t t = (size_t)((uint64_t)col * r.total / W);
                if (t < n_ok) return kOkBg;
                t -= n_ok;
                if (t < n_run) return kRunningBg;
                t -= n_run;
                if (t < n_fail) return kFailedBg;
                return kPendingBg;
            }
    }
    return kPendingBg;
}

// Процент строки (см. определение ниже).
std::wstring row_pct(const Row& r);

// Максимальный горизонтальный сдвиг: правый край самого длинного лейбла должен
// оставаться видимым. Считается так же, как ширина области лейбла в row_label:
// вся ширина минус процент и счётчик. При hscroll > этого значения последний
// символ самого длинного пути уехал бы за правый край — такой сдвиг запрещаем.
int max_hscroll_locked() {
    int W = content_width();
    int best = 0;
    for (size_t i = 0; i < g_total; i++) {
        const Row& r = g_rows[i];
        int left_w = W - str_cols(row_pct(r));
        if (left_w <= 0) left_w = W;
        int label_w = left_w - (int)str_cols(u2w(counter(i)) + L" ");
        if (label_w <= 0) continue;
        int need = str_cols(u2w(r.label)) - label_w;
        if (need > best) best = need;
    }
    return best;
}

// Текст строки: счётчик + имя файла (с учётом горизонтального сдвига). Процент
// формируется отдельно (row_pct) и выравнивается по правому краю в paint_row.
std::wstring row_label(const Row& r, size_t idx) {
    std::wstring s = u2w(counter(idx)) + L" ";
    int W = content_width();
    std::wstring pct = row_pct(r);
    int left_w = W - str_cols(pct);
    if (left_w <= 0) left_w = W;
    int label_w = left_w - str_cols(s);
    if (label_w > 0) s += fit_scroll(u2w(r.label), (size_t)label_w, g_hscroll);
    else s = fit(s, (size_t)left_w);
    return s;
}

// Процент строки (без ведущего пробела): ход выполнения во время обработки,
// у готового файла — выигрыш в сжатии; пусто для не-начатых/skip/error.
std::wstring row_pct(const Row& r) {
    if (r.done && r.whole == Whole::None) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%5.1f%%", r.win_pct);
        return u2w(buf);
    }
    if (r.has_tasks && !r.done && r.total > 0) {
        size_t finished = 0;
        for (Seg sg : r.segs)
            if (sg == Seg::Ok || sg == Seg::Failed) finished++;
        char buf[32];
        snprintf(buf, sizeof(buf), "%5.1f%%", 100.0 * (double)finished / (double)r.total);
        return u2w(buf);
    }
    return L"";
}

// Пишет текст в клетки строки y, начиная с колонки x, с учётом ширины символов
// (широкие занимают две колонки). max_cols — максимальная ширина в колонках.
// Возвращает число занятых колонок. Фон каждой колонки — как у полосы (col_bg).
int put_text_cols(int y, int x, const std::wstring& s, int max_cols, const Row& r, int W) {
    int col = x;
    for (wchar_t ch : s) {
        int w = screen::disp_width(ch);
        if (col + w > x + max_cols) break;
        if (col >= g_width) break;
        g_screen.cell(y, col) = {ch, kTextFg, col_bg(r, (size_t)col, (size_t)W), true};
        col += w;
    }
    return col - x;
}

// Рисует строку файла idx на экранной строке y. Процент — строго у правого края.
void paint_row(size_t idx, int y) {
    const Row& r = g_rows[idx];
    int W = content_width();
    for (int c = 0; c < W; c++) {
        screen::Cell cell = {L' ', kTextFg, col_bg(r, (size_t)c, (size_t)W), true};
        g_screen.cell(y, c) = cell;
    }
    std::wstring label = row_label(r, idx);
    std::wstring pct = row_pct(r);
    int pct_w = str_cols(pct);
    int left_w = W - pct_w;
    if (left_w <= 0) {
        left_w = W;
        pct_w = 0;
    }
    int lw = str_cols(label);
    if (lw > left_w) lw = left_w;
    put_text_cols(y, 0, label, lw, r, W);
    if (pct_w > 0) {
        int x0 = W - pct_w;
        put_text_cols(y, x0, pct, pct_w, r, W);
    }
}

// Подгоняет вьюпорт так, чтобы активная полоса (файлы с prep/Running) была
// видима целиком, если это возможно.
void follow_active(int vis) {
    size_t lo = g_total, hi = 0;
    for (size_t i = 0; i < g_total; i++) {
        if (!g_rows[i].active) continue;
        if (i < lo) lo = i;
        if (i > hi) hi = i;
    }
    if (hi < lo) return;  // активных нет — позицию не трогаем
    int max_scroll = (int)g_total - vis;
    if (max_scroll < 0) max_scroll = 0;
    if ((int)hi >= g_scroll + vis) g_scroll = (int)hi - vis + 1;
    if ((int)lo < g_scroll) g_scroll = (int)lo;
    if (g_scroll < 0) g_scroll = 0;
    if (g_scroll > max_scroll) g_scroll = max_scroll;
}

// Компоновка всего кадра: видимые строки списка + футер + (скроллбар справа).
void compose_frame() {
    g_screen.clear(kFooterFg, kFooterBg);
    int vis = g_height - 1;  // последняя строка — футер
    if (vis > (int)g_total) vis = (int)g_total;
    if (vis < 0) vis = 0;
    int W = content_width();

    if (g_follow) follow_active(vis);

    for (int y = 0; y < vis; y++) {
        size_t idx = (size_t)g_scroll + y;
        if (idx < g_total) paint_row(idx, y);
    }

    // Вертикальный скроллбар в последней колонке: трек — на видимые строки,
    // бегунок — доля видимой части списка. Клик по нему обрабатывается
    // в handle_input_locked (прыжок на позицию курсора).
    if (scrollbar_visible() && vis > 0) {
        int track = vis;
        int thumb = thumb_height(track);
        int thumb_y = thumb_offset(track, thumb);
        for (int y = 0; y < track; y++) {
            bool in = y >= thumb_y && y < thumb_y + thumb;
            g_screen.cell(y, g_width - 1) = {in ? L'█' : L'│',
                                             in ? (uint8_t)kScrollThumbFg : (uint8_t)kScrollTrackFg,
                                             kFooterBg, false};
        }
    }

    size_t lo = (size_t)g_scroll + 1;
    size_t hi = (size_t)g_scroll + vis;
    if (hi > g_total) hi = g_total;
    std::string foot = i18n::fmt("files %zu-%zu / %zu", lo, hi, g_total);
    if (g_follow) {
        foot += " · " + i18n::str("follow");
    } else {
        foot += " · " + i18n::str("manual");
        // Подсказка по навигации полезна только если список не помещается в окно.
        if (g_total > (size_t)vis && vis > 0)
            foot += " · " + i18n::str("arrow keys scroll, F — follow");
    }
    std::wstring wf;
    if (proc::cancelled()) {
        // Ctrl+C: сразу показываем, что идёт аккуратная остановка (иначе
        // кажется, что интерфейс завис, пока воркеры добивают процессы).
        wf = L"!" + screen::truncate_cols(
                        u2w(i18n::str("Ctrl+C: graceful shutdown in progress, "
                                      "finishing current tasks... (press again for force exit)")),
                        W - 1);
    } else if (!g_error_lines.empty()) {
        // При ошибках в футере показываем последнюю причину (иначе в интерактивном
        // режиме не видно, что пошло не так) — красным текстом на чёрном фоне.
        wf = L"!" + screen::truncate_cols(u2w(g_error_lines.back()), W - 1);
    } else {
        wf = screen::truncate_cols(u2w(foot), W);
    }
    int col = 0;
    for (wchar_t ch : wf) {
        if (col >= W) break;
        bool is_err = proc::cancelled() || !g_error_lines.empty();
        g_screen.cell(g_height - 1, col) = {ch, is_err ? (uint8_t)kFailedBg : kFooterFg,
                                            kFooterBg, is_err};
        col += screen::disp_width(ch);
    }
}

// Обновляет размер окна консоли. Возвращает true, если размер изменился.
// Размер, заданный через LLAO_STATUS_SIZE, не обновляется.
bool refresh_size_locked() {
    if (g_size_forced) return false;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (h == INVALID_HANDLE_VALUE || h == nullptr ||
        !GetConsoleScreenBufferInfo(h, &csbi))
        return false;
    int w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    int hh = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    if (w < 1) w = 1;
    if (hh < 1) hh = 1;
    if (w == g_width && hh == g_height) return false;
    g_width = w;
    g_height = hh;
    return true;
}

// Один проход отрисовки (только из потока отрисовки, под g_m).
void render_pass_locked() {
    refresh_size_locked();
    g_screen.resize(g_width, g_height);
    // После ресайза окна предел сдвига мог измениться — приводим вьюпорт в норму.
    if (g_hscroll > max_hscroll_locked()) g_hscroll = max_hscroll_locked();
    compose_frame();
    g_screen.flush();
}

// Обрабатывает накопленный ввод консоли (клавиатура/мышь/ресайз). Возвращает
// true, если вьюпорт или режим следования изменились (нужна перерисовка).
// Вызывается только из потока отрисовки под g_m; блокировки нет — читаются
// только уже накопленные события.
bool handle_input_locked() {
    if (g_input == INVALID_HANDLE_VALUE || g_input == nullptr) return false;
    DWORD n = 0;
    if (!GetNumberOfConsoleInputEvents(g_input, &n) || n == 0) return false;
    const int vis = g_height > 1 ? g_height - 1 : 0;
    int max_scroll = (int)g_total - vis;
    if (max_scroll < 0) max_scroll = 0;
    auto clamp_scroll = [&](int& s) {
        if (s < 0) s = 0;
        if (s > max_scroll) s = max_scroll;
    };
    bool changed = false;
    while (n > 0) {
        INPUT_RECORD rec;
        DWORD rd = 0;
        if (!ReadConsoleInput(g_input, &rec, 1, &rd) || rd == 0) break;
        n--;
        if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown) {
            switch (rec.Event.KeyEvent.wVirtualKeyCode) {
                case VK_UP: g_scroll--; g_follow = false; break;
                case VK_DOWN: g_scroll++; g_follow = false; break;
                case VK_PRIOR: g_scroll -= vis; g_follow = false; break;
                case VK_NEXT: g_scroll += vis; g_follow = false; break;
                case VK_HOME: g_scroll = 0; g_follow = false; break;
                case VK_END: g_scroll = max_scroll; g_follow = false; break;
                case VK_LEFT: g_hscroll--; break;
                case VK_RIGHT: g_hscroll++; break;
                case VK_SPACE:
                case 'F': g_follow = !g_follow; break;
                case VK_TAB:
                    g_mode = g_mode == BarMode::Progress ? BarMode::Mosaic : BarMode::Progress;
                    break;
                default: continue;
            }
            changed = true;
        } else if (rec.EventType == MOUSE_EVENT &&
                   (rec.Event.MouseEvent.dwEventFlags & MOUSE_WHEELED)) {
            short delta = (short)HIWORD(rec.Event.MouseEvent.dwButtonState);
            g_scroll += delta > 0 ? -3 : 3;
            g_follow = false;
            changed = true;
        } else if (rec.EventType == MOUSE_EVENT && rec.Event.MouseEvent.dwEventFlags == 0) {
            // Нажатие/отпускание кнопки. По колонке скроллбара: нажатие на сам
            // бегунок — захват с сохранением смещения от его верха (затем движение
            // перетаскивает бегунок), клик по треку — прыжок в позицию; отпускание
            // завершает перетаскивание.
            const auto& me = rec.Event.MouseEvent;
            bool pressed = (me.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;
            if (!pressed) {
                g_sb_drag = false;
                g_sb_drag_off = 0;
                continue;
            }
            if (!scrollbar_visible() || me.dwMousePosition.X != g_width - 1 || vis <= 0)
                continue;
            int track = vis;
            int y = me.dwMousePosition.Y;
            if (y < 0 || y >= track) continue;
            int thumb = thumb_height(track);
            int thumb_y = thumb_offset(track, thumb);
            g_sb_drag = true;
            if (y >= thumb_y && y < thumb_y + thumb) g_sb_drag_off = y - thumb_y;
            else g_sb_drag_off = 0;
            // Верх бегунка встаёт на точку захвата (для прыжка по треку off=0).
            sb_jump(y - g_sb_drag_off, track);
            changed = true;
        } else if (rec.EventType == MOUSE_EVENT &&
                   (rec.Event.MouseEvent.dwEventFlags & MOUSE_MOVED)) {
            // Перетаскивание скроллбара: курсор движется с зажатой кнопкой —
            // бегунок следует, сохраняя точку захвата.
            const auto& me = rec.Event.MouseEvent;
            if (g_sb_drag && (me.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) && vis > 0) {
                int track = vis;
                int y = me.dwMousePosition.Y - g_sb_drag_off;
                if (y >= 0 && y < track) {
                    sb_jump(y, track);
                    changed = true;
                }
            }
        } else if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            changed = true;  // новый размер подхватит refresh_size_locked()
        }
    }
    clamp_scroll(g_scroll);
    if (g_hscroll < 0) g_hscroll = 0;
    int max_h = max_hscroll_locked();
    if (g_hscroll > max_h) g_hscroll = max_h;
    return changed;
}

// Поток отрисовки. Спящий до появления работы; после пробуждения ждёт ещё
// kDebounce, чтобы собрать пачку обновлений в одну отрисовку. Параллельно
// с ожиданием каждые kInputPoll опрашивает ввод и размер окна.
void render_loop() {
    // Поток ввода/отрисовки — выше приоритетом, чем воркеры и кодеки,
    // чтобы навигация клавишами/мышью не тормозила под нагрузкой на CPU.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    std::unique_lock<std::mutex> lk(g_m);
    while (true) {
        g_cv.wait_for(lk, kInputPoll, [&] { return g_render_stop || g_pending; });
        if (g_render_stop) break;
        if (handle_input_locked()) g_pending = true;
        if (!g_pending && refresh_size_locked()) g_pending = true;
        if (!g_pending) continue;
        g_cv.wait_for(lk, kDebounce);  // собрать пачку
        if (g_render_stop) break;
        render_pass_locked();
        g_pending = false;
    }
    // Финальный сброс накопленного перед восстановлением буфера экрана.
    render_pass_locked();
}

void wake() {
    g_pending = true;
    g_cv.notify_all();
}

}  // namespace

void init(size_t total_files, bool no_status) {
    std::lock_guard<std::mutex> lk(g_m);
    g_total = total_files;
    g_rows.assign(total_files, Row{});
    g_init = true;
    g_interactive = false;
    g_scroll = 0;
    g_follow = true;
    g_hscroll = 0;
    g_sb_drag = false;
    g_sb_drag_off = 0;
    g_mode = BarMode::Progress;
    g_width = 80;
    g_height = 25;
    g_size_forced = false;
    g_input = INVALID_HANDLE_VALUE;
    g_orig_in_mode_valid = false;
    if (no_status) return;

    const char* force = getenv("LLAO_STATUS_FORCE");
    bool forced = force && force[0] == '1';
    const char* sz = getenv("LLAO_STATUS_SIZE");
    if (sz && *sz) {
        int W = 0, H = 0;
        if (sscanf(sz, "%dx%d", &W, &H) == 2 && W > 0 && H > 0) {
            g_width = W;
            g_height = H;
            g_size_forced = true;
        }
    }

    if (!forced) {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (h != INVALID_HANDLE_VALUE && h != nullptr) {
            DWORD mode = 0;
            if (GetConsoleMode(h, &mode)) {
                // Настоящая Windows-консоль — включаем VT-обработку.
                DWORD new_mode = mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
                SetConsoleMode(h, new_mode);
                g_orig_mode = mode;
                g_orig_mode_valid = true;
            }
            // GetConsoleMode не удался (Wine / pipe / файл): продолжаем
            // без SetConsoleMode — ANSI-коды и так работают через fwrite()
            // в любом xterm-совместимом терминале.
        }
    }

    // Размер окна: консоль -> переменные окружения COLUMNS/LINES -> 80x25.
    if (!g_size_forced) {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        bool ok = h != INVALID_HANDLE_VALUE && h != nullptr &&
                  GetConsoleScreenBufferInfo(h, &csbi);
        if (ok) {
            int w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
            int hh = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
            if (w > 0 && hh > 0) {
                g_width = w;
                g_height = hh;
            }
        } else {
            const char* cols = getenv("COLUMNS");
            const char* lines = getenv("LINES");
            if (cols && *cols && lines && *lines) {
                int w = atoi(cols), hh = atoi(lines);
                if (w > 0 && hh > 0) {
                    g_width = w;
                    g_height = hh;
                }
            }
        }
    }

    g_interactive = true;

    // Ввод: стрелки/PgUp/PgDn/Home/End — прокрутка, пробел/F — переключение
    // автоследования, колесо мыши — прокрутка. Включаем обработку событий окна
    // и мыши, отключаем QuickEdit (выделение мышью «зависает» консоль) и эхо
    // ввода (иначе набранные символы печатались бы поверх статусбара). Если
    // ввода нет (stdin не консоль, например pipe под wine) — вьюпорт остаётся
    // без прокрутки, это не ошибка.
    g_input = GetStdHandle(STD_INPUT_HANDLE);
    if (g_input != INVALID_HANDLE_VALUE && g_input != nullptr) {
        DWORD imode = 0;
        if (GetConsoleMode(g_input, &imode)) {
            g_orig_in_mode = imode;
            g_orig_in_mode_valid = true;
            DWORD new_mode = imode | ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT |
                             ENABLE_MOUSE_INPUT;
            new_mode &= ~(ENABLE_QUICK_EDIT_MODE | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
            SetConsoleMode(g_input, new_mode);
        }
    }

    // Весь псевдографический интерфейс живёт в альтернативном буфере экрана:
    // исходный экран (с командной строкой и прежним выводом) сохраняется
    // и восстанавливается в shutdown() — как у mc/opencode.
    out::text(stdout, "\x1b[?1049h");
    screen::cursor(false);
    g_screen.resize(g_width, g_height);

    // Вся отрисовка — в отдельном потоке; воркеры его только будят.
    g_render_stop = false;
    g_pending = false;
    g_render_thread = std::thread(render_loop);
}

bool interactive() { return g_interactive; }

void begin_file(size_t idx, const std::string& label) {
    if (!g_interactive) return;
    std::lock_guard<std::mutex> lk(g_m);
    if (idx >= g_total) return;
    Row& r = g_rows[idx];
    r.label = label;
    r.has_tasks = false;
    r.total = 0;
    r.segs.clear();
    r.done = false;
    r.whole = Whole::None;
    r.active = false;
    r.prepping = false;
    r.win_pct = 0.0;
    wake();
}

void prep(size_t idx) {
    if (!g_interactive) return;
    std::lock_guard<std::mutex> lk(g_m);
    if (idx >= g_total) return;
    g_rows[idx].active = true;
    g_rows[idx].prepping = true;
    wake();
}

void set_tasks(size_t idx, size_t total) {
    if (!g_interactive) return;
    std::lock_guard<std::mutex> lk(g_m);
    if (idx >= g_total) return;
    Row& r = g_rows[idx];
    r.total = total;
    r.has_tasks = true;
    r.prepping = false;
    r.segs.assign(total, Seg::Pending);
    wake();
}

void task(size_t idx, size_t task_idx, TaskState st) {
    if (!g_interactive) return;
    std::lock_guard<std::mutex> lk(g_m);
    if (idx >= g_total) return;
    Row& r = g_rows[idx];
    if (!r.has_tasks || task_idx >= r.segs.size()) return;
    Seg s = st == TaskState::Ok ? Seg::Ok
            : st == TaskState::Failed ? Seg::Failed
                                      : Seg::Running;
    r.segs[task_idx] = s;
    if (s == Seg::Running) r.active = true;
    wake();
}

void end_file(size_t idx, double pct) {
    if (!g_interactive) return;
    std::lock_guard<std::mutex> lk(g_m);
    if (idx >= g_total) return;
    g_rows[idx].done = true;
    g_rows[idx].active = false;
    g_rows[idx].prepping = false;
    g_rows[idx].win_pct = pct;
    wake();
}

void mark_skip(size_t idx) {
    if (!g_interactive) return;
    std::lock_guard<std::mutex> lk(g_m);
    if (idx >= g_total) return;
    Row& r = g_rows[idx];
    r.whole = Whole::Skip;
    r.done = true;
    r.active = false;
    wake();
}

void mark_error(size_t idx) {
    if (!g_interactive) return;
    std::lock_guard<std::mutex> lk(g_m);
    if (idx >= g_total) return;
    Row& r = g_rows[idx];
    r.whole = Whole::Error;
    r.done = true;
    r.active = false;
    wake();
}

void log(const std::string& line) {
    if (!g_interactive) {
        out::text(stdout, line);
        return;
    }
    std::lock_guard<std::mutex> lk(g_m);
    if (g_log_lines.size() < kMaxBufLines) g_log_lines.push_back(line);
}

void error(const std::string& line) {
    if (!g_interactive) {
        out::text(stderr, line);
        return;
    }
    std::lock_guard<std::mutex> lk(g_m);
    if (g_error_lines.size() < kMaxBufLines) g_error_lines.push_back(line);
    wake();
}

void shutdown() {
    std::unique_lock<std::mutex> lk(g_m);
    if (!g_interactive) return;
    g_render_stop = true;
    g_cv.notify_all();
    lk.unlock();
    // Ждём, пока поток отрисовки сбросит накопленные изменения и завершится.
    if (g_render_thread.joinable()) g_render_thread.join();
    lk.lock();
    screen::cursor(true);
    out::text(stdout, "\x1b[0m\x1b[r\x1b[2J\x1b[?1049l");
    if (g_orig_mode_valid) {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (h != INVALID_HANDLE_VALUE && h != nullptr) SetConsoleMode(h, g_orig_mode);
    }
    if (g_orig_in_mode_valid && g_input != INVALID_HANDLE_VALUE && g_input != nullptr) {
        SetConsoleMode(g_input, g_orig_in_mode);
    }
    g_interactive = false;
    g_scroll = 0;
    g_follow = true;
    g_hscroll = 0;
    g_sb_drag = false;
    g_sb_drag_off = 0;
    g_mode = BarMode::Progress;
    g_orig_mode_valid = false;
    g_orig_in_mode_valid = false;

    // Накопленные диагностические строки печатаем после восстановления
    // альтернативного буфера (см. kMaxBufLines): в интерактивном режиме они
    // подавляются, иначе причины ошибок были бы невидимы.
    for (const auto& l : g_error_lines) out::text(stderr, l);
    for (const auto& l : g_log_lines) out::text(stdout, l);
    g_error_lines.clear();
    g_log_lines.clear();
}

}  // namespace status