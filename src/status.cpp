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
bool g_overflow_handled = false;  // сброс переполнения уже выполнен (см. render_pass)
DWORD g_orig_mode = 0;
bool g_orig_mode_valid = false;

// Отдельный поток отрисовки: воркеры только меняют состояние и будят его.
// Раньше каждый begin_file вызывал redraw_all_locked() синхронно в воркере,
// и стартовый рывок N файлов превращался в N*(N+1)/2 перерисовок блока,
// блокируя обработку на десятки секунд.
std::thread g_render_thread;
std::condition_variable g_cv;
bool g_render_stop = false;
bool g_pending = false;            // есть накопленная работа для потока отрисовки
std::vector<bool> g_dirty;         // какие строки нужно перерисовать
std::vector<size_t> g_pending_ensure;  // файлы, которые нужно ввести в блок
std::vector<std::string> g_pending_log;  // строки лога, ожидающие вывода
bool g_block_grew = false;         // блок вырос — нужна полная перерисовка

int g_width = 80;
int g_height = 25;
bool g_size_valid = false;
bool g_size_forced = false;  // размер задан через LLAO_STATUS_SIZE — не обновлять

// Позиция последнего отрисованного блока статуса: при перерисовке (resize,
// рост блока) старая область стирается целиком, иначе на экране остаются
// копии старого блока.
int g_drawn_top = 0;
size_t g_drawn_rows = 0;

// Пауза для сбора пачки обновлений: воркеры будят поток отрисовки, он ждёт
// ещё kDebounce, чтобы нарисовать всё разом (а не по одному тику на файл).
constexpr std::chrono::milliseconds kDebounce(50);

// Период опроса размера окна. Ресайз замечается и без обновлений от воркеров:
// иначе, пока нет тиков прогресса (идёт долгий вариант), изменение размера
// окна не перерисовывало бы статусбар до следующего обновления.
constexpr std::chrono::milliseconds kResizePoll(200);

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

