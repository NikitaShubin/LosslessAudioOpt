#include "serve.h"
#include "serve_internal.h"

#include <chrono>
#include <memory>
#include <vector>

#include "config.h"
#include "contract.h"
#include "i18n.h"
#include "obs.h"
#include "report.h"
#include "tool.h"
#include "util.h"
#include "version.h"

namespace dsvc {

double monotonic_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

DaemonSession::DaemonSession(optimize::Options opts, EventBuffer* ev, StateMirror* st,
                             std::string restore_to)
    : opts_(std::move(opts)), restore_to_(std::move(restore_to)), ev_(ev), st_(st) {
    started_ = monotonic_s();
}

DaemonSession::~DaemonSession() { shutdown(); }

int DaemonSession::start(std::string* err) {
    try {
        auto fmts = config::load_all();
        formats_cache_ = nlohmann::json::array();
        std::vector<config::Format> enabled;
        for (const auto& f : fmts) {
            if (!f.enabled) continue;
            enabled.push_back(f);
            formats_cache_.push_back(
                {{"id", f.id},
                 {"extensions", nlohmann::json::array({f.extension})},
                 // Число вариантов кодирования из encode.variants в formats/*.json
                 // (не хардкод): максимум задач для optimize-строки сессии.
                 {"variants", f.variants.size()}});
        }
        if (!restore_to_.empty()) {
            bool known = false;
            for (const auto& f : formats_cache_)
                if (f.contains("id") && f["id"] == restore_to_) { known = true; break; }
            if (!known) {
                if (err) *err = "restore: target format \"" + restore_to_ +
                                "\" not found in formats/*.json";
                return 1;
            }
        }

        // Стартовый гейт кодеков: все включённые форматы должны быть готовы
        // (утилита в bin/<id>/ или PATH, cli_check из formats/*.json сходится).
        // Проблемы агрегируются и блокируют старт — сервер отказывает в запуске
        // при заведомо неисправных конверторах, а не «тихо» падает во время
        // прогона. Уважает --no-download (гейт не качает).
        std::vector<std::string> problems;
        bool download = !opts_.no_download;
        for (const auto& f : enabled) {
            tool::Status s = tool::ensure(f, download);
            std::string issue;
            if (s.status == "missing") {
                issue = i18n::fmt(
                    "format %s: utility not available (run `llao tools %s` "
                    "or install it into bin/%s/)",
                    f.id.c_str(), f.id.c_str(), f.id.c_str());
            }
            if (issue.empty() && !s.path.empty()) issue = tool::check_config(f);
            if (!issue.empty()) problems.push_back(issue);
        }
        if (!problems.empty()) {
            std::string msg =
                i18n::str("codecs are not ready — the server refuses to start:\n");
            for (const auto& p : problems) msg += "  - " + p + "\n";
            msg += i18n::str(
                "fix the utilities (`llao tools`) and restart, or use `--no-download`");
            if (err) *err = msg;
            return 1;
        }
    } catch (const std::exception& exc) {
        if (err) *err = exc.what();
        return 1;
    }

    sink_ = std::make_unique<DaemonSink>(ev_, st_);
    obs::set_sink(sink_.get());
    // Инкрементальная персистентность: состояние файла изменилось (prep/ok/
    // stopped/error) — записываем queue.json. Колбэк вызывается воркерами
    // движка (в т.ч. под qm на некоторых путях mark_stopped), поэтому persist()
    // не должен трогать движок — только зеркало и свой мьютекс.
    sink_->set_on_change([this] { persist(false); });

    engine_ = std::make_unique<optimize::Engine>();
    if (int rc = engine_->init(opts_, {}, err); rc != 0) return rc;
    return 0;
}

std::string DaemonSession::version() const { return LLAO_VERSION; }

double DaemonSession::uptime_s() const { return monotonic_s() - started_; }

nlohmann::json DaemonSession::session_options() const {
    return {{"jobs", opts_.jobs},
            {"jobs_float", opts_.jobs_float},
            {"dry_run", opts_.dry_run},
            {"verify", dsvc::verify_str(opts_.verify)},
            {"ignore_errors", true}};
}

nlohmann::json DaemonSession::counters() const {
    int total = 0, done = 0, failed = 0, stopped = 0, ok = 0;
    for (const auto& r : st_->snapshot()) {
        total++;
        if (r.state == "ok" || r.state == "stopped" || r.state == "error") done++;
        if (r.state == "error") failed++;
        if (r.state == "stopped") stopped++;
        if (r.state == "ok") ok++;
    }
    return {{"total", total}, {"done", done}, {"failed", failed},
            {"stopped", stopped}, {"ok", ok}};
}

bool DaemonSession::paused() const { return paused_.load(); }

void DaemonSession::set_paused(bool paused) {
    if (paused_.exchange(paused) == paused) return;
    ev_->push(paused ? "stopped" : "resumed");
    if (paused) {
        if (engine_) engine_->pause();
    } else {
        if (engine_) engine_->resume();
    }
}

void DaemonSession::request_shutdown(bool /*force*/) {
    if (on_shutdown_) on_shutdown_();
}

nlohmann::json DaemonSession::formats() const {
    return {{"formats", formats_cache_}};
}

nlohmann::json DaemonSession::debug_state() {
    if (!engine_) return nullptr;
    return engine_->debug_state();
}

void DaemonSession::shutdown() {
    if (shutting_down_.exchange(true)) return;

    // ФИНАЛЬНЫЙ persist ДО остановки движка: active (queued|prep|running) ->
    // queued на перезапуске; abort переводит активные в stopped, и после
    // engine_->shutdown() их нельзя было бы отличить от остановленных
    // пользователем. Пока движок жив, зеркало ещё содержит полный порядок
    // очереди (include строки вне движка).
    {
        std::lock_guard<std::mutex> lk(persist_m_);
        persist_rows(snapshot_for_persist(true));
    }

    if (engine_) engine_->shutdown();

    // Итоговый отчёт (если задан --report).
    if (!opts_.report_path.empty() && engine_) {
        std::vector<report::FileSummary> summaries;
        for (const auto& ef : engine_->snapshot()) {
            report::FileSummary s;
            s.path = ef.path;
            s.status = ef.state;
            s.detail = ef.detail;
            s.original = ef.original;
            s.best = ef.best;
            s.savings_pct = ef.pct;
            s.best_format = ef.best_format;
            summaries.push_back(std::move(s));
        }
        std::string rp = opts_.report_path;
        if (util::dir_exists(rp) || util::ends_with(rp, "/") || util::ends_with(rp, "\\")) {
            util::mkdirs(rp);
            rp = util::join_path(rp, "llao-report-" + report::timestamp() + ".txt");
        }
        report::write_report(rp, summaries);
    }

    // Движок ещё жив: деструкторы его заданий при уборке временных файлов
    // логируют в sink(), поэтому обнулять глобальный sink можно только после
    // полного уничтожения движка (иначе SEGV на нулевом указателе).
    engine_.reset();

    obs::set_sink(nullptr);
}

}  // namespace dsvc
