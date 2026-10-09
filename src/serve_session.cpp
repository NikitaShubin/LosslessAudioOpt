#include "serve.h"
#include "serve_internal.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <vector>

#include "config.h"
#include "contract.h"
#include "i18n.h"
#include "obs.h"
#include "out.h"
#include "report.h"
#include "stats.h"
#include "tool.h"
#include "util.h"
#include "version.h"

namespace dsvc {

double monotonic_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

DaemonSession::DaemonSession(optimize::Options opts, EventBuffer* ev, StateMirror* st,
                             std::string restore_to, bool update_codecs)
    : opts_(std::move(opts)),
      restore_to_(std::move(restore_to)),
      update_codecs_(update_codecs),
      ev_(ev),
      st_(st) {
    started_ = monotonic_s();
}

DaemonSession::~DaemonSession() {
    // Фоновую сборку кэша состояния надо дождаться до разрушения зеркала и
    // буфера событий: поток зовёт сборщик, который их читает.
    if (state_worker_.joinable()) state_worker_.join();
    shutdown();
}

std::string DaemonSession::stats_document(const stats::SummaryFilter& f) {
    // Ключ кэша — сами значения фильтра: сменилась разрядность или частота, и
    // прошлый документ уже не подходит.
    // Наборы сравниваем как множества: порядок в запросе не должен ронять кэш.
    auto same_set = [](const std::vector<int>& a, const std::vector<int>& b) {
        if (a.size() != b.size()) return false;
        for (int v : a) if (std::find(b.begin(), b.end(), v) == b.end()) return false;
        return true;
    };
    auto same_filter = [&](const stats::SummaryFilter& g) {
        return same_set(g.bits, f.bits) && same_set(g.sample_rate, f.sample_rate) &&
               same_set(g.channels, f.channels) &&
               same_set(g.duration_bucket, f.duration_bucket) &&
               g.wav_denominator == f.wav_denominator;
    };
    std::lock_guard<std::mutex> lk(stats_m_);
    const auto age = std::chrono::steady_clock::now() - stats_at_;
    if (!stats_json_.empty() && same_filter(stats_filter_) &&
        age < std::chrono::milliseconds(kStatsCacheMs))
        return stats_json_;
    std::string doc;
    try {
        doc = stats::summary_json(stats::load_rows(), f);
    } catch (...) {
        // Сбой сборки не должен ронять демон: веб получит прошлый документ
        // (или пустоту, если его ещё не было), а следующий запрос построит новый.
        return stats_json_;
    }
    if (doc.empty()) return stats_json_;
    stats_json_ = std::move(doc);
    stats_filter_ = f;
    stats_at_ = std::chrono::steady_clock::now();
    return stats_json_;
}

void DaemonSession::set_state_builder(std::function<std::string()> build) {
    bool start = false;
    {
        std::lock_guard<std::mutex> lk(state_m_);
        state_build_ = std::move(build);
        start = static_cast<bool>(state_build_) && !state_building_;
        if (start) state_building_ = true;
    }
    // Прогрев кэша сразу при монтировании обработчиков: иначе первый запрос
    // веба ждал бы синхронной сборки — на горячей очереди это минуты.
    if (!start) return;
    auto b = state_build_;
    state_worker_ = std::thread([this, b] { state_rebuild(b, st_->revision()); });
}

std::string DaemonSession::state_document(int max_age_ms) {
    std::function<std::string()> build;
    bool sync = false;
    uint64_t rev = st_->revision();
    {
        std::unique_lock<std::mutex> lk(state_m_);
        if (!state_build_) return std::string();
        // Зеркало изменилось — документ неверен в любом случае, даже если
        // секунду назад он был свежим. Это и есть read-after-write для
        // /api/state: сразу после reorder/pause/remove читатель должен увидеть
        // новое состояние.
        const bool dirty = (rev != state_rev_);
        if (state_json_.empty()) {
            sync = true;  // кэша ещё нет: отдавать нечего
        } else if (dirty) {
            // Зеркало поменялось. Если прошлая сборка была дорогой (насыщенная
            // очередь) — отдаём прошлый документ и пересобираем в фоне: там
            // ожидание в минуты куда хуже секундной задержки данных.
            if (state_build_ms_ <= kStateSyncBudgetMs) {
                sync = true;
            } else if (!state_building_) {
                state_building_ = true;
                build = state_build_;
                if (state_worker_.joinable()) state_worker_.join();
                state_worker_ = std::thread([this, build, rev] { state_rebuild(build, rev); });
            }
            if (!sync) return state_json_;
        } else {
            auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - state_at_)
                           .count();
            if (age <= max_age_ms) return state_json_;
            // Ничего не менялось, но документ подрос — обновляем в фоне, чтобы
            // счётчики и last_seq не застаивались (веб их показывает).
            if (state_build_ms_ <= kStateSyncBudgetMs) {
                sync = true;
            } else if (!state_building_) {
                state_building_ = true;
                build = state_build_;
                if (state_worker_.joinable()) state_worker_.join();
                state_worker_ = std::thread([this, build, rev] { state_rebuild(build, rev); });
            }
            if (!sync) return state_json_;
        }
        if (sync) {
            build = state_build_;
            lk.unlock();  // сборка идёт без state_m_ (см. порядок блокировок)
            auto t0 = std::chrono::steady_clock::now();
            std::string doc = build();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
            std::lock_guard<std::mutex> lk2(state_m_);
            state_json_ = doc;
            state_at_ = std::chrono::steady_clock::now();
            state_build_ms_ = ms;
            state_rev_ = rev;
            return doc;
        }
    }
}

