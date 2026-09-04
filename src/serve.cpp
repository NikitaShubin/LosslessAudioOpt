#include "serve.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <httplib.h>

#include "config.h"
#include "report.h"
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

optimize::Verify parse_verify(const std::string& s, bool* ok) {
    if (ok) *ok = true;
    if (s == "all") return optimize::Verify::All;
    if (s == "winner") return optimize::Verify::Winner;
    if (s == "none") return optimize::Verify::None;
    if (ok) *ok = false;
    return optimize::Verify::All;
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
        "LLAO daemon — headless версия llao с HTTP-API и веб-UI\n"
        "\n"
        "Использование: llao-daemon serve [опции]\n"
        "\n"
        "Опции сессии (дефолты оптимизации):\n"
        "  --jobs N|M.M          число потоков или множитель ядер (default 2.0)\n"
        "  --verify MODE         all|winner|none (default all)\n"
        "  --dry-run             не записывать результат\n"
        "  --no-stats            не накапливать stats.json\n"
        "  --debug               журнал runs/*.jsonl + отладка\n"
        "  --report PATH         итоговый отчёт при shutdown\n"
        "\n"
        "Сервер:\n"
        "  --bind ADDR           адрес прослушивания (default 127.0.0.1)\n"
        "  --port N              порт; 0 = свободный (default 0)\n"
        "  --token HEX           явный токен (по умолчанию генерируется)\n"
        "  --discovery PATH      путь к discovery-файлу (default: %%LOCALAPPDATA%%/llao/daemon.json)\n"
        "\n"
        "  --help                эта справка\n");
}

}  // namespace

// --- DaemonSession ---

DaemonSession::DaemonSession(optimize::Options opts, EventBuffer* ev, StateMirror* st)
    : opts_(std::move(opts)), ev_(ev), st_(st) {
    started_ = monotonic_s();
}

DaemonSession::~DaemonSession() { shutdown(); }

int DaemonSession::start(std::string* err) {
    try {
        auto fmts = config::load_all();
        formats_cache_ = nlohmann::json::array();
        for (const auto& f : fmts) {
            if (!f.enabled) continue;
            formats_cache_.push_back(
                {{"id", f.id},
                 {"extensions", nlohmann::json::array({f.extension})}});
        }
    } catch (const std::exception& exc) {
        if (err) *err = exc.what();
        return 1;
    }

    sink_ = std::make_unique<DaemonSink>(ev_, st_);
    obs::set_sink(sink_.get());

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
            {"verify", opts_.verify == optimize::Verify::All ? "all"
                         : opts_.verify == optimize::Verify::Winner ? "winner"
                                                                   : "none"},
            {"ignore_errors", true}};
}

