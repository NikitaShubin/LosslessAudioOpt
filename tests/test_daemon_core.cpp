// Юнит-тесты фазы 1 (платформонезависимые): EventBuffer, StateMirror, DaemonSink,
// rpc::call. Собираются/запускаются нативно: make test-daemon-core (см. Makefile).
#include <cassert>
#include <cstdint>
#include <iostream>
#include <set>
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

// --- StateMirror: tombstones и subset-reorder ---
static void test_mirror_remove_reorder() {
    StateMirror st;
    EventBuffer ev;
    DaemonSink s2(&ev, &st);

    s2.begin_file(0, "a.wav");
    s2.begin_file(1, "b.wav");
    s2.begin_file(2, "c.wav");
    CHECK(st.size() == 3);

    // remove + запоздалые события воркера: призрака быть не должно
    st.remove(1);
    CHECK(st.size() == 2);
    s2.prep(1);
    s2.set_tasks(1, 2);
    s2.task(1, 0, obs::TaskState::Running);
    s2.task(1, 1, obs::TaskState::Ok);
    s2.end_file(1, 10.0);
    CHECK(st.size() == 2);
    for (const auto& r : st.snapshot()) CHECK(r.id != 1);

    // subset-reorder видимых строк
    CHECK(st.reorder({2, 0}) == true);
    auto rows = st.snapshot();
    CHECK(rows.size() == 2);
    CHECK(rows[0].id == 2);
    CHECK(rows[1].id == 0);

    // дубли и пустой порядок — отказ
    CHECK(st.reorder({}) == false);
    CHECK(st.reorder({0, 0}) == false);
    // неизвестный id — отказ, порядок не меняется
    CHECK(st.reorder({9}) == false);
    CHECK(st.snapshot()[0].id == 2);
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
    bool restart(uint64_t id) override {
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

    d.cancel_exists = true;
    auto rs = dsvc::call(d, "restart", {{"ids", {7, 8}}});
    CHECK(rs["ok"] == true);
    CHECK(rs["result"]["restarted"].size() == 2);
    CHECK(d.cancelled_id == (uint64_t)8);

    auto rs1 = dsvc::call(d, "restart", {{"id", 5}});
    CHECK(rs1["ok"] == true);
    CHECK(rs1["result"]["restarted"].size() == 1);

    auto rs_bad = dsvc::call(d, "restart", nlohmann::json::object());
    CHECK(rs_bad["ok"] == false);
    CHECK(rs_bad["code"] == "bad_args");

    auto fmt = dsvc::call(d, "formats", nlohmann::json::object());
    CHECK(fmt["result"]["formats"][0]["id"] == "flac");

    auto unk = dsvc::call(d, "nope", nlohmann::json::object());
    CHECK(unk["ok"] == false);
    CHECK(unk["code"] == "unknown_cmd");

    auto bad = dsvc::call(d, "ping", nlohmann::json::array());
    CHECK(bad["ok"] == false);
    CHECK(bad["code"] == "bad_args");
}

// --- Замысловатые сценарии зеркала: шторм запоздалых событий ---

static void test_mirror_ghost_storm() {
    StateMirror st;
    EventBuffer ev;
    DaemonSink s(&ev, &st);

    s.begin_file(0, "a.wav");
    s.begin_file(1, "b.wav");
    s.set_tasks(0, 2);
    s.task(0, 0, obs::TaskState::Running);

    // Удаляем файл 0 прямо во время работы
    st.remove(0);
    CHECK(st.size() == 1);

    // Шторм запоздалых событий дорабатывающего воркера: ничего не воскресает
    s.prep(0);
    s.set_tasks(0, 3);
    s.task(0, 0, obs::TaskState::Ok);
    s.task(0, 1, obs::TaskState::Running);
    s.task(0, 2, obs::TaskState::Failed);
    s.end_file(0, 42.0);
    s.mark_skip(0);
    s.mark_error(0);
    s.log("late log\n");
    s.error("late error\n");
    CHECK(st.size() == 1);
    auto rows = st.snapshot();
    CHECK(rows.size() == 1);
    CHECK(rows[0].id == 1);

    // Повторный remove того же id — безопасен, размер не меняется
    st.remove(0);
    CHECK(st.size() == 1);

    // upsert с тем же id после remove — игнорируется (id не переиспользуются)
    Row r;
    r.id = 0;
    r.label = "ghost.wav";
    r.state = "queued";
    st.upsert(r);
    CHECK(st.size() == 1);
    CHECK(st.snapshot()[0].id == 1);
}

// --- Гонка task() до set_tasks (сценарий «Мерзлоты») ---

static void test_mirror_task_before_set_tasks() {
    StateMirror st;
    EventBuffer ev;
    DaemonSink s(&ev, &st);

    s.begin_file(5, "merzlota.ape");
    s.prep(5);
    // Воркер взял вариант 0 раньше, чем prep опубликовал set_tasks
    s.task(5, 0, obs::TaskState::Running);
    s.task(5, 2, obs::TaskState::Ok);
    // Поздний set_tasks обязан сохранить уже известные состояния
    std::vector<obs::TaskInfo> infos = {{"flac", "8", {"-8"}, ""},
                                        {"tak", "p2", {"-p2"}, ""},
                                        {"tta", "default", {}, ""}};
    s.set_tasks(5, infos);
    auto rows = st.snapshot();
    CHECK(rows.size() == 1);
    CHECK(rows[0].tasks.size() == 3);
    CHECK(rows[0].tasks[0] == "running");
    CHECK(rows[0].tasks[1] == "pend");
    CHECK(rows[0].tasks[2] == "ok");
    CHECK(rows[0].task_infos.size() == 3);
    CHECK(rows[0].task_infos[0].fmt_id == "flac");
    CHECK(rows[0].task_infos[1].variant_id == "p2");
    CHECK(rows[0].task_infos[2].params.empty());

    // строковая перегрузка ведёт себя так же
    s.begin_file(6, "x.wav");
    s.task(6, 1, obs::TaskState::Failed);
    st.set_tasks(6, std::vector<std::string>{"pend", "pend", "pend"});
    rows = st.snapshot();
    const Row* r6 = nullptr;
    for (const auto& r : rows) if (r.id == 6) r6 = &r;
    CHECK(r6 != nullptr);
    CHECK(r6->tasks[1] == "failed");
    CHECK(r6->tasks[0] == "pend");
}

// --- Инвариант порядка: order_ всегда соответствует rows_ ---

static void check_order_invariant(const StateMirror& st) {
    auto rows = st.snapshot();
    CHECK(rows.size() == st.size());
    std::set<size_t> seen;
    for (const auto& r : rows) CHECK(seen.insert(r.id).second);
}

static void test_mirror_order_consistency() {
    StateMirror st;
    EventBuffer ev;
    DaemonSink s(&ev, &st);

    // События вразнобой: set_task без begin_file не должен ломать порядок
    s.task(10, 0, obs::TaskState::Running);
    check_order_invariant(st);
    st.set_tasks(11, std::vector<std::string>{"pend", "pend"});
    check_order_invariant(st);
    st.set_state(12, "prep");
    check_order_invariant(st);
    st.set_pct(13, 1.5);
    check_order_invariant(st);
    s.set_tasks(14, std::vector<obs::TaskInfo>{{"f", "v", {}, ""}});
    check_order_invariant(st);
    CHECK(st.size() == 5);

    // Полный reorder, затем удаление из середины, затем subset-reorder
    CHECK(st.reorder({14, 12, 11, 10, 13}) == true);
    auto rows = st.snapshot();
    CHECK(rows[0].id == 14);
    CHECK(rows[4].id == 13);
    st.remove(12);
    check_order_invariant(st);
    CHECK(st.reorder({13, 14}) == true);
    rows = st.snapshot();
    CHECK(rows.size() == 4);
    CHECK(rows[0].id == 13);
    CHECK(rows[1].id == 14);
    // 11 и 10 сохранили текущий относительный порядок в хвосте
    CHECK(rows[2].id == 11);
    CHECK(rows[3].id == 10);

    // Двойной reorder подряд по стабильным id (второй не должен мешать)
    CHECK(st.reorder({10, 11, 14, 13}) == true);
    rows = st.snapshot();
    CHECK(rows[0].id == 10);
    CHECK(rows[3].id == 13);
}

// --- Границы EventBuffer ---

static void test_event_buffer_boundaries() {
    EventBuffer ev;
    ev.push("a");
    ev.push("b");
    uint64_t last = ev.last_seq();
    // since == last_seq -> пусто, без resync
    auto p = ev.copy_since(last);
    CHECK(p.events.empty());
    CHECK(!p.resync);
    CHECK(p.last_seq == last);
    // since далеко в будущем -> пусто, без resync
    auto p2 = ev.copy_since(last + 1000000);
    CHECK(p2.events.empty());
    CHECK(!p2.resync);
    // ровно на границе ёмкости resync ещё нет: since = last - capacity
    EventBuffer big;
    for (size_t i = 0; i < EventBuffer::kCapacity + 10; i++) big.push("x");
    uint64_t l = big.last_seq();
    auto ok = big.copy_since(l - EventBuffer::kCapacity);
    CHECK(!ok.resync);
    CHECK(ok.events.size() == EventBuffer::kCapacity);
    auto over = big.copy_since(l - EventBuffer::kCapacity - 1);
    CHECK(over.resync);
}

// --- Матрица RPC: все команды, хорошие и плохие аргументы ---

static void test_rpc_matrix() {
    FakeDaemon d;
    d.cancel_exists = true;

    // pause / resume / алиасы
    auto pause = dsvc::call(d, "pause", nlohmann::json::object());
    CHECK(pause["ok"] == true && pause["result"]["paused"] == true);
    auto ca = dsvc::call(d, "cancel-all", nlohmann::json::object());
    CHECK(ca["ok"] == true && d.paused_flag == true);
    auto resume = dsvc::call(d, "resume", nlohmann::json::object());
    CHECK(resume["ok"] == true && d.paused_flag == false);

    // cancel-file: без id -> bad_args (раньше молча брал 0)
    auto cf = dsvc::call(d, "cancel-file", nlohmann::json::object());
    CHECK(cf["ok"] == false && cf["code"] == "bad_args");
    auto cf1 = dsvc::call(d, "cancel-file", {{"id", 3}});
    CHECK(cf1["ok"] == true);
    CHECK(d.cancelled_id == (uint64_t)3);
    // кривые типы id не бросают исключений — bad_args
    for (auto bad_id : {nlohmann::json(-1), nlohmann::json("x"),
                        nlohmann::json(1.5), nlohmann::json(nullptr),
                        nlohmann::json(true)}) {
        auto r = dsvc::call(d, "cancel-file", {{"id", bad_id}});
        CHECK(r["ok"] == false && r["code"] == "bad_args");
        auto r2 = dsvc::call(d, "remove", {{"id", bad_id}});
        CHECK(r2["ok"] == false && r2["code"] == "bad_args");
    }
    auto rm_missing = dsvc::call(d, "remove", nlohmann::json::object());
    CHECK(rm_missing["ok"] == false);
    // remove true/false: ответ ok в обоих случаях
    auto rm1 = dsvc::call(d, "remove", {{"id", 1}});
    CHECK(rm1["ok"] == true && rm1["result"]["removed"] == true);
    d.cancel_exists = false;
    auto rm0 = dsvc::call(d, "remove", {{"id", 2}});
    CHECK(rm0["ok"] == true && rm0["result"]["removed"] == false);
    d.cancel_exists = true;

    // reorder: order и алиас ids
    auto ro1 = dsvc::call(d, "reorder", {{"order", {2, 0, 1}}});
    CHECK(ro1["ok"] == true);
    auto ro2 = dsvc::call(d, "reorder", {{"ids", {1, 0}}});
    CHECK(ro2["ok"] == true);
    // reorder без массива -> bad_args
    auto ro_bad = dsvc::call(d, "reorder", {{"order", "nope"}});
    CHECK(ro_bad["ok"] == false && ro_bad["code"] == "bad_args");
    auto ro_empty = dsvc::call(d, "reorder", nlohmann::json::object());
    CHECK(ro_empty["ok"] == false);
    // кривые элементы массива не бросают исключений — bad_args
    for (auto bad_arr : {nlohmann::json({0, -1}), nlohmann::json({"a"}),
                         nlohmann::json({1.5}), nlohmann::json({nullptr})}) {
        auto r = dsvc::call(d, "reorder", {{"order", bad_arr}});
        CHECK(r["ok"] == false && r["code"] == "bad_args");
        auto r2 = dsvc::call(d, "restart", {{"ids", bad_arr}});
        CHECK(r2["ok"] == false && r2["code"] == "bad_args");
    }
    auto rs_badid = dsvc::call(d, "restart", {{"id", -1}});
    CHECK(rs_badid["ok"] == false);

    // stat: типы путей через фейк утилит не проверить, но формат ответа — да
    auto st = dsvc::call(d, "stat", {{"paths", {"/tmp", "/nope"}}});
    CHECK(st["ok"] == true);
    CHECK(st["result"]["paths"].size() == 2);
    CHECK(st["result"]["paths"][0].contains("type"));
    auto st_empty = dsvc::call(d, "stat", nlohmann::json::object());
    CHECK(st_empty["ok"] == true && st_empty["result"]["paths"].empty());

    // add: пустой список, recursive=false
    auto ad = dsvc::call(d, "add", {{"paths", nlohmann::json::array()},
                                    {"recursive", false}});
    CHECK(ad["ok"] == true);
    CHECK(ad["result"]["added"].empty() && ad["result"]["rejected"].empty());

    // add: paths не массив -> bad_args; не-строки -> rejected
    auto ad_np = dsvc::call(d, "add", {{"paths", "nope"}});
    CHECK(ad_np["ok"] == false && ad_np["code"] == "bad_args");
    auto ad_ns = dsvc::call(d, "add", {{"paths", {42, "ok.wav"}}});
    CHECK(ad_ns["ok"] == true);
    CHECK(ad_ns["result"]["rejected"].size() == 1);
    CHECK(ad_ns["result"]["rejected"][0]["reason"] == "not a string path");
    // add: кривой recursive игнорируется (default true), не исключение
    auto ad_rc = dsvc::call(d, "add", {{"paths", nlohmann::json::array()},
                                       {"recursive", "yes"}});
    CHECK(ad_rc["ok"] == true);

    // stat: paths не массив -> bad_args; не-строки -> invalid
    auto st_np = dsvc::call(d, "stat", {{"paths", "nope"}});
    CHECK(st_np["ok"] == false && st_np["code"] == "bad_args");
    auto st_ns = dsvc::call(d, "stat", {{"paths", {42}}});
    CHECK(st_ns["ok"] == true);
    CHECK(st_ns["result"]["paths"][0]["type"] == "invalid");

    // shutdown без force -> force=false; кривой force игнорируется
    auto sd = dsvc::call(d, "shutdown", nlohmann::json::object());
    CHECK(sd["ok"] == true && d.shut_force == false);
    auto sd_f = dsvc::call(d, "shutdown", {{"force", "yes"}});
    CHECK(sd_f["ok"] == true && d.shut_force == false);

    // restart: cancel_exists=false -> пустой restarted, но ok
    d.cancel_exists = false;
    auto rs0 = dsvc::call(d, "restart", {{"ids", {1}}});
    CHECK(rs0["ok"] == true && rs0["result"]["restarted"].empty());
    d.cancel_exists = true;

    // Необъектные args у объектных команд
    auto arr = dsvc::call(d, "add", nlohmann::json::array());
    CHECK(arr["ok"] == false && arr["code"] == "bad_args");
}

int main() {
    test_event_buffer();
    test_event_buffer_boundaries();
    test_daemon_sink();
    test_mirror_remove_reorder();
    test_mirror_ghost_storm();
    test_mirror_task_before_set_tasks();
    test_mirror_order_consistency();
    test_rpc();
    test_rpc_matrix();
    if (failures == 0) {
        std::cout << "OK\n";
        return 0;
    }
    std::cerr << failures << " failure(s)\n";
    return 1;
}
