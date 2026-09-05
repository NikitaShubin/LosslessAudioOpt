#pragma once
#include <cstddef>
#include <vector>

#include "events.h"
#include "obs.h"

// Реализация obs::Sink для headless-демона: транслирует события движка в
// кольцевой буфер (EventBuffer) и зеркало состояния очереди (StateMirror).
// Потокобезопасна (буфер и зеркало сами мутексируются удобно движком).
namespace dsvc {

class DaemonSink final : public obs::Sink {
public:
    DaemonSink(EventBuffer* ev, StateMirror* st) : ev_(ev), st_(st) {}

    void init_session(size_t total) override;
    void begin_file(size_t id, const std::string& label) override;
    void prep(size_t id) override;
    void set_tasks(size_t id, size_t total) override;
    void set_tasks(size_t id, const std::vector<obs::TaskInfo>& infos) override;
    void set_excluded(size_t id, const std::vector<std::string>& fmts) override;
    void task(size_t id, size_t idx, obs::TaskState st) override;
    void end_file(size_t id, double pct) override;
    void mark_stopped(size_t id) override;
    void mark_error(size_t id) override;
    void log(const std::string& line) override;
    void error(const std::string& line) override;
    void files_added(const std::vector<size_t>& ids,
                     const std::vector<std::string>& labels) override;

private:
    EventBuffer* ev_ = nullptr;
    StateMirror* st_ = nullptr;
};

}  // namespace dsvc
