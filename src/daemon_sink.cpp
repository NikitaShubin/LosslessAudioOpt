#include "daemon_sink.h"

#include "persist.h"
#include "util.h"

namespace dsvc {

void DaemonSink::emit(size_t id, const std::string& type,
                      nlohmann::json payload) {
    // Публикация под мьютексом зеркала одной транзакцией с tombstone:
    // воркер, дорабатывающий снятую строку, не может вклиниться между
    // проверкой живости и push — событие не уйдёт после removed.
    st_->publish_if_alive(id, [&](size_t /*oid*/) {
        ev_->push_for(id, type, std::move(payload));
    });
}

void DaemonSink::begin_file(size_t id, const std::string& label,
                            const std::string& root) {
    emit(id, "begin_file", {{"label", label}, {"root", root}});
    Row r;
    r.id = id;
    r.label = label;
    r.root = root;
    r.state = "queued";
    st_->upsert(r);
}

void DaemonSink::prep(size_t id) {
    emit(id, "prep");
    st_->set_state(id, "prep");
    if (on_change_) on_change_();
}

void DaemonSink::set_tasks(size_t id, size_t total) {
    emit(id, "set_tasks", {{"total", total}});
    st_->set_tasks(id, std::vector<std::string>(total, "pend"));
}

void DaemonSink::set_tasks(size_t id, const std::vector<obs::TaskInfo>& infos) {
    nlohmann::json jinfos = nlohmann::json::array();
    for (auto& ti : infos) jinfos.push_back({{"fmt", ti.fmt_id}, {"variant", ti.variant_id}, {"params", ti.params}, {"note", ti.note}});
    emit(id, "set_tasks", {{"total", infos.size()}, {"task_infos", jinfos}});
    st_->set_tasks(id, infos);
}

void DaemonSink::set_excluded(size_t id, const std::vector<std::string>& fmts) {
    nlohmann::json jf = nlohmann::json::array();
    for (auto& f : fmts) jf.push_back(f);
    emit(id, "set_excluded", {{"excluded", jf}});
    st_->set_excluded(id, fmts);
}

void DaemonSink::task(size_t id, size_t idx, obs::TaskState st) {
    const char* s = st == obs::TaskState::Running ? "running"
                    : st == obs::TaskState::Ok     ? "ok"
                                                   : "failed";
    emit(id, "task", {{"idx", idx}, {"state", s}});
    st_->set_task(id, idx, s);
    // Старт варианта делает файл «в работе»: prep может быть долгим, и строка
    // должна перейти в running по первому реально запущенному варианту
    // (а держаться в prep до этого). Состояние же строки выставлять в ok/failed
    // тут нельзя — это прерогатива end_file/mark_stopped/error_file.
    if (st == obs::TaskState::Running) {
        emit(id, "state", {{"state", "running"}});
        st_->set_state(id, "running");
    }
}

void DaemonSink::end_file(size_t id, double pct) {
    emit(id, "end_file", {{"pct", pct}});
    st_->set_state(id, "ok");
    st_->set_pct(id, pct);
    if (on_change_) on_change_();
}

void DaemonSink::mark_stopped(size_t id) {
    emit(id, "stopped");
    st_->set_state(id, "stopped");
    if (on_change_) on_change_();
}

void DaemonSink::error_file(size_t id, const std::string& reason) {
    emit(id, "error_file", {{"reason", reason}});
    st_->set_state(id, "error");
    st_->set_last_error(id, reason);
    if (on_change_) on_change_();
}

void DaemonSink::mark_error(size_t id) {
    st_->set_state(id, "error");
    if (on_change_) on_change_();
}

void DaemonSink::log(const std::string& line) {
    ev_->push("log", {{"line", line}});
}

void DaemonSink::error(const std::string& line) {
    ev_->push("error", {{"line", line}});
}

void DaemonSink::files_added(const std::vector<size_t>& ids,
                             const std::vector<std::string>& labels) {
    for (size_t k = 0; k < ids.size(); k++)
        emit(ids[k], "added", {{"label", labels[k]}});
}

void DaemonSink::job_meta(size_t id, const std::string& mode,
                          const std::string& target_dir) {
    emit(id, "job_meta", {{"mode", mode}, {"target_dir", target_dir}});
    st_->set_meta(id, mode, target_dir);
}

void DaemonSink::out_file(size_t id, const std::string& path) {
    emit(id, "out_file", {{"path", path}});
    st_->set_out(id, path);
    // Флаг has_sidecar живёт по диску (тот же счёт, что в persist::snapshot):
    // рядом с итогом мог лежать доставленный sidecar <base>.tags.zip, и это
    // настолько же верно и для замены на месте (out_path — новый файл в том
    // же каталоге), и для целевой папки. had_sidecar не трогаем.
    st_->set_has_sidecar(id, util::file_exists(persist::sidecar_path_for(path)));
}

}  // namespace dsvc