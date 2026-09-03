// Юнит-тесты фазы 1 (платформонезависимые): EventBuffer, StateMirror, DaemonSink,
// rpc::call. Собираются/запускаются нативно: make test-daemon-core (см. Makefile).
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "daemon_sink.h"
#include "rpc.h"

using dsvc::EventBuffer;
using dsvc::StateMirror;
using dsvc::Row;
using dsvc::DaemonSink;
using dsvc::Daemon;

static int failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::cerr << "FAIL " << __LINE__ << ": " #cond "\n";          \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

// --- EventBuffer ---

static void test_event_buffer() {
    EventBuffer ev;
    CHECK(ev.last_seq() == 0);
    ev.push("begin_file", {{"id", 0}, {"label", "a.flac"}});
    ev.push("prep", {{"id", 0}});
    CHECK(ev.last_seq() == 2);

    auto p = ev.copy_since(0);
    CHECK(!p.resync);
    CHECK(p.events.size() == 2);
    CHECK(p.events[0].seq == 1);
    CHECK(p.events[0].type == "begin_file");
    CHECK(p.events[0].args["id"] == 0);
    CHECK(p.last_seq == 2);

    // since=1 -> одно событие (seq 2)
    auto p2 = ev.copy_since(1);
    CHECK(p2.events.size() == 1);
    CHECK(p2.events[0].seq == 2);

    // since равен последнему -> пусто
    auto p3 = ev.copy_since(2);
    CHECK(p3.events.empty());
    CHECK(p3.last_seq == 2);

    // переполнение: resync для клиента, отставшего дальше ёмкости
    EventBuffer big;
    for (size_t i = 0; i < EventBuffer::kCapacity + 50; i++) big.push("x");
    auto overflow = big.copy_since(0);
    CHECK(overflow.resync);
    CHECK(overflow.events.size() == (size_t)EventBuffer::kCapacity);
    CHECK(overflow.last_seq == (uint64_t)(EventBuffer::kCapacity + 50));
}

// --- StateMirror + DaemonSink ---

static void test_daemon_sink() {
    EventBuffer ev;
    StateMirror st;
    DaemonSink sink(&ev, &st);

    sink.init_session(2);
    sink.begin_file(0, "rel/a.wav");
    sink.begin_file(1, "rel/b.wav");
    CHECK(st.size() == 2);
    CHECK(st.snapshot()[0].state == "queued");

    sink.prep(0);
    CHECK(st.snapshot()[0].state == "prep");

    sink.set_tasks(0, 3);
    auto rows = st.snapshot();
    CHECK(rows[0].tasks.size() == 3);

    sink.task(0, 0, obs::TaskState::Running);
    sink.task(0, 0, obs::TaskState::Ok);
    sink.task(0, 1, obs::TaskState::Failed);
    rows = st.snapshot();
    CHECK(rows[0].tasks[0] == "ok");
    CHECK(rows[0].tasks[1] == "failed");
    CHECK(rows[0].tasks[2] == "pend");

    sink.end_file(0, 12.5);
    rows = st.snapshot();
    CHECK(rows[0].state == "ok");
    CHECK(rows[0].pct == 12.5);

    sink.mark_skip(1);
    rows = st.snapshot();
    CHECK(rows.size() == 2);
    CHECK(rows[1].state == "skip");

    // события: seq монотонны, типы корректны
    auto evs = ev.copy_since(0);
    CHECK(evs.events[0].type == "session");
    CHECK(evs.events[1].type == "begin_file");
    CHECK(evs.last_seq == evs.events[evs.events.size() - 1].seq);

    // error-строка с id:null (аналог obs::error(line))
    sink.error("boom\n");
    evs = ev.copy_since(0);
    auto& ev_last = evs.events[evs.events.size() - 1];
    CHECK(ev_last.type == "error");
    CHECK(ev_last.args["line"] == "boom\n");
    CHECK(ev_last.args["id"].is_null());
}

// --- rpc::call с фейковым Daemon ---

struct FakeDaemon : Daemon {
    bool paused_flag = false;
    bool shut_force = false;
    std::vector<std::string> added_paths;
    uint64_t cancelled_id = 9999;
    bool cancel_exists = false;

    std::string version() const override { return "1.10.2-dev"; }
    double uptime_s() const override { return 42.0; }
    nlohmann::json session_options() const override {
        return {{"jobs", 4}, {"verify", "all"}};
    }
    nlohmann::json counters() const override {
        return {{"total", 10}, {"done", 3}, {"failed", 1}};
    }
    bool paused() const override { return paused_flag; }
    void set_paused(bool p) override { paused_flag = p; }
    void add(const std::vector<std::string>& paths, bool recursive,
             nlohmann::json& result) override {
        (void)recursive;
        for (const auto& p : paths) {
            if (p == "/missing")
                result["rejected"].push_back({{"path", p}, {"reason", "not found"}});
            else
                result["added"].push_back({{"id", (size_t)added_paths.size()},
                                           {"label", p}});
            added_paths.push_back(p);
        }
    }
    bool cancel_file(uint64_t id) override {
        cancelled_id = id;
        return cancel_exists;
    }
    bool remove(uint64_t id) override {
        cancelled_id = id;
        return cancel_exists;
    }
    bool reorder(const std::vector<size_t>& order) override {
        (void)order;
        return true;
    }
    void request_shutdown(bool force) override { shut_force = force; }
    nlohmann::json formats() const override {
        return {{"formats", nlohmann::json::array(
                                {{{"id", "flac"}, {"extensions", {"flac"}}}})}};
    }
};

static void test_rpc() {
    FakeDaemon d;

    auto ping = dsvc::call(d, "ping", nlohmann::json::object());
    CHECK(ping["ok"] == true);
    CHECK(ping["result"]["version"] == "1.10.2-dev");
    CHECK(ping["result"]["uptime_s"] == 42.0);
    CHECK(ping["result"]["queue"]["total"] == 10);

    auto add = dsvc::call(d, "add", {{"paths", {"a.flac", "/missing"}}});
    CHECK(add["ok"] == true);
    CHECK(add["result"]["added"].size() == 1);
    CHECK(add["result"]["rejected"].size() == 1);
    CHECK(add["result"]["rejected"][0]["reason"] == "not found");

    auto cancel = dsvc::call(d, "cancel-file", {{"id", 3}});
    CHECK(cancel["ok"] == true);
    CHECK(d.cancelled_id == 3);

    auto ca = dsvc::call(d, "cancel-all", nlohmann::json::object());
    CHECK(ca["result"]["paused"] == true);
    CHECK(d.paused_flag == true);

    auto resume = dsvc::call(d, "resume", nlohmann::json::object());
    CHECK(resume["result"]["paused"] == false);
    CHECK(d.paused_flag == false);

    auto sd = dsvc::call(d, "shutdown", {{"force", true}});
    CHECK(sd["ok"] == true);
    CHECK(d.shut_force == true);

    auto fmt = dsvc::call(d, "formats", nlohmann::json::object());
    CHECK(fmt["result"]["formats"][0]["id"] == "flac");

    auto unk = dsvc::call(d, "nope", nlohmann::json::object());
    CHECK(unk["ok"] == false);
    CHECK(unk["code"] == "unknown_cmd");

    auto bad = dsvc::call(d, "ping", nlohmann::json::array());
    CHECK(bad["ok"] == false);
    CHECK(bad["code"] == "bad_args");
}

int main() {
    test_event_buffer();
    test_daemon_sink();
    test_rpc();
    if (failures == 0) {
        std::cout << "OK\n";
        return 0;
    }
    std::cerr << failures << " failure(s)\n";
    return 1;
}
