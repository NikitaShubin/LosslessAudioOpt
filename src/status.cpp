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
#include "screen.h"
#include "util.h"

namespace status {

namespace {

// ---------------------------------------------------------------------------
// Палитра (индексы ANSI 256-палитры)
// ---------------------------------------------------------------------------
constexpr uint8_t kPendingBg = 236;  // тёмно-серый — задача ждёт
constexpr uint8_t kRunningBg = 44;   // голубой — задача выполняется сейчас
constexpr uint8_t kOkBg = 28;        // зелёный — выполнено успешно
constexpr uint8_t kFailedBg = 196;   // красный — ошибка
constexpr uint8_t kSkipBg = 240;     // средне-серый — файл не конвертирован
constexpr uint8_t kTextFg = 15;      // ярко-белый текст на полосе
constexpr uint8_t kFooterFg = 7;     // обычный белый текст футера
constexpr uint8_t kFooterBg = 0;

// Состояние сегмента полосы (одного варианта).
enum class Seg { Pending, Running, Ok, Failed };

// Целое-состояние строки, заменяющее раскраску сегментов.
enum class Whole { None, Skip, Error };

// Одна строка статуса = один файл. Во время обработки строка — полоса на всю
// ширину консоли, разбитая на сегменты (по одному на вариант): зелёный —
// выполнено, голубой — выполняется сейчас, красный — ошибка, тёмно-серый —
// ожидание. По завершении полоса сохраняет финальные цвета; для skip/error
// вся полоса красится целиком.
struct Row {
    std::string label;
    bool has_tasks = false;
    size_t total = 0;
    std::vector<Seg> segs;
    bool done = false;
    Whole whole = Whole::None;
    bool active = false;  // prep или есть Running-задача (для автоследования)
};

std::mutex g_m;
bool g_init = false;
bool g_interactive = false;
size_t g_total = 0;
std::vector<Row> g_rows;

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

// Обрезка строки до width символов (голова остаётся, в конце многоточие).
std::wstring fit(const std::wstring& w, size_t width) {
    if (w.size() <= width) return w;
    if (width >= 3) return w.substr(0, width - 3) + L"...";
    return w.substr(0, width);
}

uint8_t seg_bg(Seg s) {
    switch (s) {
        case Seg::Running: return kRunningBg;
        case Seg::Ok: return kOkBg;
        case Seg::Failed: return kFailedBg;
        default: return kPendingBg;
    }
}

// Цвет фона колонки col полосы строки r (ширина полосы W). Сегмент,
// покрывающий колонку, вычисляется пропорционально: задача t занимает
// колонки [t*W/total, (t+1)*W/total). Если сегментов больше ширины — часть
// задач не получит колонки (это ок: полоса остаётся читаемой).
uint8_t col_bg(const Row& r, size_t col, size_t W) {
    switch (r.whole) {
        case Whole::Skip: return kSkipBg;
        case Whole::Error: return kFailedBg;
        case Whole::None:
            if (!r.has_tasks || r.total == 0) return kPendingBg;
            {
                size_t t = (size_t)((uint64_t)col * r.total / W);
                if (t >= r.segs.size()) t = r.segs.size() - 1;
                return seg_bg(r.segs[t]);
            }
    }
    return kPendingBg;
}

// Текст строки: счётчик, имя файла и процент выполнения во время обработки.
std::wstring row_text(const Row& r, size_t idx) {
    std::string s = counter(idx) + " " + r.label;
    if (r.has_tasks && !r.done && r.total > 0) {
        size_t finished = 0;
        for (Seg sg : r.segs)
            if (sg == Seg::Ok || sg == Seg::Failed) finished++;
        char buf[32];
        snprintf(buf, sizeof(buf), "  %5.1f%%", 100.0 * (double)finished / (double)r.total);
        s += buf;
    }
    return u2w(s);
}

// Рисует строку файла idx на экранной строке y.
void paint_row(size_t idx, int y) {
    const Row& r = g_rows[idx];
    int W = g_width;
    for (int c = 0; c < W; c++) {
        screen::Cell cell = {L' ', kTextFg, col_bg(r, (size_t)c, (size_t)W), true};
        g_screen.cell(y, c) = cell;
    }
    std::wstring txt = fit(row_text(r, idx), (size_t)W);
    for (int c = 0; c < (int)txt.size() && c < W; c++) {
        g_screen.cell(y, c) = {txt[(size_t)c], kTextFg, col_bg(r, (size_t)c, (size_t)W), true};
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

// Компоновка всего кадра: видимые строки списка + футер.
void compose_frame() {
    g_screen.clear(kFooterFg, kFooterBg);
    int vis = g_height - 1;  // последняя строка — футер
    if (vis > (int)g_total) vis = (int)g_total;
    if (vis < 0) vis = 0;

    if (g_follow) follow_active(vis);

    for (int y = 0; y < vis; y++) {
        size_t idx = (size_t)g_scroll + y;
        if (idx < g_total) paint_row(idx, y);
    }

    size_t lo = (size_t)g_scroll + 1;
    size_t hi = (size_t)g_scroll + vis;
    if (hi > g_total) hi = g_total;
    std::string foot = i18n::fmt("files %zu-%zu / %zu", lo, hi, g_total);
    if (g_follow) {
        foot += " · " + i18n::str("follow");
    } else {
        foot += " · " + i18n::str("manual") + " · " + i18n::str("arrow keys scroll, F — follow");
    }
    std::wstring wf = u2w(foot);
    for (int c = 0; c < (int)wf.size() && c < g_width; c++)
        g_screen.cell(g_height - 1, c) = {wf[(size_t)c], kFooterFg, kFooterBg, false};
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
                case VK_SPACE:
                case 'F': g_follow = !g_follow; break;
                default: continue;
            }
            changed = true;
        } else if (rec.EventType == MOUSE_EVENT &&
                   (rec.Event.MouseEvent.dwEventFlags & MOUSE_WHEELED)) {
            short delta = (short)HIWORD(rec.Event.MouseEvent.dwButtonState);
            g_scroll += delta > 0 ? -3 : 3;
            g_follow = false;
            changed = true;
        } else if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            changed = true;  // новый размер подхватит refresh_size_locked()
        }
    }
    clamp_scroll(g_scroll);
    return changed;
}

