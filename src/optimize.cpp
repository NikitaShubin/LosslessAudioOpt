#include "optimize_internal.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "config.h"
#include "i18n.h"
#include "media.h"
#include "obs.h"
#include "out.h"
#include "proc.h"
#include "report.h"
#include "stats.h"
#include "tags.h"
#include "tool.h"
#include "util.h"

namespace optimize {

void clear_session_tmp_dir(const std::string& custom) {
    clear_session_tmp_dir_impl(custom);
}

// ---------------------------------------------------------------------------
// Engine: долгоживущий движок для демона (Phase 0).
// ---------------------------------------------------------------------------

struct Engine::Impl {
    Options opts;
    std::vector<config::Format> fmts;
    std::unique_ptr<report::Logger> logger;
    Runner r;
    std::vector<std::thread> threads;
    std::string tmp;
    int jobs = 1;
    bool started = false;

    bool load_formats(std::string* err) {
        try {
            fmts = config::load_all();
        } catch (const std::exception& exc) {
            if (err) *err = exc.what();
            return false;
        }
        auto ranks = stats::ranking(stats::load());
        std::stable_sort(fmts.begin(), fmts.end(), [&](const config::Format& a,
                                                       const config::Format& b) {
            double ra = -1.0, rb = -1.0;
            for (const auto& rk : ranks) {
                if (rk.format == a.id) ra = rk.savings;
                if (rk.format == b.id) rb = rk.savings;
            }
            bool ha = ra >= 0.0, hb = rb >= 0.0;
            if (ha != hb) return ha;
            if (ha && hb) return ra > rb;
            return false;
        });
        return true;
    }

