#include "serve.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <httplib.h>

#include "config.h"
#include "contract.h"
#include "i18n.h"
#include "persist.h"
#include "report.h"
#include "tool.h"
#include "util.h"
#include "version.h"

#ifdef _WIN32
#include <bcrypt.h>
#include <windows.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <fcntl.h>
#include <fstream>
#include <unistd.h>
#endif

#include "http_api.h"

namespace dsvc {

namespace {

double monotonic_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string gen_token() {
    unsigned char buf[32];
#ifdef _WIN32
    NTSTATUS st =
        BCryptGenRandom(nullptr, buf, sizeof(buf), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (st != 0) {
        uint64_t x = (uint64_t)GetTickCount64();  // крайний, небезопасный фолбэк
        for (size_t i = 0; i < sizeof(buf); i++) {
            x = x * 6364136223846793005ULL + 1442695040888963407ULL;
            buf[i] = (unsigned char)(x >> 56);
        }
    }
#else
    std::ifstream f("/dev/urandom", std::ios::binary);
    if (f) f.read((char*)buf, sizeof(buf));
    else memset(buf, 0, sizeof(buf));
#endif
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(sizeof(buf) * 2);
    for (size_t i = 0; i < sizeof(buf); i++) {
        out.push_back(hex[buf[i] >> 4]);
        out.push_back(hex[buf[i] & 0xF]);
    }
    return out;
}

std::string discovery_path(const std::string& override_path) {
    if (!override_path.empty()) return override_path;
    if (const char* e = std::getenv("LLAO_DISCOVERY")) {
        if (*e) return e;
    }
#ifdef _WIN32
    const char* la = std::getenv("LOCALAPPDATA");
    std::string base = la ? la : ".";
    return util::join_path(util::join_path(base, "llao"), "daemon.json");
#else
    const char* xdg = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    std::string base = xdg ? xdg : (home ? std::string(home) + "/.local/share" : "/tmp");
    return util::join_path(util::join_path(base, "llao"), "daemon.json");
#endif
}

bool write_discovery(const std::string& path, const nlohmann::json& j) {
    if (path.empty()) return false;
    util::mkdirs(util::dir_name(path));
    std::string tmp = path + ".tmp";
    if (!util::write_text(tmp, j.dump())) return false;
    for (int i = 0; i < 5; i++) {
        if (std::rename(tmp.c_str(), path.c_str()) == 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

#ifdef _WIN32
volatile LONG g_ctrl = 0;
BOOL WINAPI ctrl_handler(DWORD) {
    InterlockedExchange(&g_ctrl, 1);
    return TRUE;
}
void install_signal_handlers() { SetConsoleCtrlHandler(ctrl_handler, TRUE); }
#else
volatile sig_atomic_t g_ctrl = 0;
void sig_handler(int) { g_ctrl = 1; }
void install_signal_handlers() {
    std::signal(SIGINT, sig_handler);
    std::signal(SIGTERM, sig_handler);
}
#endif

void print_help() {
    std::printf(
        "LLAO server — headless-движок с HTTP-API и веб-UI\n"
        "\n"
        "Использование: llao serve [опции]\n"
        "\n"
        "Опции сессии (дефолты оптимизации):\n"
        "  --jobs N|M.M          число потоков или множитель ядер (default 2.0)\n"
        "  --verify MODE         all|winner|none (default winner)\n"
        "  --dry-run             не записывать результат\n"
        "  --no-download         не скачивать кодеки (стартовый гейт только проверяет)\n"
        "  --no-stats            не накапливать stats.json\n"
        "  --debug               журнал runs/*.jsonl + отладка\n"
        "  --report PATH         итоговый отчёт при shutdown\n"
        "  --restore-to ID       целевой формат режима восстановления (default flac)\n"
        "\n"
        "Сервер:\n"
        "  --bind ADDR           адрес прослушивания (default 0.0.0.0)\n"
        "  --port N              порт (default 18180); 0 = свободный\n"
        "  --token HEX           явный токен (по умолчанию генерируется)\n"
        "  --no-auth             выключить авторизацию (только отладка!)\n"
        "  --discovery PATH      путь к discovery-файлу (default: %%LOCALAPPDATA%%/llao/daemon.json)\n"
        "\n"
        "  --help                эта справка\n");
}

}  // namespace

// --- DaemonSession ---

DaemonSession::DaemonSession(optimize::Options opts, EventBuffer* ev, StateMirror* st,
                             std::string restore_to)
    : opts_(std::move(opts)), restore_to_(std::move(restore_to)), ev_(ev), st_(st) {
    started_ = monotonic_s();
}

DaemonSession::~DaemonSession() { shutdown(); }

int DaemonSession::start(std::string* err) {
    try {
        auto fmts = config::load_all();
        formats_cache_ = nlohmann::json::array();
        std::vector<config::Format> enabled;
        for (const auto& f : fmts) {
            if (!f.enabled) continue;
            enabled.push_back(f);
            formats_cache_.push_back(
                {{"id", f.id},
                 {"extensions", nlohmann::json::array({f.extension})}});
        }
        if (!restore_to_.empty()) {
            bool known = false;
            for (const auto& f : formats_cache_)
                if (f.contains("id") && f["id"] == restore_to_) { known = true; break; }
            if (!known) {
                if (err) *err = "restore: target format \"" + restore_to_ +
                                "\" not found in formats/*.json";
                return 1;
            }
        }

        // Стартовый гейт кодеков: все включённые форматы должны быть готовы
        // (утилита в bin/<id>/ или PATH, cli_check из formats/*.json сходится).
        // Проблемы агрегируются и блокируют старт — сервер отказывает в запуске
        // при заведомо неисправных конверторах, а не «тихо» падает во время
        // прогона. Уважает --no-download (гейт не качает).
        std::vector<std::string> problems;
        bool download = !opts_.no_download;
        for (const auto& f : enabled) {
            tool::Status s = tool::ensure(f, download);
            std::string issue;
            if (s.status == "missing") {
                issue = i18n::fmt(
                    "format %s: utility not available (run `llao tools %s` "
                    "or install it into bin/%s/)",
                    f.id.c_str(), f.id.c_str(), f.id.c_str());
            }
            if (issue.empty() && !s.path.empty()) issue = tool::check_config(f);
            if (!issue.empty()) problems.push_back(issue);
        }
        if (!problems.empty()) {
            std::string msg =
                i18n::str("codecs are not ready — the server refuses to start:\n");
            for (const auto& p : problems) msg += "  - " + p + "\n";
            msg += i18n::str(
                "fix the utilities (`llao tools`) and restart, or use `--no-download`");
            if (err) *err = msg;
            return 1;
        }
    } catch (const std::exception& exc) {
        if (err) *err = exc.what();
        return 1;
    }

    sink_ = std::make_unique<DaemonSink>(ev_, st_);
    obs::set_sink(sink_.get());
    // Инкрементальная персистентность: состояние файла изменилось (prep/ok/
    // stopped/error) — записываем queue.json. Колбэк вызывается воркерами
    // движка (в т.ч. под qm на некоторых путях mark_stopped), поэтому persist()
    // не должен трогать движок — только зеркало и свой мьютекс.
    sink_->set_on_change([this] { persist(false); });

    engine_ = std::make_unique<optimize::Engine>();
    if (int rc = engine_->init(opts_, {}, err); rc != 0) return rc;
    return 0;
}

std::string DaemonSession::version() const { return LLAO_VERSION; }

double DaemonSession::uptime_s() const { return monotonic_s() - started_; }

nlohmann::json DaemonSession::session_options() const {
    return {{"jobs", opts_.jobs},
            {"jobs_float", opts_.jobs_float},
            {"dry_run", opts_.dry_run},
            {"verify", dsvc::verify_str(opts_.verify)},
            {"ignore_errors", true}};
}

nlohmann::json DaemonSession::counters() const {
    int total = 0, done = 0, failed = 0, stopped = 0, ok = 0;
    for (const auto& r : st_->snapshot()) {
        total++;
        if (r.state == "ok" || r.state == "stopped" || r.state == "error") done++;
        if (r.state == "error") failed++;
        if (r.state == "stopped") stopped++;
        if (r.state == "ok") ok++;
    }
    return {{"total", total}, {"done", done}, {"failed", failed},
            {"stopped", stopped}, {"ok", ok}};
}

bool DaemonSession::paused() const { return paused_.load(); }

void DaemonSession::set_paused(bool paused) {
    if (paused_.exchange(paused) == paused) return;
    ev_->push(paused ? "stopped" : "resumed");
    if (paused) {
        if (engine_) engine_->pause();
    } else {
        if (engine_) engine_->resume();
    }
}

void DaemonSession::add_locked(const std::vector<std::string>& paths,
                               const std::string& mode, const std::string& target_dir,
                               nlohmann::json& result, std::vector<size_t>& new_ids) {
    bool mode_ok = false;
    optimize::JobMode jm = dsvc::parse_mode(mode, &mode_ok);
    if (!mode_ok) {
        result["rejected"].push_back(
            {{"path", "<mode>"}, {"reason", "mode must be optimize|restore"}});
        return;
    }
    bool have_target = !target_dir.empty();
    if (have_target) {
        // Целевая папка: должна существовать на хосте демона заранее. Исходная
        // структура пачки воспроизводится внутри неё, файлы вне её каталогами
        // не нуждаются в предварительном создании (mkdirs при записи).
        if (!util::dir_exists(target_dir)) {
            result["rejected"].push_back(
                {{"path", "<target_dir>"}, {"reason", "target folder not found"}});
            return;
        }
    }
    if (jm == optimize::JobMode::Restore) {
        if (restore_to_.empty()) {
            result["rejected"].push_back(
                {{"path", "<mode>"}, {"reason", "restore: target format not configured"}});
            return;
        }
        bool known = false;
        for (const auto& f : formats_cache_)
            if (f.contains("id") && f["id"] == restore_to_) { known = true; break; }
        if (!known) {
            result["rejected"].push_back(
                {{"path", "<mode>"},
                 {"reason", "restore: target format \"" + restore_to_ + "\" not found"}});
            return;
        }
    }
    std::vector<std::string> accepted;
    for (const auto& raw : paths) {
        std::string p = dsvc::normalize_path(raw);
        if (p.empty()) {
            result["rejected"].push_back({{"path", raw}, {"reason", "empty path"}});
            continue;
        }
        bool is_dir = util::dir_exists(p);
        if (!is_dir && !util::file_exists(p)) {
            result["rejected"].push_back(
                {{"path", raw},
                 {"reason", "path not found on daemon host (use --upload in client)"}});
            continue;
        }
        if (added_paths_.count(p)) {
            // Если путь был удалён/завершён — разрешить повторное добавление
            bool still_active = false;
            if (engine_) {
                auto snap = engine_->snapshot();
                for (const auto& f : snap) if (f.path == p) {
                    if (f.state == "queued" || f.state == "prep" || f.state == "running") still_active = true;
                    break;
                }
            }
            if (still_active) {
                result["rejected"].push_back({{"path", raw}, {"reason", "already in queue"}});
                continue;
            } else {
                added_paths_.erase(p);
            }
        }
        added_paths_.insert(p);
        accepted.push_back(p);
    }
    if (!accepted.empty() && engine_) {
        optimize::AddOptions ao;
        ao.mode = jm;
        ao.target_dir = target_dir;
        ao.to = (jm == optimize::JobMode::Restore) ? restore_to_ : std::string();
        new_ids = engine_->add(accepted, ao);
    }
}

void DaemonSession::add(const std::vector<std::string>& paths,
                        const std::string& mode, const std::string& target_dir,
                        nlohmann::json& result) {
    // Вся приёмка и само добавление — под mt_ (см. комментарий в serve.h).
    // Ответ строится по idx, возвращённым движком, а не по размеру зеркала.
    std::vector<size_t> new_ids;
    {
        std::lock_guard<std::mutex> lk(mt_);
        add_locked(paths, mode, target_dir, result, new_ids);
    }
    bool need_resume = !new_ids.empty() && paused_.load();
    if (need_resume) set_paused(false);

    // begin_file синхронен (append_files эмитит события до возврата),
    // поэтому строки уже в зеркале — ищем по id, без эвристики хвоста.
    auto rows = st_->snapshot();
    // Полные пути новых строк (из движка): для персистентности строка должна
    // знать исходный путь; label — это только rel (относительный).
    std::unordered_map<size_t, std::string> id2path;
    if (engine_) {
        for (const auto& f : engine_->snapshot()) id2path[f.idx] = f.path;
    }
    for (size_t id : new_ids) {
        for (const auto& r : rows)
            if (r.id == id) {
                result["added"].push_back({{"id", r.id}, {"label", r.label}});
                auto pit = id2path.find(id);
                if (pit != id2path.end() && !pit->second.empty()) {
                    st_->set_path(id, pit->second);
                    set_had_sidecar(id, pit->second);
                }
                break;
            }
    }
    persist(false);
}

bool DaemonSession::cancel_file(uint64_t id) {
    if (!engine_) return false;
    std::lock_guard<std::mutex> lk(mt_);
    auto snap = engine_->snapshot();
    for (const auto& f : snap)
        if (f.idx == (size_t)id) {
            bool active = !(f.state == "ok" || f.state == "stopped" ||
                            f.state == "error" || f.state == "removed");
            engine_->remove((size_t)id);
            if (active) {
                ev_->push_for(id, "stopped");
                st_->set_state(id, "stopped");
            }
            persist(false);
            return true;
        }
    return false;
}

bool DaemonSession::remove_locked(uint64_t id) {
    std::string path_to_remove;
    bool found = false;
    bool in_engine = false;
    {
        auto snap_before = engine_ ? engine_->snapshot() : std::vector<optimize::EngineFile>();
        for (const auto& f : snap_before)
            if (f.idx == (size_t)id) {
                found = true;
                in_engine = true;
                path_to_remove = f.path;
                break;
            }
    }
    if (!found) {
        // Строка вне движка (восстановленная из queue.json): удаляем из зеркала.
        auto rows = st_->snapshot();
        for (const auto& r : rows)
            if (r.id == (size_t)id) {
                found = true;
                path_to_remove = r.path.empty() ? r.label : r.path;
                break;
            }
    }
    if (!found) return false;
    if (in_engine) engine_->remove((size_t)id);
    st_->remove((size_t)id);
    if (!path_to_remove.empty()) added_paths_.erase(path_to_remove);
    ev_->push_for(id, "removed");
    persist(false);
    return true;
}

bool DaemonSession::remove(uint64_t id) {
    std::lock_guard<std::mutex> lk(mt_);
    return remove_locked(id);
}

uint64_t DaemonSession::bulk_remove(const std::vector<size_t>& ids) {
    if (!engine_) return 0;
    std::lock_guard<std::mutex> lk(mt_);
    uint64_t removed = 0;
    for (size_t id : ids)
        if (remove_locked(id)) removed++;
    return removed;
}

uint64_t DaemonSession::bulk_cancel(const std::vector<size_t>& ids) {
    if (!engine_) return 0;
    std::lock_guard<std::mutex> lk(mt_);
    auto snap = engine_->snapshot();
    uint64_t cancelled = 0;
    for (size_t id : ids) {
        bool active = false;
        for (const auto& f : snap)
            if (f.idx == id) {
                active = !(f.state == "ok" || f.state == "stopped" ||
                           f.state == "error" || f.state == "removed");
                break;
            }
        if (!active) continue;
        engine_->remove(id);
        ev_->push_for(id, "stopped");
        st_->set_state(id, "stopped");
        cancelled++;
    }
    persist(false);
    return cancelled;
}

uint64_t DaemonSession::cancel_all_active() {
    if (!engine_) return 0;
    std::lock_guard<std::mutex> lk(mt_);
    auto snap = engine_->snapshot();
    uint64_t cancelled = 0;
    for (const auto& f : snap) {
        bool active = !(f.state == "ok" || f.state == "stopped" ||
                        f.state == "error" || f.state == "removed");
        if (!active) continue;
        engine_->remove(f.idx);
        ev_->push_for(f.idx, "stopped");
        st_->set_state(f.idx, "stopped");
        cancelled++;
    }
    persist(false);
    return cancelled;
}

size_t DaemonSession::sort_by_path() {
    if (!engine_) return 0;
    std::lock_guard<std::mutex> lk(mt_);
    auto snap = engine_->snapshot();
    std::unordered_map<size_t, size_t> pos;
    pos.reserve(snap.size());
    for (size_t k = 0; k < snap.size(); k++) pos[snap[k].idx] = k;
    std::vector<size_t> order;
    order.reserve(snap.size());
    for (const auto& f : snap) order.push_back(f.idx);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const std::string& pa = snap[pos[a]].path;
        const std::string& pb = snap[pos[b]].path;
        if (pa.empty()) {
            if (pb.empty()) return false;
            return false;  // пустые пути — в конец
        }
        if (pb.empty()) return true;
        return pa < pb;
    });
    if (!engine_->reorder(order)) return 0;
    st_->reorder(order);
    ev_->push("reordered", {{"order", order}});
    persist(false);
    return order.size();
}

uint64_t DaemonSession::clear_done() {
    if (!engine_) return 0;
    std::lock_guard<std::mutex> lk(mt_);
    auto rows = st_->snapshot();
    uint64_t removed = 0;
    for (const auto& r : rows)
        if (r.state == "ok" && remove_locked(r.id)) removed++;
    persist(false);
    return removed;
}

namespace {

bool is_done_state(const std::string& st) {
    return st == "ok" || st == "stopped" || st == "error" || st == "removed";
}

}  // namespace

bool DaemonSession::restart(uint64_t id) {
    if (!engine_) return false;
    // Универсальный перезапуск: активный файл сначала останавливается
    // (cancel + мгновенный kill процессов), ожидается завершение. Затем
    // НОВАЯ строка добавляется СНАЧАЛА, и только после успешного добавления
    // удаляется старая. Так исключено «исчезновение файла из списка» даже
    // когда старая строка уже была автоубрана движком после отмены.
    // Восстановленные из queue.json строки (вне движка, id из высокого
    // диапазона) перезапускаются без движковых операций: только пересоздание
    // зеркальной строки через add_locked.
    std::string path;
    std::string mode, target_dir;  // режим и целевая папка исходной строки
    bool was_active = false;
    bool in_engine = false;
    {
        auto snap = engine_->snapshot();
        for (const auto& f : snap)
            if (f.idx == (size_t)id) {
                in_engine = true;
                path = f.path;
                mode = f.mode;
                target_dir = f.target_dir;
                was_active = !is_done_state(f.state);
                break;
            }
        if (!in_engine) {
            // Строка вне движка: параметры — из зеркала (label = полный путь).
            auto rows = st_->snapshot();
            for (const auto& r : rows)
                if (r.id == (size_t)id) {
                    path = r.path.empty() ? r.label : r.path;
                    mode = r.mode;
                    target_dir = r.target_dir;
                    break;
                }
        }
        if (path.empty()) return false;
        if (in_engine && was_active) engine_->remove((size_t)id);  // cancel + kill, без зеркала
        if (in_engine && was_active) {
            bool settled = false;
            for (int i = 0; i < 200; i++) {
                auto s2 = engine_->snapshot();
                for (const auto& f : s2)
                    if (f.idx == (size_t)id && is_done_state(f.state)) {
                        settled = true;
                        break;
                    }
                if (settled) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!settled) return false;
        } else if (in_engine) {
            // Готовая строка (ok/stopped/error): процессов нет, но повторное
            // добавление спотыкается о seen_paths_ движка, поэтому старую строку
            // убираем СРАЗУ (до add); позицию восстановим в хвосте restart.
            {
                std::lock_guard<std::mutex> lk(mt_);
                auto s2 = engine_->snapshot();
                for (const auto& f : s2)
                    if (f.idx == (size_t)id) {
                        engine_->remove((size_t)id);
                        break;
                    }
            }
        }
    }
    // Исходник должен существовать: после успешной обработки (ok) файл
    // заменён другим форматом, перезапускать нечего.
    if (!util::dir_exists(path) && !util::file_exists(path)) return false;
    std::lock_guard<std::mutex> lk(mt_);
    nlohmann::json tmp = {{"added", nlohmann::json::array()},
                          {"rejected", nlohmann::json::array()}};
    std::vector<size_t> new_ids;
    // Перезапуск сохраняет режим и целевую папку исходной строки (optimize
    // остаётся optimize, restore — restore в ту же папку), иначе семантика
    // задания изменилась бы «под ногами» пользователя.
    add_locked({path}, mode, target_dir, tmp, new_ids);
    if (new_ids.empty()) return false;  // старую строку не трогаем
    size_t new_id = new_ids.front();
    // Полный путь новой строки — из движка (label — только rel). Без него
    // snapshot_for_persist() уйдёт в label, и после перезапуска демона с
    // другой cwd путь в queue.json окажется неверным.
    {
        auto snap = engine_->snapshot();
        for (const auto& f : snap)
            if (f.idx == new_id && !f.path.empty()) {
                st_->set_path(new_id, f.path);
                set_had_sidecar(new_id, f.path);
                break;
            }
    }
    // Позицию старой строки берём из ВИДИМОГО списка (зеркало) в текущий
    // момент — она уже учитывает все удалённые/завершённые строки. Позиция в
    // движке может отличаться (движок хранит и «зомби»-строки), поэтому
    // движок переупорядочиваем по его же позиции.
    size_t mirror_pos = SIZE_MAX;
    size_t engine_pos = SIZE_MAX;
    {
        auto rows = st_->snapshot();
        for (size_t k = 0; k < rows.size(); k++)
            if (rows[k].id == (size_t)id) {
                mirror_pos = k;
                break;
            }
        if (in_engine) {
            auto snap = engine_->snapshot();
            for (size_t k = 0; k < snap.size(); k++)
                if (snap[k].idx == (size_t)id) {
                    engine_pos = k;
                    break;
                }
        }
    }
    // Старая строка может отсутствовать (автоуборка после отмены) — не ошибка.
    if (in_engine) {
        auto snap = engine_->snapshot();
        for (const auto& f : snap)
            if (f.idx == (size_t)id) {
                remove_locked(id);
                break;
            }
    } else {
        // Вне движка: старую зеркальную строку снимаем напрямую (remove_locked
        // умеет и такие), чтобы не было дубля после повторного add.
        auto rows = st_->snapshot();
        for (const auto& r : rows)
            if (r.id == (size_t)id) {
                remove_locked(r.id);
                break;
            }
    }
    // Восстанавливаем прежнюю позицию строки, чтобы перезапуск не выглядел
    // как «файл удалён из списка» и не ломал ручную перетасовку очереди.
    // Зеркало (видимый список) и движок переупорядочиваются отдельно: движок
    // хранит отменённые строки-«зомби» (idx, которые из зеркала уже ушли),
    // поэтому подавать туда зеркальный порядок нельзя.
    {
        auto rows = st_->snapshot();
        std::vector<size_t> morder;
        morder.reserve(rows.size());
        for (const auto& r : rows)
            if (r.id != new_id) morder.push_back(r.id);
        if (!morder.empty()) {
            size_t goal = mirror_pos != SIZE_MAX && mirror_pos < morder.size()
                              ? mirror_pos
                              : morder.size();
            morder.insert(morder.begin() + goal, new_id);
            if (st_->reorder(morder)) ev_->push("reordered", {{"order", morder}});
        }
        if (in_engine) {
            auto snap = engine_->snapshot();
            std::vector<size_t> eorder;
            eorder.reserve(snap.size());
            for (const auto& f : snap)
                if (f.idx != new_id) eorder.push_back(f.idx);
            if (!eorder.empty()) {
                size_t goal = engine_pos != SIZE_MAX && engine_pos < eorder.size()
                                  ? engine_pos
                                  : eorder.size();
                eorder.insert(eorder.begin() + goal, new_id);
                engine_->reorder(eorder);
            }
        }
    }
    // Ручной перезапуск при остановленной очереди должен снимать паузу,
    // иначе файл добавится и останется в «queued» навсегда.
    if (paused_.load()) set_paused(false);
    persist(false);
    return true;
}

bool DaemonSession::reorder(const std::vector<size_t>& order) {
    if (!engine_) return false;
    bool ok = engine_->reorder(order);
    if (ok) {
        st_->reorder(order);
        ev_->push("reordered", {{"order", order}});
        persist(false);
    }
    return ok;
}

std::vector<persist::Row> DaemonSession::snapshot_for_persist(bool final) const {
    // Только зеркало: ни движок, ни qm не трогаем. Строки движка хранят полный
    // путь в Row::path (выставляется при add), восстановленные — в label.
    std::vector<persist::Row> rows;
    for (const auto& r : st_->snapshot()) {
        persist::Row p;
        p.path = r.path.empty() ? r.label : r.path;
        if (p.path.empty()) continue;  // гонка add: строка ещё без пути — пропускаем
        p.mode = dsvc::mode_str(dsvc::parse_mode(r.mode));
        p.target_dir = r.target_dir;
        p.out_path = r.out_path;
        p.pct = r.pct;
        p.last_error = r.last_error;
        p.had_sidecar = r.had_sidecar;
        std::string st = r.state;
        if (final && (st == "queued" || st == "prep" || st == "running")) st = "queued";
        p.state = st;
        // has_sidecar: актуально только для ok и вычисляется по диску (живёт ли
        // sidecar рядом с итогом). Для остальных состояний — false.
        if (p.state == "ok" && !p.out_path.empty()) {
            p.has_sidecar = util::file_exists(persist::sidecar_path_for(p.out_path));
        }
        rows.push_back(std::move(p));
    }
    return rows;
}

void DaemonSession::persist_rows(const std::vector<persist::Row>& rows) {
    if (persist_path_.empty()) return;
    std::string err;
    if (!persist::write_file(persist_path_, rows)) {
        std::fprintf(stderr, "WARNING: could not persist queue to %s\n",
                     persist_path_.c_str());
    }
}

void DaemonSession::persist(bool final) {
    // Собственный мьютекс: persist() вызывается и из RPC-потоков (держащих
    // mt_), и из воркеров движка (держащих qm) — сериализуем запись здесь,
    // без вложенных блокировок движка/сессии.
    std::lock_guard<std::mutex> lk(persist_m_);
    if (shutting_down_.load() && !final) return;  // после shutdown — только финальный
    persist_rows(snapshot_for_persist(final));
}

void DaemonSession::set_had_sidecar(size_t id, const std::string& full_path) {
    bool had = util::file_exists(persist::sidecar_path_for(full_path));
    st_->set_sidecar_flags(id, had, false);
}

// Строка вне движка: всего строк в persist-файле может быть больше, чем
// вмещает движок (движок хранит только запускаемые). id строк, которые не
// попадают в движок, берутся из высокого диапазона, чтобы не пересекаться с
// монотонными idx движка (jobs растёт от 0).
static constexpr size_t kRestoredIdBase = 1ULL << 40;

void DaemonSession::load_persisted(std::string* err) {
    if (persist_path_.empty()) return;
    std::vector<persist::Row> rows;
    if (!persist::read_file(persist_path_, &rows)) {
        // Нет файла или он не читается (v1/битый) — новый старт без очереди.
        // Не считаем это ошибкой: v1 и обрыв между записями здесь не фатальны.
        if (err) *err = std::string();
        return;
    }
    if (rows.empty()) return;
    size_t rid = kRestoredIdBase;
    std::vector<size_t> order;  // итоговый порядок строк в зеркале
    order.reserve(rows.size());

    auto restore_to_mirror = [&](const persist::Row& pr, const std::string& st,
                                 const std::string& why) {
        dsvc::Row r;
        r.id = rid++;
        r.label = pr.path;
        r.path = pr.path;
        r.state = st;
        r.pct = pr.pct;
        r.mode = dsvc::mode_str(dsvc::parse_mode(pr.mode));
        r.target_dir = pr.target_dir;
        r.out_path = pr.out_path;
        r.last_error = why;
        r.had_sidecar = pr.had_sidecar;
        r.has_sidecar = pr.has_sidecar;
        st_->upsert(r);
        added_paths_.insert(pr.path);
        order.push_back(r.id);
    };

    // Валидация путей-источников (для не-ok строк): файл и, при необходимости,
    // его sidecar обязаны существовать на хосте демона.
    auto source_ok = [](const persist::Row& pr, std::string* why) -> bool {
        if (!util::dir_exists(pr.path) && !util::file_exists(pr.path)) {
            *why = "исходный файл не найден при перезапуске: " + pr.path;
            return false;
        }
        if (pr.had_sidecar &&
            !util::file_exists(persist::sidecar_path_for(pr.path))) {
            *why = "sidecar (теги) исходника не найден при перезапуске: " +
                   persist::sidecar_path_for(pr.path);
            return false;
        }
        return true;
    };

    for (const auto& pr : rows) {
        if (pr.state == "queued") {
            // Продолжаем только файлы, до которых очередь ещё не дошла; все
            // проверки — как в add_locked. При любой неудаче строка остаётся
            // в зеркале со статусом stopped и причиной, в движок не заносится.
            std::string why;
            if (!source_ok(pr, &why)) {
                restore_to_mirror(pr, "stopped", why);
                continue;
            }
            nlohmann::json tmp = {{"added", nlohmann::json::array()},
                                  {"rejected", nlohmann::json::array()}};
            std::vector<size_t> nids;
            add_locked({pr.path}, pr.mode, pr.target_dir, tmp, nids);
            if (!nids.empty()) {
                // Строка попала в движок: движок уже эмитил begin_file/добавил
                // в зеркало. Позиция в порядке очереди — по persist-порядку.
                for (size_t nid : nids) order.push_back(nid);
                // Полный путь (label из движка — только rel) и флаг sidecar,
                // иначе при следующем persist очередь сохранит относительный путь.
                for (size_t nid : nids) {
                    auto snap = engine_->snapshot();
                    for (const auto& f : snap)
                        if (f.idx == nid && !f.path.empty()) {
                            st_->set_path(nid, f.path);
                            set_had_sidecar(nid, f.path);
                            break;
                        }
                }
                ev_->push("restored", {{"type", "queued"}, {"id", nids.front()},
                                       {"path", pr.path}});
                continue;
            }
            std::string reason =
                !tmp["rejected"].empty() && tmp["rejected"][0].contains("reason")
                    ? tmp["rejected"][0]["reason"].get<std::string>()
                    : "не удалось восстановить задачу из queue.json";
            restore_to_mirror(pr, "stopped", reason);
        } else if (pr.state == "ok") {
            // Итог проверяем по out_path (и по sidecar, если has_sidecar).
            std::string why;
            if (!pr.out_path.empty()) {
                if (!util::file_exists(pr.out_path))
                    why = "результат не найден при перезапуске: " + pr.out_path;
                else if (pr.has_sidecar &&
                         !util::file_exists(persist::sidecar_path_for(pr.out_path)))
                    why = "sidecar результата не найден при перезапуске: " +
                          persist::sidecar_path_for(pr.out_path);
            }
            restore_to_mirror(pr, "ok", why);
            ev_->push("restored", {{"type", "ok"}, {"path", pr.path}});
        } else if (pr.state == "prep" || pr.state == "running") {
            // Прервано во время обработки — подозрительная строка; в движок не
            // заносим, пользователь явно перезапустит через «Запустить».
            std::string why = "остановлено при перезапуске (обработка не завершена)";
            restore_to_mirror(pr, "stopped", why);
            ev_->push("restored", {{"type", "interrupted"}, {"path", pr.path}});
        } else {
            // stopped | error: сохраняем статус, но исходник + sidecar должны
            // существовать; иначе отмечаем причину (после перезапуска файл мог
            // исчезнуть — «Запустить» всё равно откажет без него).
            std::string why;
            if (!source_ok(pr, &why)) {
                restore_to_mirror(pr, pr.state, why);
                continue;
            }
            restore_to_mirror(pr, pr.state, pr.last_error);
        }
    }

    // Итоговый порядок очереди по строкам из queue.json (движок-строки уже в
    // зеркале в порядке add; восстановленные — в порядке обхода).
    if (!order.empty()) st_->reorder(order);
    ev_->push("restored", {{"rows", (int)rows.size()}});
    if (err) *err = std::string();
    // Сразу пишем consolidated-состояние, чтобы в файле была одна актуальная
    // картина (новые id вместо персист-состояний) — но только если есть движок.
    persist(false);
}

void DaemonSession::request_shutdown(bool /*force*/) {
    if (on_shutdown_) on_shutdown_();
}

nlohmann::json DaemonSession::formats() const {
    return {{"formats", formats_cache_}};
}

nlohmann::json DaemonSession::debug_state() {
    if (!engine_) return nullptr;
    return engine_->debug_state();
}

void DaemonSession::shutdown() {
    if (shutting_down_.exchange(true)) return;

    // ФИНАЛЬНЫЙ persist ДО остановки движка: active (queued|prep|running) ->
    // queued на перезапуске; abort переводит активные в stopped, и после
    // engine_->shutdown() их нельзя было бы отличить от остановленных
    // пользователем. Пока движок жив, зеркало ещё содержит полный порядок
    // очереди (include строки вне движка).
    {
        std::lock_guard<std::mutex> lk(persist_m_);
        persist_rows(snapshot_for_persist(true));
    }

    if (engine_) engine_->shutdown();

    // Итоговый отчёт (если задан --report).
    if (!opts_.report_path.empty() && engine_) {
        std::vector<report::FileSummary> summaries;
        for (const auto& ef : engine_->snapshot()) {
            report::FileSummary s;
            s.path = ef.path;
            s.status = ef.state;
            s.detail = ef.detail;
            s.original = ef.original;
            s.best = ef.best;
            s.savings_pct = ef.pct;
            s.best_format = ef.best_format;
            summaries.push_back(std::move(s));
        }
        std::string rp = opts_.report_path;
        if (util::dir_exists(rp) || util::ends_with(rp, "/") || util::ends_with(rp, "\\")) {
            util::mkdirs(rp);
            rp = util::join_path(rp, "llao-report-" + report::timestamp() + ".txt");
        }
        report::write_report(rp, summaries);
    }

    // Движок ещё жив: деструкторы его заданий при уборке временных файлов
    // логируют в sink(), поэтому обнулять глобальный sink можно только после
    // полного уничтожения движка (иначе SEGV на нулевом указателе).
    engine_.reset();

    obs::set_sink(nullptr);
}

}  // namespace dsvc

// --- entry ---

int dsvc::run_daemon(const std::vector<std::string>& args) {
    dsvc::install_signal_handlers();

#ifdef _WIN32
    // Одиночный экземпляр демона. Именованный мьютекс в пространстве «Local\»
    // (одна копия на сессию пользователя): вторая копия на другом порту будет
    // отвергнута, чтобы не делить tmp-каталог и очередь. Handle держим до
    // выхода — при завершении процесса ОС сама освободит имя.
    HANDLE singleton = CreateMutexW(nullptr, FALSE, L"Local\\llao-singleton");
    if (!singleton) {
        std::fprintf(stderr, "ERROR: could not create singleton mutex (error %lu)\n",
                     (unsigned long)GetLastError());
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        std::fprintf(stderr, "ERROR: another LLAO server is already running\n");
        CloseHandle(singleton);
        return 1;
    }
    (void)singleton;  // живёт до завершения процесса; закрытие — за ОС
#endif

    // По умолчанию слушаем все интерфейсы (0.0.0.0) — веб-интерфейс доступен
    // с других машин локальной сети. Авторизация (токен) включена по умолчанию:
    // без неё только --no-auth для отладки. Порт по умолчанию 18180.
    std::string bind = "0.0.0.0";
    int port = 18180;
    std::string token;
    bool no_auth = false;  // отладка: выключить Bearer-авторизацию
    std::string discovery_override;
    std::string restore_to = "flac";  // целевой формат режима восстановления
    optimize::Options opts;
    opts.mode = optimize::SessionMode::Daemon;
    // Демон по умолчанию валидирует только победителя (winner): валидация
    // промежуточных вариантов была прерогативой CLI.
    opts.verify = optimize::Verify::Winner;

    for (size_t i = 1; i < args.size(); i++) {
        const std::string& a = args[i];
        auto next = [&]() -> std::string { return i + 1 < args.size() ? args[++i] : std::string(); };
        if (a == "--help" || a == "-h") {
            dsvc::print_help();
            return 0;
        } else if (a == "--bind") bind = next();
        else if (a == "--port") port = std::atoi(next().c_str());
        else if (a == "--token") token = next();
        else if (a == "--no-auth") no_auth = true;
        else if (a == "--discovery") discovery_override = next();
        else if (a == "--jobs") {
            std::string v = next();
            opts.jobs = std::atof(v.c_str());
            opts.jobs_float = v.find('.') != std::string::npos;
        } else if (a == "--verify") {
            bool ok = false;
            opts.verify = dsvc::parse_verify(next(), &ok);
            if (!ok) {
                std::fprintf(stderr, "ERROR: bad --verify (all|winner|none)\n");
                return 1;
            }
        } else if (a == "--dry-run") opts.dry_run = true;
        else if (a == "--no-download") opts.no_download = true;
        else if (a == "--no-stats") opts.no_stats = true;
        else if (a == "--debug") opts.debug = true;
        else if (a == "--report") opts.report_path = next();
        else if (a == "--restore-to") restore_to = next();
        else {
            if (a == "serve") continue;
            std::fprintf(stderr, "ERROR: unknown option: %s\n", a.c_str());
            dsvc::print_help();
            return 1;
        }
    }

    dsvc::EventBuffer events;
    dsvc::StateMirror state;
    dsvc::DaemonSession session(std::move(opts), &events, &state, restore_to);

    // Персистентность очереди: queue.json рядом с discovery-файлом.
    std::string disc = dsvc::discovery_path(discovery_override);
    session.set_persist_path(util::join_path(util::dir_name(disc), "queue.json"));

    std::string err;
    if (int rc = session.start(&err); rc != 0) {
        std::fprintf(stderr, "ERROR: init: %s\n", err.c_str());
        return rc;
    }

    // Восстановление очереди из queue.json (после start, до первого RPC).
    session.load_persisted(&err);
    if (!err.empty())
        std::fprintf(stderr, "WARNING: restore queue: %s\n", err.c_str());

    if (no_auth) {
        // Отладочный режим: авторизация выключена, токен не генерируется.
        // http_api разрешает все запросы при пустом токене.
        token.clear();
        std::fprintf(stderr, "WARNING: авторизация выключена (--no-auth), только для отладки!\n");
    } else if (token.empty()) {
        token = dsvc::gen_token();
    }
    session.set_token(token);

    httplib::Server svr;
    // Единичная привязка: либо фиксированный порт, либо авто (0 -> свободный).
    if (port > 0) {
        if (!svr.bind_to_port(bind, port)) {
            std::fprintf(stderr, "ERROR: could not bind %s:%d\n", bind.c_str(), port);
            return 1;
        }
    } else {
        port = svr.bind_to_any_port(bind);
    }
    dsvc::ApiContext ctx;
    ctx.daemon = &session;
    ctx.events = &events;
    ctx.state = &state;
    ctx.token = token;
    dsvc::mount(svr, ctx);

    session.set_on_shutdown([&] { svr.stop(); });

    // Discovery-файл (порт известен после bind).
    std::string pid = util::process_id();
    nlohmann::json dj = {{"port", port},
                         {"token", token},
                         {"pid", pid},
                         {"version", LLAO_VERSION}};
    bool disc_ok = dsvc::write_discovery(disc, dj);
    if (!disc_ok)
        std::fprintf(stderr, "WARNING: could not write discovery file %s\n", disc.c_str());
    std::fprintf(stdout, "LLAO server %s listening on %s:%d (pid %s)\n",
                  LLAO_VERSION, bind.c_str(), port, pid.c_str());
    if (no_auth)
        std::fprintf(stdout, "Auth: disabled (--no-auth)\n");
    else
        std::fprintf(stdout, "Token: %s\n", token.c_str());
    if (disc_ok) std::fprintf(stdout, "Discovery: %s\n", disc.c_str());
    if (bind == "0.0.0.0") {
        std::fprintf(stderr,
                     "WARNING: демон слушает на всех интерфейсах (0.0.0.0) — "
                     "доступен из сети. Убедитесь, что токен надёжен.\n");
    }
    std::fflush(stdout);

    // Блокирующий приём. svr.stop() (по RPC shutdown) прерывает listen и
    // возвращает управление. Сигнал (Ctrl+C) — запасной путь: прерывает
    // accept c EINTR; после возврата из listen выполняем graceful shutdown.
    svr.listen_after_bind();

    // Остановить приём; подождать активные файлы; финализировать.
    svr.stop();
    if (!disc.empty()) util::remove_file(disc);

    session.shutdown();
    std::fprintf(stdout, "LLAO server stopped\n");
    return 0;
}
