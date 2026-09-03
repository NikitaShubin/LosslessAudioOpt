#include "obs.h"
#include "status.h"

// StatusSink: адаптер obs::Sink -> status::* (псевдографический статусбар).
// Используется монолитом llao.exe (одноразовый прогон с интерактивным UI).
// Все методы делегируют в status:: с сохранением поведения.

class StatusSink final : public obs::Sink {
public:
    void begin_file(size_t idx, const std::string& label) override {
        status::begin_file(idx, label);
    }
    void prep(size_t idx) override { status::prep(idx); }
    void set_tasks(size_t idx, size_t total) override { status::set_tasks(idx, total); }
    void task(size_t idx, size_t task_idx, obs::TaskState st) override {
        status::task(idx, task_idx, static_cast<status::TaskState>(st));
    }
    void end_file(size_t idx, double pct) override { status::end_file(idx, pct); }
    void mark_skip(size_t idx) override { status::mark_skip(idx); }
    void mark_error(size_t idx) override { status::mark_error(idx); }
    void log(const std::string& line) override { status::log(line); }
    void error(const std::string& line) override { status::error(line); }
};

// Устанавливает глобальный StatusSink. Вызывается llao.exe до старта воркеров.
void install_status_sink();
