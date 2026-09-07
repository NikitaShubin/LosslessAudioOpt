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
#include "persist.h"
#include "rpc.h"

// serve — командa `llao serve`: headless-движок с HTTP-API.
// DaemonSession реализует dsvc::Daemon (см. rpc.h) поверх optimize::Engine,
// пишет discovery-файл, обслуживает graceful shutdown.
namespace dsvc {

// Единая точка входа `llao serve`: парсит опции из args (args[0] — имя
// программы; токен "serve" в любом месте argv пропускается), поднимает
// HTTP-сервер с веб-UI и обслуживает его до graceful shutdown.
int run_daemon(const std::vector<std::string>& args);

// Реализация решает, как трактовать paths (в v1 — только локальные для демона).
class DaemonSession : public Daemon {
public:
    // ev/st должны жить дольше сессии. restore_to — целевой формат для режима
    // восстановления (id из formats/*.json; дефолт "flac" см. serve.cpp main).
    DaemonSession(optimize::Options opts, EventBuffer* ev, StateMirror* st,
                  std::string restore_to);
    ~DaemonSession() override;

    // Инициализация движка (загрузка конфигов, пул воркеров). err при сбое.
    int start(std::string* err);

    // Токен авторизации (выставляется serve после генерации).
    void set_token(const std::string& token) { token_ = token; }

    // Колбэк на запрос остановки (serve ставит svr.stop()).
    void set_on_shutdown(std::function<void()> cb) { on_shutdown_ = std::move(cb); }

    // Персистентность очереди. Путь к queue.json (рядом с discovery-файлом).
    // Должен быть выставлен ДО start(), иначе инкрементальная запись и
    // финальный persist при shutdown не сработают (только если path пуст —
    // персистентность выключена).
    void set_persist_path(const std::string& path) { persist_path_ = path; }

    // Восстанавливает очередь из persist_path_ после start(). Распределяет
    // строки по правилам модели (см. serve.cpp): ok — зеркало, queued —
    // движок с автозапуском, stopped/error — зеркало, prep/running — зеркало
    // как stopped. Строки, не прошедшие проверку файлов, остаются в зеркале
    // (stopped + last_error), но в движок не заносятся. err при ошибке.
    void load_persisted(std::string* err);

    // --- dsvc::Daemon ---
    std::string version() const override;
    double uptime_s() const override;
    nlohmann::json session_options() const override;
    nlohmann::json counters() const override;
    bool paused() const override;
    void set_paused(bool paused) override;
    void add(const std::vector<std::string>& paths,
             const std::string& mode, const std::string& target_dir,
             nlohmann::json& result) override;
    bool cancel_file(uint64_t id) override;
    bool remove(uint64_t id) override;
    bool restart(uint64_t id) override;
    uint64_t bulk_remove(const std::vector<size_t>& ids) override;
    uint64_t bulk_cancel(const std::vector<size_t>& ids) override;
    uint64_t cancel_all_active() override;
    size_t sort_by_path() override;
    uint64_t clear_done() override;
    bool reorder(const std::vector<size_t>& order) override;
    void request_shutdown(bool force) override;
    nlohmann::json formats() const override;
    nlohmann::json debug_state() override;

    // Graceful shutdown: остановить движок, подождать активные файлы,
    // записать отчёт, снять discovery (если задан). Вызывается после
    // завершения HTTP-сервера.
    void shutdown();

private:
    // Ядро операций под mt_: движок — единственный источник истины очереди,
    // mt_ сериализует check-then-act между параллельными RPC (два таба,
    // cli+web). Порядок блокировок везде mt_ -> qm движка -> мьютексы
    // зеркала/событий; обратного вложения нет.
    void add_locked(const std::vector<std::string>& paths,
                    const std::string& mode, const std::string& target_dir,
                    nlohmann::json& result, std::vector<size_t>& new_ids);
    bool remove_locked(uint64_t id);

    // Инкрементная персистентность: снять снапшоты зеркала и движка
    // ОТДЕЛЬНО (без вложенных блокировок — см. деадлок-осторожность) и
    // записать queue.json атомарно. Вызывается из мутаций сессии и из
    // obs::Sink-колбэков (on_mutation). final=true — финальный сброс при
    // shutdown: активные (queued|prep|running) -> queued, очередь достраивается
    // зеркальными строками за пределами движка.
    void persist(bool final);
    // Пометить строку (по id) как имевшую sidecar на входе (add).
    void set_had_sidecar(size_t id, const std::string& full_path);
    // Собрать список persist-строк из текущих снапшотов. Вызывается под persist_m_.
    std::vector<persist::Row> snapshot_for_persist(bool final) const;
    // Записать снапшот в persist_path_ (lenient: при пустом пути — no-op).
    void persist_rows(const std::vector<persist::Row>& rows);

    optimize::Options opts_;
    std::string restore_to_;  // целевой формат для restore (id из formats/*.json)
    std::string persist_path_;  // путь к queue.json (пусто = персистентность выкл)
    EventBuffer* ev_ = nullptr;
    StateMirror* st_ = nullptr;
    std::unique_ptr<optimize::Engine> engine_;
    std::unique_ptr<DaemonSink> sink_;
    double started_ = 0;                 // монотонные секунды (uptime)
    nlohmann::json formats_cache_;
    std::string token_;
    std::set<std::string> added_paths_;  // под mt_ — для дедупа add
    std::mutex mt_;
    std::mutex persist_m_;               // сериализация записи queue.json
    std::atomic<bool> paused_{false};
    std::atomic<bool> shutting_down_{false};
    std::function<void()> on_shutdown_;
};

}  // namespace dsvc
