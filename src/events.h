#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "obs.h"

// daemon — инфраструктура headless-демона: кольцевой буфер событий и зеркало
// состояния очереди. Используется obs::Sink-реализацией демона (daemon_sink),
// диспетчером команд (rpc) и HTTP-обработчиками (http_api).
//
// Модуль платформонезависимый (только нити + nlohmann) — юнит-тестируется нативно.
namespace dsvc {

// Событие буфера: монотонный seq + тип + аргументы (JSON).
struct Event {
    uint64_t seq = 0;
    std::string type;
    nlohmann::json args = nlohmann::json::object();
};

// Кольцевой буфер событий с монотонным seq. Ёмкость фиксирована; при
// переполнении вытесняются самые старые события. Клиент, отставший дальше
// ёмкости, получает флаг resync и обязан перечитать /api/state.
class EventBuffer {
public:
    static constexpr size_t kCapacity = 10000;

    // Добавляет событие (потокобезопасно). seq присваивается автоматически.
    void push(const std::string& type, nlohmann::json args = nlohmann::json::object());

    // События с seq > since (не более kCapacity за раз), last_seq — последний
    // доступный seq (равен since, если новых нет). resync = true, если since
    // отстал дальше ёмкости буфера (часть событий утеряна).
    // Вызывающий не должен удерживать блокировку буфера.
    struct Poll {
        std::vector<Event> events;
        uint64_t last_seq = 0;
        bool resync = false;
    };
    Poll copy_since(uint64_t since) const;

    uint64_t last_seq() const;

private:
    mutable std::mutex m_;
    std::deque<Event> buf_;
    uint64_t next_seq_ = 1;
    uint64_t last_seq_ = 0;
};

// Строка очереди в зеркале состояния (без отрисовки — аналог Row из status.cpp).
struct Row {
    size_t id = 0;
    std::string label;
    std::string state;             // queued | prep | running | ok | skip | error
    double pct = 0;                // выигрыш в сжатии (после ok)
    std::vector<std::string> tasks; // состояния вариантов: pend|running|ok|failed
    std::vector<obs::TaskInfo> task_infos; // метаданные задач (fmt/variant)
};

// Потокобезопасное зеркало строк очереди. Обновляется obs::Sink-реализацией
// демона из событий движка; читается HTTP-обработчиком /api/state.
class StateMirror {
public:
    // Создать/обновить строку целиком (UPSERT по id).
    void upsert(const Row& r);
    void set_label(size_t id, const std::string& label);
    void set_state(size_t id, const std::string& st);
    void set_tasks(size_t id, std::vector<std::string> tasks);
    void set_tasks(size_t id, const std::vector<obs::TaskInfo>& infos);
    void set_task(size_t id, size_t idx, const std::string& st);
    void set_pct(size_t id, double pct);
    void remove(size_t id);
    void reorder(const std::vector<size_t>& order);

    std::vector<Row> snapshot() const;
    size_t size() const;

private:
    mutable std::mutex m_;
    std::map<size_t, Row> rows_;
    std::vector<size_t> order_;  // порядок очереди (ids); пусто = по id
    // Tombstones удалённых строк: запоздалые события движка (воркер
    // дорабатывает снятый файл) не должны воскрешать строку-призрака.
    // Id файлов монотонны и не переиспользуются, tombstone вечен.
    std::set<size_t> removed_;
};

}  // namespace dsvc