    bool collect(const std::vector<std::string>& inputs, std::vector<FileItem>& items,
                 std::string* err) {
        for (const auto& p : inputs) {
            std::string e;
            collect_files(p, items, &e, this->fmts);
            if (!e.empty() && err && err->empty()) *err = e;
        }
        if (items.empty() && err && err->empty()) *err = "no audio files found";
        return !items.empty();
    }
};

Engine::Engine() : impl_(std::make_unique<Impl>()) {}
Engine::~Engine() { shutdown(); }

int Engine::init(const Options& opts, const std::vector<std::string>& initial_inputs,
                 std::string* err) {
    Impl& i = *impl_;
    if (i.started) return 0;

    i.opts = opts;
    i.opts.mode = SessionMode::Daemon;

    if (!i.load_formats(err)) return 1;

    std::vector<FileItem> items;
    i.collect(initial_inputs, items, nullptr);

    i.jobs = resolve_jobs(i.opts.jobs, i.opts.jobs_float);

    clear_tmp_base(i.opts.tmp_dir);
    i.tmp = base_tmp_dir(i.opts.tmp_dir);

    if (i.opts.debug)
        i.logger = std::make_unique<report::Logger>(
            util::join_path(util::exe_dir(), "runs"));

    Runner& r = i.r;
    r.opts = &i.opts;
    r.fmts = &i.fmts;
    r.logger = (i.logger && i.logger->ok()) ? i.logger.get() : nullptr;
    r.ffprobe = media::find_ffprobe();
    r.ffmpeg = media::find_ffmpeg();
    r.tmp = i.tmp;
    r.window = i.jobs;
    r.rm.set_tmp_path(i.tmp);
    r.rm.set_max_workers(i.jobs);

    if (i.logger && i.logger->ok()) {
        i.logger->event({{"type", "run_start"},
                        {"jobs", i.jobs},
                        {"files", (size_t)items.size()},
                        {"dry_run", i.opts.dry_run},
                        {"verify", verify_name(i.opts.verify)},
                        {"ignore_errors", i.opts.ignore_errors},
                        {"report", i.opts.report_path},
                        {"machine_id", util::machine_id()},
                        {"machine_cpu", util::machine_cpu()},
                        {"machine_host", util::machine_host()}});
    }

    r.jobs.reserve(items.size());
    for (const auto& it : items) {
        size_t idx = r.jobs.size();
        r.jobs.emplace_back(std::make_unique<FileJob>());
        r.make_job(*r.jobs[idx], idx, it);
        r.seen_paths_.insert(norm_path(it.path));
    }
    for (size_t k = 0; k < r.jobs.size(); k++) {
        obs::sink()->begin_file(r.jobs[k]->idx, r.jobs[k]->rel);
        obs::sink()->job_meta(r.jobs[k]->idx, "optimize", std::string());
    }
    if (r.jobs.size() > 1) {
        std::vector<size_t> idx(r.jobs.size());
        std::vector<std::string> labels;
        for (size_t k = 0; k < r.jobs.size(); k++) {
            idx[k] = k;
            labels.push_back(r.jobs[k]->rel);
        }
        obs::sink()->files_added(idx, labels);
    }

    for (int t = 0; t < i.jobs; t++) i.threads.emplace_back(&Runner::worker, &r);
    i.started = true;
    return 0;
}

std::vector<size_t> Engine::add(const std::vector<std::string>& inputs,
                                const AddOptions& ao) {
    Impl& i = *impl_;
    if (!i.started) return {};
    std::vector<FileItem> items;
    i.collect(inputs, items, nullptr);
    if (items.empty()) return {};
    return i.r.append_files(items, ao);
}

bool Engine::remove(size_t idx) {
    Impl& i = *impl_;
    if (!i.started) return false;
    i.r.remove_file(idx);
    return true;
}

bool Engine::reorder(const std::vector<size_t>& ids) {
    Impl& i = *impl_;
    if (!i.started) return false;
    return i.r.reorder(ids);
}

void Engine::pause() {
    Impl& i = *impl_;
    if (i.started) i.r.pause_queue();
}

void Engine::resume() {
    Impl& i = *impl_;
    if (i.started) i.r.resume_queue();
}

std::vector<EngineFile> Engine::snapshot() {
    std::vector<EngineFile> out;
    Impl& i = *impl_;
    if (!i.started) return out;
    std::lock_guard<std::mutex> lk(i.r.qm);
    out.reserve(i.r.jobs.size());
    for (size_t k = 0; k < i.r.jobs.size(); k++) {
        const FileJob& j = *i.r.jobs[k];
        EngineFile e;
        e.idx = j.idx;
        e.path = j.path;
        e.rel = j.rel;
        e.mode = j.mode == JobMode::Restore ? "restore" : "optimize";
        e.target_dir = j.target_dir;
        e.out_path = j.out_path;
        e.completed = j.completed;
        e.total_tasks = j.tasks.size();
        if (j.m) {
            std::lock_guard<std::mutex> jl(*j.m);
            if (j.done) {
                e.state = j.summary.status;
                e.detail = j.summary.detail;
                e.pct = j.summary.savings_pct;
                e.original = j.summary.original;
                e.best = j.summary.best;
                e.best_format = j.summary.best_format;
            } else if (j.cancelled) {
                e.state = "removed";
            } else if (j.prep_done) {
                e.state = (j.released > 0) ? "running" : "queued";
            } else {
                e.state = "queued";
            }
        }
        out.push_back(std::move(e));
    }
    return out;
}

nlohmann::json Engine::debug_state() {
    Impl& i = *impl_;
    nlohmann::json jobs = nlohmann::json::array();
    if (!i.started) {
        return {{"started", false}, {"jobs", jobs}};
    }
    std::lock_guard<std::mutex> lk(i.r.qm);
    for (const auto& jp : i.r.jobs) {
        const FileJob& j = *jp;
        jobs.push_back({
            {"idx", j.idx},
            {"done", j.done},
            {"prep_done", j.prep_done},
            {"prep_running", j.prep_running},
            {"cancelled", j.cancelled},
            {"deferred", j.deferred},
            {"released", j.released},
            {"completed", j.completed},
            {"tasks", j.tasks.size()},
        });
    }
    return {
        {"started", true},
        {"paused", i.r.queue_paused.load()},
        {"abort", i.r.abort.load()},
        {"shutdown_requested", i.r.shutdown_requested.load()},
        {"workers_alive", i.r.workers_alive.load()},
        {"proc_cancelled", proc::cancelled()},
        {"proc_aborted", proc::aborted()},
        {"prep_active", i.r.prep_active},
        {"max_workers", i.r.rm.max_workers()},
        {"window", i.r.window},
        {"next_prep", i.r.next_prep},
        {"total_done", i.r.total_done},
        {"jobs", jobs},
    };
}

size_t Engine::done_count() {
    Impl& i = *impl_;
    std::lock_guard<std::mutex> lk(i.r.qm);
    return i.r.total_done;
}

size_t Engine::total_count() {
    Impl& i = *impl_;
    std::lock_guard<std::mutex> lk(i.r.qm);
    return i.r.jobs.size();
}

void Engine::shutdown() {
    Impl& i = *impl_;
    if (!i.started) return;
    proc::abort_all();
    i.r.shutdown_requested.store(true);
    i.r.cv.notify_all();
    for (auto& t : i.threads) {
        if (t.joinable()) t.join();
    }
    i.threads.clear();
    if (i.logger && i.logger->ok()) {
        i.logger->event({{"type", "run_end"},
                        {"done", i.r.total_done},
                        {"failed", i.r.failed.load()}});
    }
    clear_tmp_base(i.opts.tmp_dir);
    i.started = false;
}

// ---------------------------------------------------------------------------
// run(): одноразовый прогон
// ---------------------------------------------------------------------------

int run(const Options& opts) {
    std::vector<config::Format> fmts;
    try {
        fmts = config::load_all();
    } catch (const std::exception& exc) {
        out::error("ERROR: %s\n", exc.what());
        return 1;
    }

    {
        auto ranks = stats::ranking(stats::load());
        std::stable_sort(fmts.begin(), fmts.end(), [&](const config::Format& a,
                                                       const config::Format& b) {
            double ra = -1.0, rb = -1.0;
            for (const auto& r : ranks) {
                if (r.format == a.id) ra = r.savings;
                if (r.format == b.id) rb = r.savings;
            }
            bool ha = ra >= 0.0, hb = rb >= 0.0;
            if (ha != hb) return ha;
            if (ha && hb) return ra > rb;
            return false;
        });
    }

    std::vector<FileItem> items;
    for (const auto& p : opts.inputs) {
        std::string err;
        collect_files(p, items, &err, fmts);
        if (!err.empty()) out::error("WARNING: %s\n", err.c_str());
    }
    if (items.empty()) {
        out::error("ERROR: no audio files found\n");
        return 1;
    }
    std::vector<std::string> files;
    files.reserve(items.size());
    for (const auto& it : items) files.push_back(it.path);

    int jobs = resolve_jobs(opts.jobs, opts.jobs_float);

    clear_session_tmp_dir(opts.tmp_dir);
    std::string tmp = session_tmp_dir(opts.tmp_dir);

    report::Logger logger(opts.debug ? util::join_path(util::exe_dir(), "runs")
                                     : std::string());
    if (opts.debug) {
        if (logger.ok()) {
            out::print("Log: %s\n", logger.path().c_str());
        } else {
            out::error("WARNING: could not open the JSONL log (runs/)\n");
        }
    }

    out::print("Files: %zu, threads: %d\n", files.size(), jobs);

    if (logger.ok()) {
        logger.event({{"type", "run_start"},
                      {"jobs", jobs},
                      {"files", files.size()},
                      {"dry_run", opts.dry_run},
                      {"verify", verify_name(opts.verify)},
                      {"ignore_errors", opts.ignore_errors},
                      {"report", opts.report_path},
                      {"machine_id", util::machine_id()},
                      {"machine_cpu", util::machine_cpu()},
                      {"machine_host", util::machine_host()}});
    }

    Runner r;
    r.opts = &opts;
    r.fmts = &fmts;
    r.logger = logger.ok() ? &logger : nullptr;
    r.ffprobe = media::find_ffprobe();
    r.ffmpeg = media::find_ffmpeg();
    r.tmp = tmp;
    r.window = jobs;
    r.rm.set_tmp_path(tmp);
    r.rm.set_max_workers(jobs);
    r.jobs.clear(); r.jobs.reserve(files.size()); for (size_t _i=0;_i<files.size();_i++) r.jobs.emplace_back(std::make_unique<FileJob>());
    for (size_t i = 0; i < files.size(); i++) r.make_job(*r.jobs[i], i, items[i]);
    for (size_t i = 0; i < files.size(); i++) obs::sink()->begin_file(i, r.jobs[i]->rel);

    std::vector<std::thread> threads;
    for (int i = 0; i < jobs; i++) threads.emplace_back(&Runner::worker, &r);
    for (auto& t : threads) t.join();

    if (proc::cancelled()) {
        out::print("%s", i18n::str("Interrupted by user\n").c_str());
    }

    {
        bool any = false;
        for (auto& _j : r.jobs) { auto& j = *_j;
            if (j.summary.replacement_error.empty()) continue;
            if (!any) out::print("%s", i18n::str("Replacement failed:\n").c_str());
            out::print("  %s — %s\n", j.path.c_str(), j.summary.replacement_error.c_str());
            any = true;
        }
    }

    clear_session_tmp_dir(opts.tmp_dir);

    if (logger.ok()) {
        logger.event({{"type", "run_end"},
                      {"done", r.total_done},
                      {"failed", r.failed.load()}});
    }

    std::vector<report::FileSummary> summaries;
    for (auto& _j : r.jobs) summaries.push_back(_j->summary);

    if (!opts.report_path.empty()) {
        std::string rp = opts.report_path;
        if (util::dir_exists(rp) || util::ends_with(rp, "/") || util::ends_with(rp, "\\")) {
            util::mkdirs(rp);
            rp = util::join_path(rp, "llao-report-" + report::timestamp() + ".txt");
        }
        report::write_report(rp, summaries);
    }

    if (proc::cancelled()) return 130;

    uint64_t t_orig = 0, t_best = 0;
    for (const auto& fs : summaries) {
        t_orig += fs.original;
        t_best += fs.best;
    }
    if (t_orig > 0) {
        out::print("Total: %.2f MB -> %.2f MB (savings %.2f%%)\n", t_orig / 1048576.0,
                    t_best / 1048576.0,
                    t_best > 0 ? 100.0 * (1.0 - (double)t_best / (double)t_orig) : 0.0);
    }

    out::print("Done: %zu files processed, errors: %d\n", r.total_done, r.failed.load());
    if ((r.failed.load() > 0 || r.abort.load()) && !opts.ignore_errors) {
        out::error("Aborted: %d file(s) failed. Fix the issues above or re-run with "
                   "--ignore-errors to skip such files.\n",
                   r.failed.load());
    }
    return (r.failed.load() > 0 || r.abort.load()) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Восстановление (restore)
// ---------------------------------------------------------------------------

static int restore_one(size_t idx, const std::string& path, const config::Format& target,
                       const config::Variant& variant, bool no_download, bool allow_lossy,
                       const std::vector<config::Format>& fmts) {
    std::string ffprobe = media::find_ffprobe();
    std::string ffmpeg = media::find_ffmpeg();
    if (ffprobe.empty() || ffmpeg.empty()) {
        obs::sink()->error_file(idx, i18n::str("ERROR: ffprobe/ffmpeg unavailable (bin/ffmpeg/ or PATH)\n"));
        return 1;
    }

    std::string base = util::base_name(path);
    std::string base_ne = base_no_ext(path);
    std::string dir = util::dir_name(path);
    std::string tmp = session_tmp_dir(std::string());

    FileSession session(path, tmp);
    std::string src_wav = session.ref_wav_path();

    media::Probe probe = media::probe_file(path, ffprobe);
    bool src_decoded = false;
    if (!probe.ok && lower_ext(path) != util::to_lower(target.extension)) {
        const config::Format* src_fmt = find_source_fmt(probe, path, fmts);
        if (src_fmt && src_fmt->id != target.id) {
            DecodeStatus ds = decode_source_native(src_fmt, path, src_wav, 16);
            if (ds == DecodeStatus::NeedsCopy) {
                std::string copy = util::join_path(session.dir(),
                                                   "src_copy." + lower_ext(path));
                if (util::copy_file(path, copy)) {
                    ds = decode_source_native(src_fmt, copy, src_wav, 16);
                    util::remove_file(copy);
                }
            }
            if (ds == DecodeStatus::Ok) {
                probe = media::probe_file(src_wav, ffprobe);
                if (probe.ok) {
                    probe.format_name = src_fmt->id;
                    probe.size = util::file_size(path);
                    src_decoded = true;
                }
            }
        }
        if (!probe.ok) {
            session.cleanup();
            obs::sink()->error_file(idx, "ERROR " + path + " — " + probe.error + "\n");
            return 1;
        }
    }
    if (lower_ext(path) == util::to_lower(target.extension)) {
        obs::sink()->log("SKIP " + path + " — " + i18n::str("already the format ") + target.id + "\n");
        obs::sink()->mark_stopped(idx);
        return 0;
    }
    if (probe.has_video) {
        obs::sink()->log("SKIP " + path + " — " + i18n::str("video stream present (not audio)") + "\n");
        obs::sink()->mark_stopped(idx);
        return 0;
    }
    if (!probe.is_lossless() && !allow_lossy) {
        obs::sink()->log("SKIP " + path + " — " +
                     i18n::fmt("lossy input (codec %s), use --allow-lossy", probe.codec_name.c_str()) + "\n");
        obs::sink()->mark_stopped(idx);
        return 0;
    }

    obs::sink()->prep(idx);

    tags::TagSet embed_ts = tags::extract_tags(path, probe, target.tag_native_reader);
    tags::TagSet ts;
    std::string serr;
    tags::TagSet side_ts;
    bool have_sidecar = tags::read_sidecar(path, side_ts, &serr);
    if (have_sidecar) ts = tags::merge_tags(std::move(embed_ts), side_ts);
    else ts = std::move(embed_ts);

    int bits = probe.bits_per_sample;
    if (bits <= 0) bits = 16;

    const config::Format* src_fmt = find_source_fmt(probe, path, fmts);
    std::string dec_err;
    bool decoded = src_decoded;
    if (!decoded && src_fmt && src_fmt->id != target.id) {
        DecodeStatus ds = decode_source_native(src_fmt, path, src_wav, bits);
        if (ds == DecodeStatus::NeedsCopy) {
            std::string copy = util::join_path(session.dir(),
                                               "src_copy." + lower_ext(path));
            if (util::copy_file(path, copy)) {
                ds = decode_source_native(src_fmt, copy, src_wav, bits);
                util::remove_file(copy);
            }
        }
        decoded = (ds == DecodeStatus::Ok);
    }
    if (!decoded) decoded = media::decode_to_wav(path, src_wav, ffmpeg, bits, &dec_err);
    if (!decoded) {
        session.cleanup();
        obs::sink()->error_file(idx, "ERROR " + path + " — " + i18n::str("decode to reference WAV: ") + dec_err + "\n");
        return 1;
    }

    tool::Status st = tool::ensure(target, !no_download, "[" + target.id + "] ");
    if (st.path.empty()) {
        session.cleanup();
        obs::sink()->error_file(idx, i18n::fmt("ERROR %s — utility %s unavailable (%s)\n", path.c_str(),
                                target.id.c_str(), st.status.c_str()));
        return 1;
    }

    obs::sink()->set_tasks(idx, 1);

    Env env;
    env.fmt = &target;
    env.encoder = st.path;
    env.decoder = decoder_path(target, st.path);
    env.bits = bits;

    std::string candidate = session.candidate_path("restore", variant.id, target.extension);

    obs::sink()->task(idx, 0, obs::TaskState::Running);

    std::string verr = encode_candidate(src_wav, candidate, variant.args, env);
    if (verr.empty()) verr = validate_candidate(src_wav, candidate, env);
    if (!verr.empty()) {
        obs::sink()->task(idx, 0, obs::TaskState::Failed);
        session.cleanup();
        obs::sink()->error_file(idx, "ERROR " + path + " — " + verr + "\n");
        return 1;
    }
    uint64_t size = util::file_size(candidate);
    if (size == 0) {
        obs::sink()->task(idx, 0, obs::TaskState::Failed);
        session.cleanup();
        obs::sink()->error_file(idx, "ERROR " + path + " — " + i18n::str("empty file") + "\n");
        return 1;
    }

    obs::sink()->task(idx, 0, obs::TaskState::Ok);

    tags::TagPlan plan = tags::plan_tags(ts, native_types(target), target, true);
    std::string terr;
    uint64_t sidecar = 0;
    bool has_tags = false;
    for (const auto& [gtype, grp] : plan.embed) {
        terr = tags::write_group(candidate, target, gtype, grp);
        if (!terr.empty()) break;
    }
    if (terr.empty()) {
        if (!plan.embed.empty()) {
            size = util::file_size(candidate);
            has_tags = true;
        }
        if (!plan.sidecar.empty()) {
            std::string sc_base = util::join_path(session.dir(), target.id);
            sidecar = tags::write_sidecar(sc_base, plan.sidecar, &terr);
        }
    }
    if (!terr.empty()) {
        session.cleanup();
        obs::sink()->error_file(idx, "ERROR " + path + " — " + i18n::str("tags: ") + terr + "\n");
        return 1;
    }
    if (has_tags && ts.present) {
        std::string v2 = tags::validate_groups(candidate, target, plan.embed, ffprobe);
        if (!v2.empty()) {
            session.cleanup();
            obs::sink()->error_file(idx, "ERROR " + path + " — " + i18n::str("tag validation: ") + v2 + "\n");
            return 1;
        }
    }

    std::string new_path = util::join_path(dir, base_ne + "." + target.extension);
    if (!util::copy_file(candidate, new_path)) {
        session.cleanup();
        obs::sink()->error_file(idx, i18n::fmt("ERROR %s — could not copy the candidate into the folder\n", path.c_str()));
        return 1;
    }
    if (!util::remove_file(path)) {
        obs::sink()->log(i18n::fmt("WARNING %s — the new file is saved as %s, but the old file "
                               "could not be removed and is left in place\n",
                               path.c_str(), new_path.c_str()));
    }

    std::string old_sc = util::join_path(dir, base_ne + ".tags.zip");
    if (sidecar > 0) {
        std::string sc_src = util::join_path(session.dir(), target.id + ".tags.zip");
        bool sc_ok = util::copy_file(sc_src, old_sc);
        session.cleanup();
        if (sc_ok) {
            obs::sink()->log(i18n::fmt("OK   %s -> %s (%s), tags in sidecar\n", base.c_str(),
                                    target.id.c_str(), target.extension.c_str()));
        } else {
            obs::sink()->log(i18n::fmt("!    %s -> %s, but the sidecar was not copied\n", base.c_str(),
                                    target.id.c_str()));
        }
    } else {
        session.cleanup();
        util::remove_file(old_sc);
        obs::sink()->log(i18n::fmt("OK   %s -> %s (%s): %d KB, tags embedded\n", base.c_str(),
                                target.id.c_str(), target.extension.c_str(), (int)(size / 1024)));
    }
    obs::sink()->end_file(idx, 0.0);
    return 0;
}

int restore_run(const RestoreOptions& opts) {
    std::vector<config::Format> fmts;
    try {
        fmts = config::load_all();
    } catch (const std::exception& exc) {
        out::error("ERROR: %s\n", exc.what());
        return 1;
    }

    const config::Format* target = nullptr;
    for (const auto& f : fmts)
        if (f.id == opts.to) target = &f;
    if (!target) {
        out::error("ERROR: unknown target format '%s'\n", opts.to.c_str());
        return 1;
    }

    const config::Variant* variant = nullptr;
    if (!opts.variant.empty()) {
        for (const auto& v : target->variants)
            if (v.id == opts.variant) variant = &v;
        if (!variant) {
            out::error("ERROR: format '%s' has no variant '%s'\n", target->id.c_str(),
                        opts.variant.c_str());
            return 1;
        }
    } else if (!target->variants.empty()) {
        variant = &target->variants.back();
    }
    if (!variant) {
        out::error("ERROR: format '%s' has no compression variants\n", target->id.c_str());
        return 1;
    }

    std::vector<FileItem> items;
    for (const auto& p : opts.inputs) {
        std::string err;
        collect_files(p, items, &err, fmts);
        if (!err.empty()) out::error("WARNING: %s\n", err.c_str());
    }
    if (items.empty()) {
        out::error("ERROR: no audio files found\n");
        return 1;
    }

    int jobs = resolve_jobs(opts.jobs, opts.jobs_float);
    if (jobs > (int)items.size()) jobs = (int)items.size();

    clear_session_tmp_dir(std::string());

    out::print("Restoring to %s (variant %s): %zu files, threads: %d\n", target->id.c_str(),
               variant->id.c_str(), items.size(), jobs);

    std::atomic<int> failed{0};
    std::atomic<size_t> next{0};
    std::mutex m;
    size_t done = 0;
    std::vector<std::thread> threads;
    std::function<void()> worker = [&]() {
        util::set_thread_below_normal();
        for (;;) {
            size_t idx;
            {
                std::lock_guard<std::mutex> lk(m);
                if (next >= items.size()) break;
                idx = next++;
            }
            try {
                if (restore_one(idx, items[idx].path, *target, *variant, opts.no_download,
                                opts.allow_lossy, fmts) != 0)
                    failed++;
            } catch (const std::exception& exc) {
                obs::sink()->error_file(idx, "ERROR [" + items[idx].path + "]: " + exc.what() + "\n");
                failed++;
            }
            {
                std::lock_guard<std::mutex> lk(m);
                done++;
            }
        }
    };
    for (int i = 0; i < jobs; i++) threads.emplace_back(worker);
    for (auto& t : threads) t.join();

    clear_session_tmp_dir(std::string());

    out::print("Done: %zu files restored, errors: %d\n", done, failed.load());
    return failed.load() > 0 ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Список вариантов сжатия
// ---------------------------------------------------------------------------

int list_variants(const std::vector<std::string>& ids) {
    std::vector<config::Format> fmts = config::load_all();
    if (!ids.empty()) {
        std::vector<config::Format> filtered;
        std::vector<std::string> unknown;
        for (const auto& f : fmts)
            if (std::find(ids.begin(), ids.end(), f.id) != ids.end()) filtered.push_back(f);
        for (const auto& id : ids) {
            bool found = false;
            for (const auto& f : fmts)
                if (f.id == id) found = true;
            if (!found) unknown.push_back(id);
        }
        for (const auto& u : unknown)
            out::error("ERROR: unknown format '%s'\n", u.c_str());
        if (!unknown.empty()) return 1;
        if (filtered.empty()) {
            out::error("ERROR: no formats to display\n");
            return 1;
        }
        fmts = filtered;
    }

    out::print("LLAO — converter settings (from formats/*.json)\n\n");
    for (const auto& f : fmts) {
        out::print("%-18s %s (.%s) [%s]\n", f.id.c_str(), f.name.c_str(),
                    f.extension.c_str(), i18n::str(f.enabled ? "enabled" : "disabled").c_str());
        std::string base;
        for (size_t i = 0; i < f.encode_cmd.size(); i++) {
            if (i > 0) base += " ";
            base += f.encode_cmd[i];
        }
        out::print("  template: %s\n", base.c_str());
        out::print("  variants:\n");
        if (f.variants.empty()) {
            out::print("    (no variants)\n");
        } else {
            for (const auto& v : f.variants) {
                std::vector<std::string> args =
                    build_cmd(f.encode_cmd, f.encode_cmd.empty() ? f.id : f.encode_cmd[0],
                              "<input>", "<output>", v.args, f.engine_codec,
                              f.engine_container);
                std::string cmd;
                for (size_t i = 0; i < args.size(); i++) {
                    if (i > 0) cmd += " ";
                    cmd += args[i];
                }
                printf("    %-14s %s%s%s\n", v.id.c_str(), cmd.c_str(),
                       v.note.empty() ? "" : "   # ",
                       v.note.empty() ? "" : v.note.c_str());
            }
        }
        printf("\n");
    }
    return 0;
}

}  // namespace optimize