// Поток отрисовки. Спящий до появления работы; после пробуждения ждёт ещё
// kDebounce, чтобы собрать пачку обновлений в одну отрисовку. Параллельно
// с ожиданием каждые kInputPoll опрашивает ввод и размер окна.
void render_loop() {
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
        if (h == INVALID_HANDLE_VALUE || h == nullptr) return;
        DWORD mode = 0;
        if (!GetConsoleMode(h, &mode)) return;  // stdout не консоль (pipe/файл)
        DWORD new_mode = mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        if (!SetConsoleMode(h, new_mode)) return;  // VT не поддерживается
        g_orig_mode = mode;
        g_orig_mode_valid = true;
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
    wake();
}

void prep(size_t idx) {
    if (!g_interactive) return;
    std::lock_guard<std::mutex> lk(g_m);
    if (idx >= g_total) return;
    g_rows[idx].active = true;
    wake();
}

void set_tasks(size_t idx, size_t total) {
    if (!g_interactive) return;
    std::lock_guard<std::mutex> lk(g_m);
    if (idx >= g_total) return;
    Row& r = g_rows[idx];
    r.total = total;
    r.has_tasks = true;
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

void end_file(size_t idx) {
    if (!g_interactive) return;
    std::lock_guard<std::mutex> lk(g_m);
    if (idx >= g_total) return;
    g_rows[idx].done = true;
    g_rows[idx].active = false;
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
    // Интерактивный режим: на экране только полосы, строки подавляются.
}

void error(const std::string& line) {
    if (!g_interactive) {
        out::text(stderr, line);
        return;
    }
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
    g_orig_mode_valid = false;
    g_orig_in_mode_valid = false;
}

}  // namespace status