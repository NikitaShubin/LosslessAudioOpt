#include "daemon_sink.h"

namespace daemon {

void DaemonSink::init_session(size_t total) {
    ev_->push("session", {{"total", (size_t)total}});
}

void DaemonSink::begin_file(size_t id, const std::string& label) {
    ev_->push("begin_file", {{"id", id}, {"label", label}});
    Row r;
    r.id = id;
    r.label = label;
    r.state = "queued";
    st_->upsert(r);
}

void DaemonSink::prep(size_t id) {
    ev_->push("prep", {{"id", id}});
    st_->set_state(id, "prep");
}

void DaemonSink::set_tasks(size_t id, size_t total) {
    ev_->push("set_tasks", {{"id", id}, {"total", (size_t)total}});
    st_->set_tasks(id, std::vector<std::string>(total, "pend"));
}

void DaemonSink::task(size_t id, size_t idx, obs::TaskState st) {
    const char* s = st == obs::TaskState::Running ? "running"
                    : st == obs::TaskState::Ok     ? "ok"
                                                   : "failed";
    ev_->push("task", {{"id", id}, {"idx", idx}, {"state", s}});
    st_->set_task(id, idx, s);
}

void DaemonSink::end_file(size_t id, double pct) {
    ev_->push("end_file", {{"id", id}, {"pct", pct}});
    st_->set_state(id, "ok");
    st_->set_pct(id, pct);
}

void DaemonSink::mark_skip(size_t id) {
    ev_->push("skip", {{"id", id}});
    st_->set_state(id, "skip");
}

void DaemonSink::mark_error(size_t id) {
    ev_->push("error", {{"id", id}, {"line", ""}});
    st_->set_state(id, "error");
}

void DaemonSink::log(const std::string& line) {
    ev_->push("log", {{"line", line}});
}

void DaemonSink::error(const std::string& line) {
    ev_->push("error", {{"id", nlohmann::json(nullptr)}, {"line", line}});
}

void DaemonSink::files_added(const std::vector<size_t>& ids,
                             const std::vector<std::string>& labels) {
    for (size_t k = 0; k < ids.size(); k++)
        ev_->push("added", {{"id", ids[k]}, {"label", labels[k]}});
}

}  // namespace daemon
