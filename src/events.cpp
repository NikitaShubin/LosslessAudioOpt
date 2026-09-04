#include "events.h"
#include <algorithm>

namespace dsvc {

void EventBuffer::push(const std::string& type, nlohmann::json args) {
    if (args.is_null()) args = nlohmann::json::object();
    std::lock_guard<std::mutex> lk(m_);
    Event ev;
    ev.seq = next_seq_++;
    ev.type = type;
    ev.args = std::move(args);
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
}

void StateMirror::set_state(size_t id, const std::string& st) {
    std::lock_guard<std::mutex> lk(m_);
    if (removed_.count(id)) return;
    rows_[id].state = st;
}

void StateMirror::set_tasks(size_t id, std::vector<std::string> tasks) {
    std::lock_guard<std::mutex> lk(m_);
    if (removed_.count(id)) return;
    auto& r = rows_[id];
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
    if (idx >= r.tasks.size()) r.tasks.resize(idx + 1, "pend");
    r.tasks[idx] = st;
}

void StateMirror::set_pct(size_t id, double pct) {
    std::lock_guard<std::mutex> lk(m_);
    if (removed_.count(id)) return;
    rows_[id].pct = pct;
}

void StateMirror::remove(size_t id) {
    std::lock_guard<std::mutex> lk(m_);
    rows_.erase(id);
    order_.erase(std::remove(order_.begin(), order_.end(), id), order_.end());
    removed_.insert(id);
}

void StateMirror::reorder(const std::vector<size_t>& order) {
    std::lock_guard<std::mutex> lk(m_);
    if (order.size() != rows_.size()) return;
    // Проверка что все ids присутствуют
    for (size_t id : order) if (rows_.find(id) == rows_.end()) return;
    order_ = order;
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
