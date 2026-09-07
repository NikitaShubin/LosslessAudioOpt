#include "events.h"
#include <algorithm>

namespace dsvc {

void EventBuffer::push(const std::string& type, nlohmann::json payload) {
    if (payload.is_null()) payload = nlohmann::json::object();
    std::lock_guard<std::mutex> lk(m_);
    Event ev;
    ev.seq = next_seq_++;
    ev.type = type;
    ev.payload = std::move(payload);
    buf_.push_back(std::move(ev));
    last_seq_ = ev.seq;
    while (buf_.size() > kCapacity) buf_.pop_front();
}

void EventBuffer::push_for(size_t file_id, const std::string& type,
                           nlohmann::json payload) {
    if (payload.is_null()) payload = nlohmann::json::object();
    std::lock_guard<std::mutex> lk(m_);
    Event ev;
    ev.seq = next_seq_++;
    ev.type = type;
    ev.has_id = true;
    ev.file_id = file_id;
    ev.payload = std::move(payload);
    buf_.push_back(std::move(ev));
    last_seq_ = ev.seq;
    while (buf_.size() > kCapacity) buf_.pop_front();
}

EventBuffer::Poll EventBuffer::copy_since(uint64_t since) const {
    Poll p;
    std::lock_guard<std::mutex> lk(m_);
    p.last_seq = last_seq_;
    if (since < last_seq_ && (last_seq_ - since) > kCapacity) p.resync = true;
    for (const auto& ev : buf_) {
        if (ev.seq > since) p.events.push_back(ev);
    }
    return p;
}

uint64_t EventBuffer::last_seq() const {
    std::lock_guard<std::mutex> lk(m_);
    return last_seq_;
}

// --- StateMirror ---

void StateMirror::upsert(const Row& r) {
    std::lock_guard<std::mutex> lk(m_);
    if (removed_.count(r.id)) return;  // удалён — не воскрешать
    bool is_new = rows_.find(r.id) == rows_.end();
    rows_[r.id] = r;
    if (is_new) {
        if (std::find(order_.begin(), order_.end(), r.id) == order_.end())
            order_.push_back(r.id);
    }
}

void StateMirror::set_label(size_t id, const std::string& label) {
    std::lock_guard<std::mutex> lk(m_);
    if (removed_.count(id)) return;
    rows_[id].label = label;
    rows_[id].id = id;
    touch_locked(id);
}

void StateMirror::set_path(size_t id, const std::string& path) {
    std::lock_guard<std::mutex> lk(m_);
    if (removed_.count(id)) return;
    auto& r = rows_[id];
    r.id = id;
    r.path = path;
}

void StateMirror::set_state(size_t id, const std::string& st) {
    std::lock_guard<std::mutex> lk(m_);
    if (removed_.count(id)) return;
    rows_[id].state = st;
    rows_[id].id = id;
    touch_locked(id);
}

void StateMirror::set_tasks(size_t id, std::vector<std::string> tasks) {
    std::lock_guard<std::mutex> lk(m_);
    if (removed_.count(id)) return;
    auto& r = rows_[id];
    r.id = id;
    touch_locked(id);
    // Гонка task(Running) до set_tasks: другой воркер мог уже пометить
    // задачи после prep_done. Сохраняем известные состояния по индексам.
    for (size_t i = 0; i < tasks.size() && i < r.tasks.size(); i++) {
        if (r.tasks[i] != "pend") tasks[i] = r.tasks[i];
    }
    r.tasks = std::move(tasks);
}

void StateMirror::set_tasks(size_t id, const std::vector<obs::TaskInfo>& infos) {
    std::lock_guard<std::mutex> lk(m_);
    if (removed_.count(id)) return;
    auto& r = rows_[id];
    r.id = id;
    touch_locked(id);
    r.task_infos = infos;
    std::vector<std::string> tasks(infos.size(), "pend");
    for (size_t i = 0; i < tasks.size() && i < r.tasks.size(); i++) {
        if (r.tasks[i] != "pend") tasks[i] = r.tasks[i];
    }
    r.tasks = std::move(tasks);
}

void StateMirror::set_task(size_t id, size_t idx, const std::string& st) {
    std::lock_guard<std::mutex> lk(m_);
    if (removed_.count(id)) return;
    auto& r = rows_[id];
    r.id = id;
    touch_locked(id);
    if (idx >= r.tasks.size()) r.tasks.resize(idx + 1, "pend");
    r.tasks[idx] = st;
}

void StateMirror::set_pct(size_t id, double pct) {
    std::lock_guard<std::mutex> lk(m_);
    if (removed_.count(id)) return;
    auto& r = rows_[id];
    r.id = id;
    touch_locked(id);
    r.pct = pct;
}

void StateMirror::set_excluded(size_t id, const std::vector<std::string>& fmts) {
    std::lock_guard<std::mutex> lk(m_);
    if (removed_.count(id)) return;
    auto& r = rows_[id];
    r.id = id;
    touch_locked(id);
    r.excluded_fmts = fmts;
}

void StateMirror::set_meta(size_t id, const std::string& mode,
                           const std::string& target_dir) {
    std::lock_guard<std::mutex> lk(m_);
    if (removed_.count(id)) return;
    auto& r = rows_[id];
    r.id = id;
    touch_locked(id);
    r.mode = mode;
    r.target_dir = target_dir;
}

void StateMirror::set_out(size_t id, const std::string& path) {
    std::lock_guard<std::mutex> lk(m_);
    if (removed_.count(id)) return;
    auto& r = rows_[id];
    r.id = id;
    touch_locked(id);
    r.out_path = path;
}

void StateMirror::set_last_error(size_t id, const std::string& msg) {
    std::lock_guard<std::mutex> lk(m_);
    if (removed_.count(id)) return;
    auto& r = rows_[id];
    r.id = id;
    touch_locked(id);
    r.last_error = msg;
}

void StateMirror::set_sidecar_flags(size_t id, bool had_sidecar, bool has_sidecar) {
    std::lock_guard<std::mutex> lk(m_);
    if (removed_.count(id)) return;
    auto& r = rows_[id];
    r.id = id;
    touch_locked(id);
    r.had_sidecar = had_sidecar;
    r.has_sidecar = has_sidecar;
}

void StateMirror::remove(size_t id) {
    std::lock_guard<std::mutex> lk(m_);
    rows_.erase(id);
    order_.erase(std::remove(order_.begin(), order_.end(), id), order_.end());
    removed_.insert(id);
}

bool StateMirror::alive(size_t id) const {
    std::lock_guard<std::mutex> lk(m_);
    return removed_.count(id) == 0;
}

bool StateMirror::reorder(const std::vector<size_t>& order) {
    std::lock_guard<std::mutex> lk(m_);
    if (order.empty()) return false;
    std::set<size_t> seen;
    for (size_t id : order) {
        if (!seen.insert(id).second) return false;  // дубли
        if (rows_.find(id) == rows_.end()) return false;  // неизвестный id
    }
    // Перечисленные — первыми, остальные видимые — в прежнем порядке в хвост.
    std::vector<size_t> next;
    next.reserve(rows_.size());
    for (size_t id : order) next.push_back(id);
    if (!order_.empty() && order_.size() == rows_.size()) {
        for (size_t id : order_) {
            if (rows_.find(id) == rows_.end()) continue;
            if (seen.count(id)) continue;
            next.push_back(id);
        }
    } else {
        for (const auto& kv : rows_) {
            if (seen.count(kv.first)) continue;
            next.push_back(kv.first);
        }
    }
    order_ = std::move(next);
    return true;
}

std::vector<Row> StateMirror::snapshot() const {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<Row> out;
    out.reserve(rows_.size());
    if (!order_.empty() && order_.size() == rows_.size()) {
        for (size_t id : order_) {
            auto it = rows_.find(id);
            if (it != rows_.end()) out.push_back(it->second);
        }
    } else {
        for (const auto& kv : rows_) out.push_back(kv.second);
    }
    return out;
}

size_t StateMirror::size() const {
    std::lock_guard<std::mutex> lk(m_);
    return rows_.size();
}

}  // namespace dsvc
