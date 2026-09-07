#include "serve.h"
#include "serve_internal.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>

#include "http_api.h"
#include "contract.h"
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

namespace dsvc {

namespace {

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

int run_daemon(const std::vector<std::string>& args) {
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

}  // namespace dsvc
