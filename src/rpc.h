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

    // Пауза очереди (pause/resume): не запускать новые задачи, активные
    // продолжают. Следующий add/restart снимает остановку.
    virtual bool paused() const = 0;
    virtual void set_paused(bool paused) = 0;

    // Добавить пути (резолвятся на стороне демона). Заполняет result:
    // {"added":[{id,label}], "rejected":[{path,reason}]}.
    // mode: "optimize"|"restore"; target_dir: целевая папка (пусто = замена на месте).
    // Обход папок всегда рекурсивный (контракт v1).
    virtual void add(const std::vector<std::string>& paths,
                     const std::string& mode, const std::string& target_dir,
                     nlohmann::json& result) = 0;

    // Снять файл из очереди (cancel-file). true если id существовал.
    virtual bool cancel_file(uint64_t id) = 0;

    // Удалить файл из очереди (remove). Для pending/running — как cancel,
    // для завершённых (ok/stopped/error) — убрать строку из списка.
    virtual bool remove(uint64_t id) = 0;

    // Перезапустить файл (restart): активный сначала останавливается
    // (cancel + kill + ожидание завершения), затем строка заменяется новым
    // заданием. true если перезапущен.
    virtual bool restart(uint64_t id) = 0;

    // Массовое удаление файлов из очереди за один вызов (bulk-remove).
    // Возвращает число реально удалённых.
    virtual uint64_t bulk_remove(const std::vector<size_t>& ids) = 0;

    // Массовая остановка активных файлов за один вызов (bulk-cancel).
    // Возвращает число реально обработанных (активных) файлов.
    virtual uint64_t bulk_cancel(const std::vector<size_t>& ids) = 0;

    // Остановка ВСЕХ активных файлов очереди (cancel-all): снимает каждый
    // off-page/могущий устареть id сам, возвращает число остановленных.
    // Не зависит от переданного клиентом списка (поэтому работает всегда,
    // даже если вкладка отдала stale-состояние). Паузу ставит отдельно rpc.
    virtual uint64_t cancel_all_active() = 0;

    // Отсортировать очередь по полному пути файла (регистрозависимо).
    // Возвращает число файлов в очереди (или 0 при сбое).
    virtual size_t sort_by_path() = 0;

    // Удалить из очереди все успешно завершённые (state=="ok") файлы.
    // Возвращает число удалённых. skip/stopped/error не трогаются.
    virtual uint64_t clear_done() = 0;

    // Переупорядочить очередь (ids — новый порядок индексов файлов).
    virtual bool reorder(const std::vector<size_t>& order) = 0;

    // Запрос остановки демона (graceful|force).
    virtual void request_shutdown(bool force) = 0;

    // Список форматов из formats/*.json: [{id, extensions:[...]}].
    virtual nlohmann::json formats() const = 0;

    // Внутреннее состояние движка (отладка: prep_active, abort, флаги jobs).
    // По умолчанию пусто; реализуется DaemonSession.
    virtual nlohmann::json debug_state() { return nullptr; }
};

// Исполняет команду. cmd ("ping", "add", …), args — объект аргументов.
// Возвращает json-ответ: {"ok":true,"result":{...}} либо
// {"ok":false,"code":"...","error":"..."}.
nlohmann::json call(Daemon& d, const std::string& cmd, const nlohmann::json& args);

}  // namespace dsvc
