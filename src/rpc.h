#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// rpc — диспетчер команд управления демоном (§6.1 REFACTOR_TODO.md).
// Платформонезависимый (nlohmann + ссылки на интерфейс Daemon): юнит-тестируется
// нативно. Разбор/исполнение команд `POST /rpc` и поддержка `GET /api/formats`.
namespace dsvc {

struct EngineFile;

// Граница между диспетчером команд (rpc) и реализацией демона (serve.cpp).
// Позволяет юнит-тестировать rpc с фейковой реализацией.
struct Daemon {
    virtual ~Daemon() = default;

    virtual std::string version() const = 0;
    virtual double uptime_s() const = 0;

    // Параметры сессии (jobs/verify/dry_run/…): для ping/state.
    virtual nlohmann::json session_options() const = 0;
    // Счётчики {total, done, failed}.
    virtual nlohmann::json counters() const = 0;

    // Остановка всей очереди (cancel-all): не запускать новые задачи.
    // next add снимает остановку.
    virtual bool paused() const = 0;
    virtual void set_paused(bool paused) = 0;

    // Добавить пути (резолвятся на стороне демона). Заполняет result:
    // {"added":[{id,label}], "rejected":[{path,reason}]}.
    virtual void add(const std::vector<std::string>& paths, bool recursive,
                     nlohmann::json& result) = 0;

    // Снять файл из очереди (cancel-file). true если id существовал.
    virtual bool cancel_file(uint64_t id) = 0;

    // Запрос остановки демона (graceful|force).
    virtual void request_shutdown(bool force) = 0;

    // Список форматов из formats/*.json: [{id, extensions:[...]}].
    virtual nlohmann::json formats() const = 0;
};

// Исполняет команду. cmd ("ping", "add", …), args — объект аргументов.
// Возвращает json-ответ: {"ok":true,"result":{...}} либо
// {"ok":false,"code":"...","error":"..."}.
nlohmann::json call(Daemon& d, const std::string& cmd, const nlohmann::json& args);

}  // namespace dsvc
