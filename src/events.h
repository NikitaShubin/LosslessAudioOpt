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
//
// Каноническая модель события (Фаза B): файловые события создаются через
// push_for(id, ...) и несут has_id/file_id; payload содержит остальные поля.
// Наружу /api/events раскладывает их инлайново: {type, seq, id?, ...payload}.
//
// Единственная модель строки очереди демона — Row (см. ниже); снимок движка
// (optimize::EngineFile) остаётся отдельным слоем и в демон не попадает.
namespace dsvc {

// Каноническое событие буфера: монотонный seq + тип + привязка к строке
// очереди (file_id, опционально) + полезная нагрузка (JSON).
struct Event {
    uint64_t seq = 0;
    std::string type;
    bool has_id = false;        // событие привязано к строке очереди
    size_t file_id = 0;         // стабильный id строки (используется при has_id)
    nlohmann::json payload = nlohmann::json::object();
};

// Кольцевой буфер событий с монотонным seq. Ёмкость фиксирована; при
// переполнении вытесняются самые старые события. Клиент, отставший дальше
// ёмкости, получает флаг resync и обязан перечитать /api/state.
class EventBuffer {
public:
    static constexpr size_t kCapacity = 10000;

    // Добавляет глобальное событие (не привязано к строке очереди;
    // потокобезопасно). seq присваивается автоматически.
    void push(const std::string& type, nlohmann::json payload = nlohmann::json::object());

    // Добавляет событие, привязанное к строке очереди с id file_id.
    // file_id НЕ копируется в payload — наружу выставляется http_api через
    // has_id (поле "id" в JSON). Так гарантируется единая схема file-событий.
    void push_for(size_t file_id, const std::string& type,
                  nlohmann::json payload = nlohmann::json::object());

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

// Строка очереди в зеркале состояния (без отрисовки).
struct Row {
    size_t id = 0;
    std::string label;
    std::string root;            // корень добавления (абсолют); пусто у старых строк
    std::string path;            // полный путь исходника (для персистентности;
                                 // у строк движка может быть пусто — берётся из
                                 // engine::snapshot; у восстановленных строк
                                 // label сам = полному пути)
    std::string state;             // queued | prep | running | ok | stopped | error
    double pct = 0;                // выигрыш в сжатии (после ok)
    std::vector<std::string> tasks; // состояния вариантов: pend|running|ok|failed
    std::vector<obs::TaskInfo> task_infos; // метаданные задач (fmt/variant)
    std::vector<std::string> excluded_fmts; // форматы, исключённые по caps (жёлтые точки)
    std::string mode;              // "optimize" | "restore"
    std::string target_dir;        // целевая папка (пусто = замена на месте)
    std::string out_path;          // фактический путь результата (после ok)
    std::string last_error;        // текст последней ошибки (error/stopped)
    bool had_sidecar = false;      // на входе был sidecar <base>.tags.zip
    bool has_sidecar = false;      // рядом с итогом есть sidecar
};

// Потокобезопасное зеркало строк очереди. Обновляется obs::Sink-реализацией
// демона из событий движка; читается HTTP-обработчиком /api/state.
class StateMirror {
public:
    // Создать/обновить строку целиком (UPSERT по id).
    void upsert(const Row& r);
    void set_label(size_t id, const std::string& label);
    void set_path(size_t id, const std::string& path);
    void set_root(size_t id, const std::string& root);
    void set_state(size_t id, const std::string& st);
    void set_tasks(size_t id, std::vector<std::string> tasks);
    void set_tasks(size_t id, const std::vector<obs::TaskInfo>& infos);
    void set_task(size_t id, size_t idx, const std::string& st);
    void set_pct(size_t id, double pct);
    void set_excluded(size_t id, const std::vector<std::string>& fmts);
    // Метаданные строки: режим и целевая папка (при добавлении).
    void set_meta(size_t id, const std::string& mode, const std::string& target_dir);
    // Фактический путь результата (после ок, restore/замена на месте).
    void set_out(size_t id, const std::string& path);
    // Текст последней ошибки (error/stopped).
    void set_last_error(size_t id, const std::string& msg);
    // Флаги sidecar: на входе (had_sidecar) / после результата (has_sidecar).
    void set_sidecar_flags(size_t id, bool had_sidecar, bool has_sidecar);
    // Обновить только has_sidecar (например, при out_file: рядом с итогом факт
    // sidecar известен по диску, не трогая had_sidecar).
    void set_has_sidecar(size_t id, bool has_sidecar);
    void remove(size_t id);
    // Жива ли строка (не удалена). Для подавления запоздалых событий воркера
    // по снятой строке: зеркало уже защищено tombstone, а буфер событий — нет.
    bool alive(size_t id) const;

    // Атомарно с remove(): вызывает fn(id) только если строка не удалена.
    // Исключает гонку «проверили живость → сервер томбинит → событие уходит
    // после removed» (см. DaemonSink::emit). fn — push_for в текущий буфер.
    template <typename Fn>
    void publish_if_alive(size_t id, Fn&& fn) {
        std::lock_guard<std::mutex> lk(m_);
        if (removed_.count(id)) return;
        fn(id);
    }
    // Переупорядочить видимые строки. Допускается подмножество id:
    // перечисленные встают первыми, остальные сохраняют порядок в хвосте.
    // Tombstones и неизвестные id игнорируются. Возвращает false, если
    // порядок пуст или содержит дубли.
    bool reorder(const std::vector<size_t>& order);

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

    // Инвариант: order_ содержит ровно id из rows_ (для snapshot).
    // Вызывать под m_ после создания строки через rows_[id].
    void touch_locked(size_t id) {
        if (std::find(order_.begin(), order_.end(), id) == order_.end())
            order_.push_back(id);
    }
};

}  // namespace dsvc