void DaemonSession::state_rebuild(const std::function<std::string()>& build,
                                  uint64_t rev) {
    auto t0 = std::chrono::steady_clock::now();
    std::string doc;
    try {
        doc = build();
    } catch (...) {
        // Сборка не должна ронять демон из-за одного сбойного снимка: веб
        // получит прошлый документ, а следующая попытка построит новый.
        std::lock_guard<std::mutex> lk(state_m_);
        state_building_ = false;
        return;
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0)
                  .count();
    std::lock_guard<std::mutex> lk(state_m_);
    state_json_ = std::move(doc);
    state_at_ = std::chrono::steady_clock::now();
    state_build_ms_ = ms;
    // Ревизию фиксируем ту, под которую реально собрали. Пока зеркало уедет
    // вперёд, документ считается устаревшим по change и следующий запрос либо
    // пересоберёт, либо отдаст прошлый.
    state_rev_ = rev;
    state_building_ = false;
}

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
        // --update-codecs: сначала обновление, потом обычный гейт. Обновление
        // идёт до ensure(), иначе ensure() увидит старый бинарник в кэше и
        // вернёт success, не дав заглянуть в новую версию.
        if (update_codecs_) {
            auto results = tool::update_codecs(enabled);
            for (const auto& r : results) {
                if (r.status == "failed") {
                    problems.push_back(i18n::fmt(
                        "format %s: codec update failed (%s)", r.id.c_str(),
                        r.message.c_str()));
                } else if (r.status == "updated") {
                    out::print("  %s: codec updated (%s)\n", r.id.c_str(), r.message.c_str());
                }
            }
        }
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
    // Отметка «файл в работе» — синхронно и до обновления зеркала (см.
    // DaemonSession::mark_active): без неё kill -9 в момент между «демон
    // показал prep» и «queue.json перезаписан» приводит к тихой перезаписи
    // прерванного файла при следующем старте.
    sink_->set_on_active([this](size_t id, bool active) { mark_active(id, active); });

    engine_ = std::make_unique<optimize::Engine>();
    if (int rc = engine_->init(opts_, {}, err); rc != 0) return rc;
    return 0;
}

std::string DaemonSession::version() const { return LLAO_VERSION; }

double DaemonSession::uptime_s() const { return monotonic_s() - started_; }

nlohmann::json DaemonSession::session_options() const {
    // Отдаём реальные настройки сессии. Раньше здесь стояла константа true, и
    // интерфейс показывал tolerant-режим, хотя демон работал строго — режим,
    // заданный флагом --ignore-errors, до сюда не доходил.
    return {{"jobs", opts_.jobs},
            {"jobs_float", opts_.jobs_float},
            {"dry_run", opts_.dry_run},
            {"verify", dsvc::verify_str(opts_.verify)},
            {"ignore_errors", opts_.ignore_errors}};
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
    // Пауза меняет только флаг и не трогает зеркало, поэтому ревизия зеркала не
    // растёт — а /api/state отдаёт из кэша и показывал бы прежнее значение
    // paused. Сбрасываем кэш явно: после паузы и возобновления читатель обязан
    // увидеть новое состояние сразу.
    {
        std::lock_guard<std::mutex> lk(state_m_);
        state_json_.clear();
        state_rev_ = st_->revision();
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

    // Штатная остановка: активные файлы переведены в queue.json как queued,
    // маркер «в работе» больше не нужен — иначе следующий старт увидит в нём
    // пути уже завершённых файлов и восстановит их как stopped.
    clear_active();

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
