#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "daemon_sink.h"
#include "events.h"
#include "obs.h"
#include "optimize.h"
#include "rpc.h"

// serve — командa `llao-daemon serve`: headless-движок с HTTP-API.
// DaemonSession реализует dsvc::Daemon (см. rpc.h) поверх optimize::Engine,
// пишет discovery-файл, обслуживает graceful shutdown.
namespace dsvc {

// Реализация решает, как трактовать paths (в v1 — только локальные для демона).
class DaemonSession : public Daemon {
public:
    // ev/st должны жить дольше сессии.
    DaemonSession(optimize::Options opts, EventBuffer* ev, StateMirror* st);
    ~DaemonSession() override;

    // Инициализация движка (загрузка конфигов, пул воркеров). err при сбое.
    int start(std::string* err);

    // Токен авторизации (выставляется serve после генерации).
    void set_token(const std::string& token) { token_ = token; }

    // Колбэк на запрос остановки (serve ставит svr.stop()).
    void set_on_shutdown(std::function<void()> cb) { on_shutdown_ = std::move(cb); }

    // --- dsvc::Daemon ---
    std::string version() const override;
    double uptime_s() const override;
    nlohmann::json session_options() const override;
    nlohmann::json counters() const override;
    bool paused() const override;
    void set_paused(bool paused) override;
    void add(const std::vector<std::string>& paths, bool recursive,
             nlohmann::json& result) override;
    bool cancel_file(uint64_t id) override;
    void request_shutdown(bool force) override;
    nlohmann::json formats() const override;

    // Graceful shutdown: остановить движок, подождать активные файлы,
    // записать отчёт, снять discovery (если задан). Вызывается после
    // завершения HTTP-сервера.
    void shutdown();

private:
    optimize::Options opts_;
    EventBuffer* ev_ = nullptr;
    StateMirror* st_ = nullptr;
    std::unique_ptr<optimize::Engine> engine_;
    std::unique_ptr<DaemonSink> sink_;
    double started_ = 0;                 // монотонные секунды (uptime)
    nlohmann::json formats_cache_;
    std::string token_;
    std::set<std::string> added_paths_;  // под mt_ — для дедупа add
    std::mutex mt_;
    std::atomic<bool> paused_{false};
    std::atomic<bool> shutting_down_{false};
    std::function<void()> on_shutdown_;
};

}  // namespace dsvc