nlohmann::json DaemonSession::counters() const {
    int total = 0, done = 0, failed = 0;
    for (const auto& r : st_->snapshot()) {
        total++;
        if (r.state == "ok" || r.state == "skip" || r.state == "error") done++;
        if (r.state == "error") failed++;
    }
    return {{"total", total}, {"done", done}, {"failed", failed}};
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

void DaemonSession::add(const std::vector<std::string>& paths, bool /*recursive*/,
                        nlohmann::json& result) {
    std::vector<std::string> accepted;
    {
        std::lock_guard<std::mutex> lk(mt_);
        for (const auto& p : paths) {
            if (p.empty()) {
                result["rejected"].push_back({{"path", p}, {"reason", "empty path"}});
                continue;
            }
            bool is_dir = util::dir_exists(p);
            if (!is_dir && !util::file_exists(p)) {
                result["rejected"].push_back(
                    {{"path", p},
                     {"reason", "path not found on daemon host (use --upload in client)"}});
                continue;
            }
            if (added_paths_.count(p)) {
                // Если файл был удалён/завершён — разрешить повторное добавление
                bool still_active = false;
                if (engine_) {
                    auto snap = engine_->snapshot();
                    for (const auto& f : snap) if (f.path == p) {
                        if (f.state == "queued" || f.state == "prep" || f.state == "running" || f.state == "removed") still_active = true;
                        break;
                    }
                }
                if (still_active) {
                    result["rejected"].push_back({{"path", p}, {"reason", "already in queue"}});
                    continue;
                } else {
                    added_paths_.erase(p);
                }
            }
            added_paths_.insert(p);
            accepted.push_back(p);
        }
        // запомнить, нужно ли снять паузу — вне мьютекса вызовем set_paused
    }
    bool need_resume = !accepted.empty() && paused_.load();
    if (need_resume) set_paused(false);

    size_t before = st_->size();
    if (!accepted.empty()) engine_->add(accepted);
    size_t after = st_->size();

    // Добавленные строки — «хвост» зеркала (id отсортированы по возрастанию).
    auto rows = st_->snapshot();
    for (size_t i = before; i < after && i < rows.size(); i++)
        result["added"].push_back({{"id", rows[i].id}, {"label", rows[i].label}});
}

bool DaemonSession::cancel_file(uint64_t id) {
    return engine_ && engine_->remove(id);
}

bool DaemonSession::remove(uint64_t id) {
    if (!engine_) return false;
    std::string path_to_remove;
    auto snap_before = engine_->snapshot();
    for (const auto& f : snap_before) if (f.idx == (size_t)id) { path_to_remove = f.path; break; }
    bool ok = engine_->remove((size_t)id);
    bool is_done = false;
    for (const auto& f : snap_before) if (f.idx == (size_t)id) {
        is_done = (f.state == "ok" || f.state == "skip" || f.state == "error" || f.state == "removed");
        break;
    }
    if (!ok && is_done) ok = true;
    if (ok) {
        st_->remove((size_t)id);
        if (!path_to_remove.empty()) {
            std::lock_guard<std::mutex> lk(mt_);
            added_paths_.erase(path_to_remove);
        }
        ev_->push("removed", {{"id", id}});
    }
    return ok;
}

bool DaemonSession::reorder(const std::vector<size_t>& order) {
    if (!engine_) return false;
    bool ok = engine_->reorder(order);
    if (ok) {
        st_->reorder(order);
        ev_->push("reordered", {{"order", order}});
    }
    return ok;
}

void DaemonSession::request_shutdown(bool /*force*/) {
    if (on_shutdown_) on_shutdown_();
}

nlohmann::json DaemonSession::formats() const {
    return {{"formats", formats_cache_}};
}

void DaemonSession::shutdown() {
    if (shutting_down_.exchange(true)) return;
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

    obs::set_sink(nullptr);
}

}  // namespace dsvc

// --- entry ---

int main(int argc, char** argv) {
    dsvc::install_signal_handlers();

    std::string bind = "127.0.0.1";
    int port = 0;
    std::string token;
    std::string discovery_override;
    optimize::Options opts;
    opts.mode = optimize::SessionMode::Daemon;
    opts.no_status = true;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--help" || a == "-h") {
            dsvc::print_help();
            return 0;
        } else if (a == "--bind") bind = next();
        else if (a == "--port") port = std::atoi(next().c_str());
        else if (a == "--token") token = next();
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
        else if (a == "--no-stats") opts.no_stats = true;
        else if (a == "--debug") opts.debug = true;
        else if (a == "--report") opts.report_path = next();
        else {
            if (a == "serve") continue;
            std::fprintf(stderr, "ERROR: unknown option: %s\n", a.c_str());
            dsvc::print_help();
            return 1;
        }
    }

    dsvc::EventBuffer events;
    dsvc::StateMirror state;
    dsvc::DaemonSession session(std::move(opts), &events, &state);

    std::string err;
    if (int rc = session.start(&err); rc != 0) {
        std::fprintf(stderr, "ERROR: init: %s\n", err.c_str());
        return rc;
    }

    if (token.empty()) token = dsvc::gen_token();
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
    std::string disc = dsvc::discovery_path(discovery_override);
    std::string pid = util::process_id();
    nlohmann::json dj = {{"port", port},
                         {"token", token},
                         {"pid", pid},
                         {"version", LLAO_VERSION}};
    bool disc_ok = dsvc::write_discovery(disc, dj);
    if (!disc_ok)
        std::fprintf(stderr, "WARNING: could not write discovery file %s\n", disc.c_str());
    std::fprintf(stdout, "LLAO daemon %s listening on %s:%d (pid %s)\n",
                  LLAO_VERSION, bind.c_str(), port, pid.c_str());
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
    std::fprintf(stdout, "LLAO daemon stopped\n");
    return 0;
}