// Обрезка строки с сохранением хвоста: при превышении ширины голова заменяется
// на "…", а важная часть (причина ошибки, результат) остаётся видимой.
std::string truncate_tail(const std::wstring& w, size_t width) {
    if (w.size() <= width) return w2u(w);
    if (width >= 2) return w2u(L"…" + w.substr(w.size() - (width - 1)));
    return w2u(w.substr(w.size() - width));
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
// не продвигается и текст склеивается без пробелов. Строка предварительно
// стирается (\x1b[2K), чтобы не наслаиваться на лог/старую полосу.
std::string strip_row(const Row& r, size_t idx, int W) {
    double f = r.total > 0 ? (double)r.done / (double)r.total : 0.0;
    int filled = (int)(f * W + 0.5);
    if (filled > W) filled = W;
    if (filled < 0) filled = 0;
    std::wstring body = u2w(row_text(r, idx, W));
    std::string s;
    s += "\x1b[2K";
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

// Обновляет размер окна консоли. Возвращает true, если размер изменился
// (или изменилось состояние overflow). Размер, заданный через LLAO_STATUS_SIZE,
// не обновляется.
bool refresh_size_locked() {
    if (g_size_forced) return false;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (h == INVALID_HANDLE_VALUE || h == nullptr ||
        !GetConsoleScreenBufferInfo(h, &csbi))
        return false;
    int w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    int hh = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    if (w == g_width && hh == g_height) return false;
    g_width = w;
    g_height = hh;
    if ((int)g_visible >= g_height - 1) {
        g_overflow = true;
        g_overflow_handled = false;
    } else {
        if (g_overflow) {
            // Окно снова выросло и блок помещается — возвращаем статусбар
            // (иначе после переполнения он бы не появлялся даже при развороте).
            g_overflow = false;
        }
        g_block_grew = true;  // новые размеры — все строки пересчитать
    }
    return true;
}

// Стирает строки, в которых раньше находился блок статуса: объединение старой
// области (g_drawn_top..g_drawn_rows) и новой (top..top+g_visible). Без этого
// при resize и при росте блока старые строки остаются на экране «копиями».
void erase_block_area_locked(int top) {
    int lo = top;
    int hi = top + (int)g_visible;
    if (g_drawn_rows > 0) {
        lo = std::min(lo, g_drawn_top);
        hi = std::max(hi, g_drawn_top + (int)g_drawn_rows);
    }
    if (lo < 1) lo = 1;
    if (hi > g_height + 1) hi = g_height + 1;
    for (int r = lo; r < hi; r++) out::text(stdout, pos(r, 1) + "\x1b[2K");
}

// Полная перерисовка блока статусных строк и курсор в зону лога.
// Вызывается при resize окна и при изменении состава/высоты блока.
void redraw_all_locked() {
    if (!g_size_valid || g_overflow) return;
    set_region();
    int top = g_height - (int)g_visible + 1;
    erase_block_area_locked(top);
    for (size_t i = 0; i < g_visible; i++) {
        const Row& r = g_rows[i];
        std::string line = pos(top + (int)i, 1);
        line += r.finished ? finished_row(r, i, g_width) : strip_row(r, i, g_width);
        out::text(stdout, line);
    }
    out::text(stdout, pos(scroll_bottom(), 1));
    g_drawn_top = top;
    g_drawn_rows = g_visible;
}

// Перерисовка одной строки блока (обычный тик прогресса) + курсор в зону лога.
void draw_row_locked(size_t idx) {
    if (!g_size_valid || g_overflow) return;
    if (idx >= g_visible) return;
    const Row& r = g_rows[idx];
    int top = g_height - (int)g_visible + 1;
    std::string line = pos(top + (int)idx, 1);
    line += r.finished ? finished_row(r, idx, g_width) : strip_row(r, idx, g_width);
    line += pos(scroll_bottom(), 1);
    out::text(stdout, line);
}

// Вывод накопленных строк лога в зону прокрутки над блоком.
void log_line(const std::string& text);   // определена ниже
void ensure_visible_locked(size_t idx);   // определена ниже

void flush_log_locked() {
    for (auto& line : g_pending_log) log_line(line);
    g_pending_log.clear();
}

// Один проход отрисовки (вызывается только из потока отрисовки под g_m):
// применяет накопленные изменения минимальным числом операций записи.
void render_pass_locked() {
    // Ресайз окна обрабатывается только здесь: GetConsoleScreenBufferInfo
    // не вызывается в воркерах.
    refresh_size_locked();

    if (!g_size_valid) {
        for (auto& line : g_pending_log) out::text(stdout, line);
        g_pending_log.clear();
        g_pending_ensure.clear();
        std::fill(g_dirty.begin(), g_dirty.end(), false);
        g_block_grew = false;
        return;
    }

    // Новые файлы должны попасть в блок (строки добавляются к низу).
    if (!g_overflow) {
        for (size_t idx : g_pending_ensure) {
            ensure_visible_locked(idx);
            if (g_overflow) break;
        }
    }

    if (g_overflow) {
        // Блок не помещается — обычный построчный вывод.
        if (!g_overflow_handled) {
            // Сброс scroll-региона на весь экран (иначе \x1b[2J очистит
            // только старый регион) и полная очистка.
            out::text(stdout, "\x1b[0m" + pos(1, 1));
            out::text(stdout, "\x1b[r");
            out::text(stdout, "\x1b[2J" + pos(1, 1));
            g_drawn_top = 0;
            g_drawn_rows = 0;
            g_overflow_handled = true;
        }
        for (size_t idx : g_pending_ensure)
            out::text(stdout, row_text(g_rows[idx], idx, g_width) + "\n");
        for (auto& line : g_pending_log) out::text(stdout, line);
        g_pending_ensure.clear();
        g_pending_log.clear();
        std::fill(g_dirty.begin(), g_dirty.end(), false);
        g_block_grew = false;
        return;
    }

    if (g_block_grew) {
        redraw_all_locked();
        g_block_grew = false;
        std::fill(g_dirty.begin(), g_dirty.end(), false);
        g_pending_ensure.clear();
        flush_log_locked();
        return;
    }

    // Обычный тик: перерисовываем только изменённые строки.
    for (size_t i = 0; i < g_rows.size(); i++) {
        if (g_dirty[i]) draw_row_locked(i);
    }
    std::fill(g_dirty.begin(), g_dirty.end(), false);
    g_pending_ensure.clear();
    flush_log_locked();
}

// Поток отрисовки. Спящий до появления работы; после пробуждения ждёт ещё
// kDebounce, чтобы собрать пачку обновлений (например, 11 begin_file подряд)
// в одну отрисовку вместо синхронных перерисовок в воркерах. Параллельно с
// ожиданием работы каждые kResizePoll опрашивает размер окна: ресайз
// перерисовывает статусбар сразу, не дожидаясь следующего тика прогресса.
void render_loop() {
    std::unique_lock<std::mutex> lk(g_m);
    while (true) {
        g_cv.wait_for(lk, kResizePoll, [&] { return g_render_stop || g_pending; });
        if (g_render_stop) break;
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

// Гарантирует, что файл idx попал в блок (строки видны в порядке файлов).
// Пропущенные файлы приходят сразу в end_file без begin_file. Выполняется
// только в потоке отрисовки; сам вывод при переполнении делает render_pass.
void ensure_visible_locked(size_t idx) {
    while (g_visible <= idx) {
        if ((int)g_visible >= g_height - 1) {  // блок заполнил почти весь экран
            g_overflow = true;
            g_overflow_handled = false;
            return;
        }
        g_visible++;
    }
}

// Курсор в последнюю строку scroll-региона и вывод строк лога. Многострочный
// текст разбивается на строки; каждая строка стирается и обрезается до ширины
// окна (длинная строка не должна переноситься в зону блока статуса), после
// чего регион прокручивается.
void log_line(const std::string& text) {
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        std::string line =
            text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        out::text(stdout, pos(scroll_bottom(), 1));
        out::text(stdout, "\x1b[2K");
        out::text(stdout, truncate_tail(u2w(line), (size_t)g_width));
        if (end == std::string::npos) break;
        out::text(stdout, "\r\n");
        start = end + 1;
    }
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
    g_overflow_handled = false;
    g_block_grew = false;
    g_width = 80;
    g_height = 25;
    g_size_valid = false;
    g_size_forced = false;
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

    // Весь псевдографический интерфейс живёт в альтернативном буфере экрана:
    // исходный экран (с командной строкой и прежним выводом) сохраняется
    // и восстанавливается в shutdown() — как у mc/opencode.
    out::text(stdout, "\x1b[?1049h");
    out::text(stdout, "\x1b[2J" + pos(1, 1));
    g_drawn_top = 0;
    g_drawn_rows = 0;

    // Вся отрисовка — в отдельном потоке; воркеры его только будят.
    g_dirty.assign(g_total, false);
    g_render_stop = false;
    g_pending = false;
    g_render_thread = std::thread(render_loop);
}

bool interactive() { return g_interactive; }

void log(const std::string& line) {
    if (!g_interactive) {
        out::text(stdout, line);
        return;
    }
    std::lock_guard<std::mutex> lk(g_m);
    g_pending_log.push_back(line);
    g_pending = true;
    g_cv.notify_all();
}

void error(const std::string& line) {
    if (!g_interactive) {
        out::text(stderr, line);
        return;
    }
    std::lock_guard<std::mutex> lk(g_m);
    g_pending_log.push_back(line);
    g_pending = true;
    g_cv.notify_all();
}

void begin_file(size_t idx, const std::string& label, size_t total_tasks) {
    if (!g_interactive) return;
    std::lock_guard<std::mutex> lk(g_m);
    if (idx >= g_total) return;
    g_rows[idx].label = label;
    g_rows[idx].total = total_tasks;
    g_rows[idx].done = 0;
    g_rows[idx].finished = false;
    g_dirty[idx] = true;
    g_pending_ensure.push_back(idx);
    g_block_grew = true;
    g_pending = true;
    g_cv.notify_all();
}

void tick(size_t idx, size_t done) {
    if (!g_interactive) return;
    std::lock_guard<std::mutex> lk(g_m);
    if (idx >= g_total) return;
    if (g_rows[idx].label.empty()) return;
    g_rows[idx].done = done;
    g_dirty[idx] = true;
    g_pending = true;
    g_cv.notify_all();
}

void end_file(size_t idx, const std::string& label, const std::string& winner, double pct) {
    if (!g_interactive) return;
    std::lock_guard<std::mutex> lk(g_m);
    if (idx >= g_total) return;
    g_rows[idx].label = label;
    g_rows[idx].finished = true;
    g_rows[idx].winner = winner;
    g_rows[idx].pct = pct;
    g_dirty[idx] = true;
    g_pending_ensure.push_back(idx);
    g_block_grew = true;
    g_pending = true;
    g_cv.notify_all();
}

void shutdown() {
    std::unique_lock<std::mutex> lk(g_m);
    if (!g_interactive) return;
    g_render_stop = true;
    g_cv.notify_all();
    lk.unlock();
    // Ждём, пока поток отрисовки сбросит накопленный лог и завершится.
    if (g_render_thread.joinable()) g_render_thread.join();
    lk.lock();
    out::text(stdout, "\x1b[0m");
    if (g_size_valid && !g_overflow) {
        out::text(stdout, "\x1b[r");      // весь экран снова скроллится
        out::text(stdout, "\x1b[2J");     // очистить альтернативный буфер
    }
    out::text(stdout, "\x1b[?1049l");     // вернуться к исходному экрану
    if (g_orig_mode_valid) {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (h != INVALID_HANDLE_VALUE && h != nullptr) SetConsoleMode(h, g_orig_mode);
    }
    g_interactive = false;
    g_visible = 0;
    g_overflow = false;
    g_drawn_top = 0;
    g_drawn_rows = 0;
}

}  // namespace status
