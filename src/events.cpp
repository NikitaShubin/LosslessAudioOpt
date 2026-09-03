#include "events.h"

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
    rows_[r.id] = r;
}

void StateMirror::set_label(size_t id, const std::string& label) {
    std::lock_guard<std::mutex> lk(m_);
    rows_[id].label = label;
}

void StateMirror::set_state(size_t id, const std::string& st) {
    std::lock_guard<std::mutex> lk(m_);
    rows_[id].state = st;
}

void StateMirror::set_tasks(size_t id, std::vector<std::string> tasks) {
    std::lock_guard<std::mutex> lk(m_);
    rows_[id].tasks = std::move(tasks);
}

void StateMirror::set_task(size_t id, size_t idx, const std::string& st) {
    std::lock_guard<std::mutex> lk(m_);
    auto& r = rows_[id];
    if (idx >= r.tasks.size()) r.tasks.resize(idx + 1, "pend");
    r.tasks[idx] = st;
}

void StateMirror::set_pct(size_t id, double pct) {
    std::lock_guard<std::mutex> lk(m_);
    rows_[id].pct = pct;
}

std::vector<Row> StateMirror::snapshot() const {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<Row> out;
    out.reserve(rows_.size());
    for (const auto& kv : rows_) out.push_back(kv.second);
    return out;
}

size_t StateMirror::size() const {
    std::lock_guard<std::mutex> lk(m_);
    return rows_.size();
}

}  // namespace dsvc
