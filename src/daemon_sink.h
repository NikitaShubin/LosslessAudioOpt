#pragma once
#include <cstddef>
#include <functional>
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

    // Колбэк на изменение состояния файла (для инкрементной персистентности).
    // Вызывается из prep/end_file/mark_stopped/error_file/mark_error после
    // обновления зеркала, без блокировок события; реализация должна быть
    // потокобезопасна (движок зовёт из воркеров).
    void set_on_change(std::function<void()> cb) { on_change_ = std::move(cb); }

    void begin_file(size_t id, const std::string& label) override;
    void prep(size_t id) override;
    void set_tasks(size_t id, size_t total) override;
    void set_tasks(size_t id, const std::vector<obs::TaskInfo>& infos) override;
    void set_excluded(size_t id, const std::vector<std::string>& fmts) override;
    void task(size_t id, size_t idx, obs::TaskState st) override;
    void end_file(size_t id, double pct) override;
    void mark_stopped(size_t id) override;
    // Единая модель ошибки файла: одно файл-событие с причиной (см. optimize).
    void error_file(size_t id, const std::string& reason) override;
    // Запасной путь: состояние error без отдельного события (если какой-то
    // путь движка ещё вызывает mark_error без error_file).
    void mark_error(size_t id) override;
    void log(const std::string& line) override;
    void error(const std::string& line) override;
    void files_added(const std::vector<size_t>& ids,
                     const std::vector<std::string>& labels) override;
    void job_meta(size_t id, const std::string& mode,
                  const std::string& target_dir) override;
    void out_file(size_t id, const std::string& path) override;

private:
    // Публикует файловое событие, если строка ещё жива. Строки, снятые
    // remove/bulk-remove, томбируют в зеркале; запоздалые события их воркера
    // (воркер дорабатывает снятый кандидат) не должны попадать в буфер —
    // клиенту не приходят «призрачные» task/state/end_file по удалённой строке.
    void emit(size_t id, const std::string& type,
              nlohmann::json payload = nlohmann::json::object());

    EventBuffer* ev_ = nullptr;
    StateMirror* st_ = nullptr;
    std::function<void()> on_change_;
};

}  // namespace dsvc
