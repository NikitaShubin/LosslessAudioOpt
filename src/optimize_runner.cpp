#include "optimize_internal.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <set>

#include "i18n.h"
#include "media.h"
#include "obs.h"
#include "proc.h"
#include "report.h"
#include "stats.h"
#include "tags.h"
#include "tool.h"
#include "util.h"

namespace optimize {

// ---------------------------------------------------------------------------
// Runner: методы (определения)
// ---------------------------------------------------------------------------

void Runner::make_job(FileJob& j, size_t idx, const FileItem& it) {
    j.idx = idx;
    j.path = it.path;
    j.base = util::base_name(it.path);
    j.rel = it.rel;
    j.base_ne = base_no_ext(it.path);
    j.dir = util::dir_name(it.path);
    j.tok = tmp_token(it.path);
    j.m = std::make_unique<std::mutex>();
}

std::vector<size_t> Runner::append_files(const std::vector<FileItem>& items,
                                         const AddOptions& ao) {
    std::vector<size_t> idx;
    std::vector<std::string> labels;
    {
        std::lock_guard<std::mutex> lk(qm);
        for (const auto& it : items) {
            if (seen_paths_.count(norm_path(it.path))) continue;
            seen_paths_.insert(norm_path(it.path));
            size_t i = jobs.size();
            jobs.emplace_back(std::make_unique<FileJob>());
            make_job(*jobs[i], i, it);
            jobs[i]->mode = ao.mode;
            jobs[i]->target_dir = ao.target_dir;
            jobs[i]->restore_to = ao.to;
            idx.push_back(i);
            labels.push_back(it.rel);
        }
        for (size_t k = 0; k < idx.size(); k++) {
            const FileJob& j = *jobs[idx[k]];
            obs::sink()->begin_file(idx[k], labels[k]);
            obs::sink()->job_meta(idx[k],
                                  j.mode == JobMode::Restore ? "restore" : "optimize",
                                  j.target_dir);
        }
        if (idx.size() > 1) obs::sink()->files_added(idx, labels);
        cv.notify_all();
    }
    return idx;
}

void Runner::pause_queue() {
    queue_paused.store(true);
    cv.notify_all();
}

void Runner::resume_queue() {
    queue_paused.store(false);
    cv.notify_all();
}

bool Runner::is_paused() const { return queue_paused.load(); }

size_t Runner::find_pos_locked(size_t idx) const {
    for (size_t p = 0; p < jobs.size(); p++)
        if (jobs[p]->idx == idx) return p;
    return SIZE_MAX;
}

void Runner::remove_file(size_t idx) {
    std::lock_guard<std::mutex> lk(qm);
    size_t pos = find_pos_locked(idx);
    if (pos == SIZE_MAX) return;
    FileJob& j = *jobs[pos];
    seen_paths_.erase(norm_path(j.path));
    if (j.done) {
        if (j.finalizing.load(std::memory_order_relaxed))
            j.kill_requested.store(true, std::memory_order_relaxed);
        return;
    }
    if (!j.prep_done && !j.prep_running && j.released == 0) {
        j.cancelled = true;
        j.kill_requested.store(true, std::memory_order_relaxed);
        j.done = true;
        j.summary.path = j.path;
        j.summary.status = "stopped";
        j.summary.detail = i18n::str("removed from queue");
        discard_job_tmp(j);
        total_done++;
        cv.notify_all();
        obs::sink()->mark_stopped(j.idx);
    } else {
        j.kill_requested.store(true, std::memory_order_relaxed);
        std::lock_guard<std::mutex> jl(*j.m);
        j.cancelled = true;
    }
}

void Runner::discard_job_tmp(FileJob& j) {
    if (j.peak_file > 0) {
        rm.release_disk(j.peak_file);
        j.peak_file = 0;
    }
    if (j.session) j.session.reset();
}

bool Runner::reorder(const std::vector<size_t>& ids) {
    std::lock_guard<std::mutex> lk(qm);
    if (ids.empty() || ids.size() > jobs.size()) return false;
    std::vector<size_t> pos_of(jobs.size(), SIZE_MAX);
    for (size_t p = 0; p < jobs.size(); p++)
        if (jobs[p]->idx < jobs.size()) pos_of[jobs[p]->idx] = p;
    std::vector<bool> seen(jobs.size(), false);
    for (size_t id : ids) {
        if (id >= jobs.size() || seen[id] || pos_of[id] == SIZE_MAX) return false;
        seen[id] = true;
    }
    std::vector<bool> listed(jobs.size(), false);
    for (size_t id : ids) listed[id] = true;
    std::vector<std::unique_ptr<FileJob>> reordered;
    reordered.reserve(jobs.size());
    for (size_t id : ids) reordered.push_back(std::move(jobs[pos_of[id]]));
    for (size_t p = 0; p < jobs.size(); p++)
        if (jobs[p] && !listed[jobs[p]->idx])
            reordered.push_back(std::move(jobs[p]));
    jobs = std::move(reordered);
    next_prep = 0;
    cv.notify_all();
    return true;
}

bool Runner::all_done_locked() const { return total_done == jobs.size(); }

void Runner::count_error(FileJob& j) {
    {
        std::lock_guard<std::mutex> jl(*j.m);
        count_error_locked(j);
    }
    if (opts->ignore_errors || opts->mode == SessionMode::Daemon) return;
    abort.store(true);
    proc::abort_all();
    cv.notify_all();
}

void Runner::count_error_locked(FileJob& j) {
    if (!j.error_counted) {
        j.error_counted = true;
        failed++;
    }
}

bool Runner::variant_launchable_locked() {
    if (abort.load()) return false;
    if (queue_paused.load() && opts->mode == SessionMode::Daemon) return false;
    for (size_t i = 0; i < jobs.size(); i++) {
        if (rm.can_start_new_variant(*jobs[i], window, false))
            return true;
    }
    return false;
}

bool Runner::deferred_retry_locked() {
    if (abort.load()) return false;
    if (queue_paused.load() && opts->mode == SessionMode::Daemon) return false;
    auto now = std::chrono::steady_clock::now();
    for (size_t i = 0; i < jobs.size(); i++) {
        const FileJob& j = *jobs[i];
        if (j.done || j.prep_done || j.prep_running || !j.deferred) continue;
        if (now >= j.defer_until) return true;
    }
    return false;
}

bool Runner::find_next_prep_locked(size_t* out) {
    if (jobs.empty()) return false;
    auto now = std::chrono::steady_clock::now();
    for (size_t k = 0; k < jobs.size(); k++) {
        size_t i = (next_prep + k) % jobs.size();
        FileJob& j = *jobs[i];
        if (j.done || j.prep_done || j.prep_running) continue;
        if (j.deferred && now < j.defer_until) continue;
        *out = i;
        return true;
    }
    return false;
}

bool Runner::prep_allowed_locked() const {
    if (queue_paused.load() && opts->mode == SessionMode::Daemon) return false;
    return rm.can_start_new_file(next_prep, prep_active, jobs, abort.load());
}

bool Runner::take_work_locked(Work* w) {
    if (abort.load()) return false;
    if (queue_paused.load() && opts->mode == SessionMode::Daemon) return false;
    for (size_t i = 0; i < jobs.size(); i++) {
        FileJob& j = *jobs[i];
        if (!rm.can_start_new_variant(j, window, false)) continue;
        uint64_t reserved = 0;
        size_t jf = j.released - j.completed;
        if (jf > 0 && j.ref_size > 0) {
            uint64_t vpeak = variant_peak_bytes(j.ref_size, opts->verify);
            if (rm.request_disk(vpeak).status != ResourceRequest::Status::Granted) continue;
            reserved = vpeak;
        }
        *w = {WorkKind::Variant, j.idx, jobs[i].get(), j.released, reserved};
        jobs[i]->released++;
        return true;
    }
    if (prep_allowed_locked()) {
        for (size_t i = 0; i < jobs.size(); i++) {
            FileJob& j = *jobs[i];
            if (j.done || j.prep_done || j.prep_running || !j.deferred) continue;
            if (std::chrono::steady_clock::now() < j.defer_until) continue;
            if (j.probe.ok && j.wav_est > 0) {
                j.peak_file = file_peak_bytes(j.wav_est, opts->verify);
                if (rm.request_disk(j.peak_file).status == ResourceRequest::Status::Granted) {
                    j.deferred = false;
                    j.prep_running = true;
                    rm.on_prep_started();
                    prep_active++;
                    *w = {WorkKind::Prep, j.idx, jobs[i].get(), 0};
                    return true;
                }
            }
        }
        size_t i;
        if (find_next_prep_locked(&i)) {
            if (next_prep < jobs.size()) next_prep = i + 1;
            jobs[i]->prep_running = true;
            rm.on_prep_started();
            prep_active++;
            *w = {WorkKind::Prep, jobs[i]->idx, jobs[i].get(), 0};
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// prep_file: подготовка файла (проба, теги, эталонный WAV, список задач)
// ---------------------------------------------------------------------------

void Runner::prep_file(FileJob& j) {
    const Options& opts = *this->opts;
    const auto& fmts = *this->fmts;

    auto release_deferred_budget = [&]() {
        if (j.peak_file > 0) { rm.release_disk(j.peak_file); j.peak_file = 0; }
    };

    if (proc::cancelled() || proc::aborted()) { release_deferred_budget(); return; }
    if (ffprobe.empty() || ffmpeg.empty()) {
        j.summary.path = j.path;
        j.summary.status = "error";
        j.summary.detail = i18n::str("ffprobe/ffmpeg unavailable (bin/ffmpeg/ or PATH)");
        obs::sink()->log(i18n::str("ERROR: ffprobe/ffmpeg unavailable (bin/ffmpeg/ or PATH)\n"));
        release_deferred_budget();
        return;
    }

    j.session = std::make_unique<FileSession>(j.path, tmp);
    std::string ref_wav = j.session->ref_wav_path();
    bool src_decoded = false;

    // Watchdog prep: общий бюджет времени на подготовку файла (аналог encode).
    uint64_t src_ns = util::file_size(j.path);
    uint64_t prep_budget = src_ns / 50000;
    if (prep_budget < 1800) prep_budget = 1800;
    if (prep_budget > 7200) prep_budget = 7200;
    const auto prep_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(prep_budget);
    auto remaining_sec = [&]() -> int {
        auto rem = std::chrono::duration_cast<std::chrono::seconds>(
                       prep_deadline - std::chrono::steady_clock::now())
                       .count();
        return rem > 0 ? (int)rem : 0;
    };
    auto prep_watchdog_error = [&](const std::string& step) {
        j.summary.path = j.path;
        j.summary.status = "error";
        j.summary.detail =
            i18n::fmt("prep watchdog timeout (%s, limit %llu s)", step.c_str(),
                      (unsigned long long)prep_budget);
        if (logger) {
            logger->event({{"type", "file_done"},
                           {"file", j.path},
                           {"status", "error"},
                           {"reason", j.summary.detail}});
        }
        obs::sink()->error_file(j.idx, "ERROR " + j.path + " — " +
                                           j.summary.detail + "\n");
        j.error_reported = true;
    };

    auto prep_t0 = std::chrono::steady_clock::now();
    media::Probe probe = media::probe_file(j.path, ffprobe, &j.kill_requested);
    if (proc::aborted() || j.kill_requested.load(std::memory_order_relaxed)) {
        release_deferred_budget();
        return;
    }
    proc::OutputMonitor prep_mon;
    prep_mon.path = ref_wav;
    prep_mon.stall_timeout_sec = 120;
    prep_mon.hard_timeout_sec = remaining_sec();
    if (!probe.ok) {
        const config::Format* src_fmt = find_source_fmt(probe, j.path, fmts);
        if (src_fmt && src_fmt->id != probe.format_name) {
            DecodeStatus ds = decode_source_native(src_fmt, j.path, ref_wav, 16,
                                                   &j.kill_requested, &prep_mon);
            if (ds == DecodeStatus::NeedsCopy) {
                std::string copy = util::join_path(j.session->dir(), "src_copy." + lower_ext(j.path));
                if (util::copy_file(j.path, copy)) {
                    ds = decode_source_native(src_fmt, copy, ref_wav, 16,
                                              &j.kill_requested, &prep_mon);
                    util::remove_file(copy);
                }
            }
            if (ds == DecodeStatus::Ok) {
                probe = media::probe_file(ref_wav, ffprobe, &j.kill_requested);
                if (proc::aborted() || j.kill_requested.load(std::memory_order_relaxed)) {
                    release_deferred_budget();
                    if (j.session) j.session->cleanup();
                    return;
                }
                if (probe.ok) {
                    probe.format_name = src_fmt->id;
                    probe.size = util::file_size(j.path);
                    src_decoded = true;
                }
            }
        }
        if (!probe.ok && remaining_sec() <= 0) {
            release_deferred_budget();
            if (j.session) j.session->cleanup();
            prep_watchdog_error("probe/native-decode");
            return;
        }
        if (!probe.ok) {
            if (j.session) j.session->cleanup();
            if (proc::aborted() || j.kill_requested.load(std::memory_order_relaxed)) {
                release_deferred_budget();
                return;
            }
            j.summary.path = j.path;
            j.summary.status = "error";
            j.summary.detail = probe.error;
            if (logger) {
                logger->event({{"type", "file_done"},
                               {"file", j.path},
                               {"status", "error"},
                               {"reason", probe.error}});
            }
            obs::sink()->error_file(j.idx, "ERROR " + j.path + " — " + probe.error + "\n");
                j.error_reported = true;
                release_deferred_budget();
                return;
        }
    }
    j.probe = probe;
    if (logger) {
        logger->event({{"type", "file_start"}, {"file", j.path}});
    }
    if (probe.has_video) {
        j.summary.path = j.path;
        j.summary.status = "stopped";
        j.summary.detail = i18n::str("video stream present (not audio)");
        if (logger) {
            logger->event({{"type", "file_done"},
                           {"file", j.path},
                           {"status", "stopped"},
                           {"reason", "video stream present"}});
        }
        obs::sink()->log("SKIP " + j.path + " — " + i18n::str("video stream present (not audio)") + "\n");
        release_deferred_budget();
        return;
    }
    if (!probe.is_lossless() && !opts.allow_lossy) {
        j.summary.path = j.path;
        j.summary.status = "stopped";
        j.summary.detail =
            i18n::fmt("lossy input (codec %s), use --allow-lossy", probe.codec_name.c_str());
        if (logger) {
            logger->event({{"type", "file_done"},
                           {"file", j.path},
                           {"status", "stopped"},
                           {"reason", "lossy input"}});
        }
        obs::sink()->log("SKIP " + j.path + " — " + j.summary.detail + "\n");
        release_deferred_budget();
        return;
    }

    {
        tags::TagSet base = tags::extract_tags(j.path, probe, true);
        tags::TagSet sc;
        std::string sc_err;
        if (tags::read_sidecar(j.path, sc, &sc_err)) {
            j.ts = tags::merge_tags(std::move(base), sc);
        } else {
            j.ts = std::move(base);
        }
    }
    int bits = probe.bits_per_sample;
    if (bits <= 0) bits = 16;
    j.bits = bits;

    if (j.mode == JobMode::Restore && !j.restore_to.empty()) {
        const config::Format* tf = nullptr;
        for (const auto& f : fmts)
            if (f.id == j.restore_to) { tf = &f; break; }
        if (tf && lower_ext(j.path) == tf->extension) {
            release_deferred_budget();
            j.early_ok = true;
            j.summary.path = j.path;
            j.summary.status = "ok";
            j.summary.detail = i18n::fmt("already %s — nothing to do", tf->id.c_str());
            return;
        }
    }

    j.wav_est = estimated_wav_bytes(probe, bits);
    if (j.peak_file == 0) {
        j.peak_file = file_peak_bytes(j.wav_est, opts.verify);
        if (rm.request_disk(j.peak_file).status != ResourceRequest::Status::Granted) {
            j.deferred = true;
            j.peak_file = 0;
            j.defer_until =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            return;
        }
    }

    std::string derr;
    bool decoded = src_decoded;
    prep_mon.hard_timeout_sec = remaining_sec();
    auto dec_t0 = std::chrono::steady_clock::now();
    if (!decoded) {
        const config::Format* src_fmt = find_source_fmt(probe, j.path, fmts);
        if (src_fmt) {
            DecodeStatus ds = decode_source_native(src_fmt, j.path, ref_wav, bits,
                                                   &j.kill_requested, &prep_mon);
            if (ds == DecodeStatus::NeedsCopy) {
                std::string copy = util::join_path(j.session->dir(),
                                                   "src_copy." + lower_ext(j.path));
                if (util::copy_file(j.path, copy)) {
                    ds = decode_source_native(src_fmt, copy, ref_wav, bits,
                                              &j.kill_requested, &prep_mon);
                    util::remove_file(copy);
                }
            }
            decoded = (ds == DecodeStatus::Ok);
        }
    }
    if (!decoded)
        decoded = media::decode_to_wav(j.path, ref_wav, ffmpeg, bits, &derr,
                                       &j.kill_requested, &prep_mon);
    j.decode_wall_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - dec_t0)
                           .count();
    if (proc::aborted() || j.kill_requested.load(std::memory_order_relaxed)) {
        release_deferred_budget();
        if (j.session) j.session->cleanup();
        return;
    }
    if (!decoded && remaining_sec() <= 0) {
        release_deferred_budget();
        if (j.session) j.session->cleanup();
        prep_watchdog_error("decode");
        return;
    }
    if (!decoded) {
        release_deferred_budget();
        if (j.session) j.session->cleanup();
        j.summary.path = j.path;
        j.summary.status = "error";
        j.summary.detail = i18n::str("decode to reference WAV: ") + derr;
        if (logger) {
            logger->event({{"type", "file_done"},
                           {"file", j.path},
                           {"status", "error"},
                           {"reason", j.summary.detail}});
        }
        obs::sink()->error_file(j.idx, "ERROR " + j.path + " — " + j.summary.detail + "\n");
        j.error_reported = true;
        return;
    }
    j.ref_size = util::file_size(ref_wav);

    for (size_t fi = 0; fi < fmts.size(); fi++) {
        if (j.kill_requested.load(std::memory_order_relaxed)) {
            release_deferred_budget();
            if (j.session) j.session->cleanup();
            return;
        }
        const config::Format& f = fmts[fi];
        if (!f.enabled) continue;
        if (!opts.formats.empty() &&
            std::find(opts.formats.begin(), opts.formats.end(), f.id) == opts.formats.end())
            continue;
        if (j.mode == JobMode::Restore && !j.restore_to.empty() &&
            f.id != j.restore_to)
            continue;

        std::string why;
        if (f.channels_min > 0 && probe.channels < f.channels_min)
            why = i18n::fmt("channels %d < %d", probe.channels, f.channels_min);
        else if (f.channels_max > 0 && probe.channels > f.channels_max)
            why = i18n::fmt("channels %d > %d", probe.channels, f.channels_max);
        else if (!f.bit_depth.empty() &&
                 std::find(f.bit_depth.begin(), f.bit_depth.end(), bits) == f.bit_depth.end())
            why = i18n::fmt("bit depth %d not supported", bits);
        else if (f.has_sample_rate &&
                 (probe.sample_rate < f.sample_rate_min || probe.sample_rate > f.sample_rate_max))
            why = i18n::fmt("sample rate %d Hz out of range", probe.sample_rate);
        if (!why.empty()) {
            j.failures.push_back(f.id + ": " + i18n::str("out of caps") + " (" + why + ")");
            j.exclusions.push_back(f.id + ": " + i18n::str("out of caps") + " (" + why + ")");
            j.excluded_fmts.push_back(f.id);
            continue;
        }

        tool::Status st =
            tool::ensure(f, !opts.no_download, "[" + f.id + "] ", &j.kill_requested);
        if (j.kill_requested.load(std::memory_order_relaxed)) {
            release_deferred_budget();
            if (j.session) j.session->cleanup();
            return;
        }
        if (st.path.empty()) {
            j.failures.push_back(f.id + ": " + i18n::str("utility unavailable") + " (" +
                                 st.status + ")");
            j.exclusions.push_back(f.id + ": " + i18n::str("utility unavailable") + " (" +
                                   st.status + ")");
            j.tool_errors++;
            continue;
        }

        Env env;
        env.fmt = &f;
        env.encoder = st.path;
        env.decoder = decoder_path(f, st.path);
        env.bits = j.bits;
        j.envs[f.id] = env;

        for (size_t vi = 0; vi < f.variants.size(); vi++) {
            if (j.mode == JobMode::Restore && vi + 1 < f.variants.size()) continue;
            j.tasks.push_back({fi, vi});
        }
        if (j.mode == JobMode::Restore && !j.restore_to.empty()) break;
    }
    j.prep_wall_ms =
        (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - prep_t0)
            .count();
    j.prep_ok = true;
}

// ---------------------------------------------------------------------------
// run_variant: один вариант (кодирование, валидация, теги, жадный отбор)
// ---------------------------------------------------------------------------

VariantOutcome Runner::run_variant(FileJob& j, size_t task_idx) {
    const Options& opts = *this->opts;
    const TaskDesc& td = j.tasks[task_idx];
    const config::Format& f = (*fmts)[td.fmt_idx];
    const config::Variant& v = f.variants[td.variant_idx];
    const Env& env = j.envs[f.id];

    if (proc::cancelled() || proc::aborted() ||
        j.kill_requested.load(std::memory_order_relaxed))
        return VariantOutcome::Cancelled;

    uint64_t cpu0 = proc::child_cpu_ms() + proc::thread_cpu_ms();
    auto wall0 = std::chrono::steady_clock::now();

    std::string candidate = j.session->candidate_path(f.id, v.id, f.extension);
    util::remove_file(candidate);

    nlohmann::json rec = {
        {"file", j.path},
        {"source_format", j.probe.format_name},
        {"codec_name", j.probe.codec_name},
        {"source_size", j.probe.size},
        {"channels", j.probe.channels},
        {"sample_rate", j.probe.sample_rate},
        {"bits", j.bits},
        {"duration", j.probe.duration},
        {"format", f.id},
        {"variant", v.id},
        {"verify", verify_name(opts.verify)},
        {"prep_wall_ms", j.prep_wall_ms},
        {"decode_wall_ms", j.decode_wall_ms},
    };

    auto record_error = [&](const std::string& err) {
        std::lock_guard<std::mutex> lk(*j.m);
        j.failures.push_back(f.id + "/" + v.id + ": " + err);
        j.variant_errors++;
        rec["status"] = "error";
        rec["error"] = err;
        rec["cpu_ms"] = proc::child_cpu_ms() + proc::thread_cpu_ms() - cpu0;
        rec["wall_ms"] = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - wall0)
                             .count();
        if (logger) {
            logger->event({{"type", "candidate"},
                           {"file", j.path},
                           {"format", f.id},
                           {"variant", v.id},
                           {"status", "error"},
                           {"error", err},
                           {"cpu_ms", rec["cpu_ms"]},
                           {"wall_ms", rec["wall_ms"]}});
        }
        j.stat_records.push_back(std::move(rec));
    };

    proc::OutputMonitor mon;
    mon.path = candidate;
    mon.stall_timeout_sec = 120;
    mon.hard_timeout_sec = env.encode_timeout > 0 ? env.encode_timeout : 1800;
    if (j.ref_size > 0) {
        uint64_t proportional = j.ref_size / 50000;
        if (proportional > (uint64_t)mon.hard_timeout_sec)
            mon.hard_timeout_sec = (int)std::min(proportional, (uint64_t)7200);
    }

    std::string verr = encode_candidate(j.session->ref_wav_path(), candidate, v.args, env,
                                        mon, &j.kill_requested);
    if (verr.empty() && opts.verify == Verify::All)
        verr = validate_candidate(j.session->ref_wav_path(), candidate, env,
                                  &j.kill_requested);
    if (proc::cancelled() || proc::aborted() ||
        j.kill_requested.load(std::memory_order_relaxed)) {
        util::remove_file(candidate);
        return VariantOutcome::Cancelled;
    }
    if (!verr.empty()) {
        util::remove_file(candidate);
        record_error(verr);
        return VariantOutcome::Failed;
    }
    uint64_t size = util::file_size(candidate);
    if (size == 0) {
        util::remove_file(candidate);
        record_error(i18n::str("empty file"));
        return VariantOutcome::Failed;
    }

    FmtPlan fp;
    {
        std::lock_guard<std::mutex> lk(*j.m);
        auto it = j.fmt_plans.find(f.id);
        if (it == j.fmt_plans.end()) {
            fp.plan = tags::plan_tags(j.ts, native_types(f), f, false);
            if (!fp.plan.sidecar.empty()) {
                std::string sc_path = j.session->sidecar_path(f.id);
                util::remove_file(sc_path);
                std::string terr;
                std::string sc_base = util::join_path(j.session->dir(), f.id);
                fp.sidecar_size = tags::write_sidecar(sc_base, fp.plan.sidecar, &terr);
                if (!terr.empty()) {
                    util::remove_file(sc_path);
                    fp.sidecar_size = 0;
                }
                fp.sidecar_path = sc_path;
            }
            j.fmt_plans[f.id] = fp;
        } else {
            fp = it->second;
        }
    }

    uint64_t sidecar = fp.sidecar_size;
    bool has_tags = false;
    std::string terr;
    for (const auto& [gtype, grp] : fp.plan.embed) {
        terr = tags::write_group(candidate, f, gtype, grp);
        if (!terr.empty()) break;
    }
    uint64_t cost = size;
    if (terr.empty() && !fp.plan.embed.empty()) {
        size = util::file_size(candidate);
        has_tags = true;
        cost = size;
    }
    if (terr.empty() && sidecar > 0) cost = size + sidecar;

    Candidate cand;
    cand.format = f.id;
    cand.variant = v.id;
    cand.size = size;
    cand.sidecar = sidecar;
    cand.has_tags = has_tags;
    cand.cost = cost;
    cand.order = task_idx;
    cand.path = candidate;

    if (!terr.empty()) {
        util::remove_file(candidate);
        record_error(i18n::str("tags: ") + terr);
        return VariantOutcome::Failed;
    }

    if (cand.has_tags && j.ts.present) {
        std::string verr2 = tags::validate_groups(candidate, f, fp.plan.embed, ffprobe);
        if (!verr2.empty()) {
            util::remove_file(candidate);
            record_error(i18n::str("tag validation: ") + verr2);
            return VariantOutcome::Failed;
        }
    }

    rec["result_size"] = cand.size;
    rec["sidecar_size"] = cand.sidecar;
    rec["cost"] = cand.cost;
    rec["has_tags"] = cand.has_tags;
    rec["status"] = "ok";
    rec["cpu_ms"] = proc::child_cpu_ms() + proc::thread_cpu_ms() - cpu0;
    rec["wall_ms"] = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - wall0)
                         .count();
    if (logger) {
        logger->event({{"type", "candidate"},
                       {"file", j.path},
                       {"format", f.id},
                       {"variant", v.id},
                       {"status", "ok"},
                       {"size", cand.size},
                       {"sidecar", cand.sidecar},
{"cost", cand.cost},
                        {"cpu_ms", rec["cpu_ms"]},
                        {"wall_ms", rec["wall_ms"]}});
    }

    {
        std::lock_guard<std::mutex> lk(*j.m);
        if (j.mode == JobMode::Restore) {
            if (j.best_valid) util::remove_file(j.best.path);
            j.best = cand;
            j.best_order = cand.order;
            j.best_valid = true;
            j.any_passed = true;
            j.stat_records.push_back(std::move(rec));
            return VariantOutcome::Ok;
        }
        if (cand.cost >= j.probe.size) {
            bool keep_for_target = !j.target_dir.empty();
            if (!keep_for_target) {
                util::remove_file(candidate);
                j.any_passed = true;
                j.stat_records.push_back(std::move(rec));
                return VariantOutcome::Ok;
            }
        }
        bool promote = false;
        if (!j.best_valid) promote = true;
        else if (cand.cost < j.best.cost) promote = true;
        else if (cand.cost == j.best.cost && cand.order < j.best_order) promote = true;
        if (promote) {
            if (j.best_valid) util::remove_file(j.best.path);
            j.best = cand;
            j.best_order = cand.order;
            j.best_valid = true;
        } else {
            util::remove_file(candidate);
        }
        j.any_passed = true;
        j.stat_records.push_back(std::move(rec));
    }
    return VariantOutcome::Ok;
}

// ---------------------------------------------------------------------------
// finalize_file: отчёт, замена исходника, чистка tmp
// ---------------------------------------------------------------------------

void Runner::finalize_file(FileJob& j) {
    const Options& opts = *this->opts;
    j.finalizing.store(true, std::memory_order_relaxed);

    std::unique_lock<std::mutex> lk(*j.m);
    std::vector<nlohmann::json> records = std::move(j.stat_records);
    if (j.cancelled) {
        j.summary.path = j.path;
        j.summary.status = "stopped";
        j.summary.detail = i18n::str("removed from queue");
        discard_job_tmp(j);
        if (logger) {
            logger->event({{"type", "file_done"},
                           {"file", j.path},
                           {"status", "stopped"},
                           {"reason", j.summary.detail}});
        }
        obs::sink()->mark_stopped(j.idx);
        return;
    }
    j.summary.path = j.path;
    j.summary.exclusions = j.exclusions;

    std::string msg;
    char buf[512];

    if (j.summary.status == "error") {
    } else if (j.early_ok) {
        j.summary.original = j.probe.size;
        j.summary.best = j.probe.size;
        j.summary.savings_pct = 0.0;
        j.summary.detail = i18n::str("already in the target format");
        msg = i18n::fmt("OK   %s: already %s — nothing to do\n", j.base.c_str(),
                        j.restore_to.c_str());
        if (!opts.no_stats) stats::append_all(records);
        if (logger) {
            logger->event({{"type", "file_done"},
                           {"file", j.path},
                           {"status", "ok"},
                           {"reason", "already in the target format"}});
        }
    } else if (!j.best_valid) {
        bool hard = j.variant_errors > 0 || j.tool_errors > 0;
        std::string reason =
            j.failures.empty() ? i18n::str("no suitable candidates") : j.failures[0];
        if (hard) {
            j.summary.status = "error";
            j.summary.detail = reason;
            obs::sink()->error_file(j.idx, "ERROR " + j.path + " — " + reason + "\n");
            j.error_reported = true;
            if (!opts.no_stats) stats::append_all(records);
            if (logger) {
                logger->event({{"type", "file_done"},
                               {"file", j.path},
                               {"status", "error"},
                               {"reason", reason}});
            }
        } else {
            double savings = 0.0;
            uint64_t best_cost = j.probe.size;
            j.summary.status = "ok";
            j.summary.detail = reason;
            j.summary.original = j.probe.size;
            j.summary.best = best_cost;
            j.summary.savings_pct = savings;
            msg = "OK   " + j.base + " — " + reason + "\n";
            if (!opts.no_stats) stats::append_all(records);
            if (logger) {
                logger->event({{"type", "file_done"},
                               {"file", j.path},
                               {"status", "ok"},
                               {"reason", reason}});
            }
        }
    } else if (!opts.ignore_errors && j.variant_errors > 0) {
        std::string reason =
            j.failures.empty() ? i18n::str("variant failed") : j.failures[0];
        j.summary.status = "error";
        j.summary.detail = reason;
        obs::sink()->error_file(j.idx, "ERROR " + j.path + " — " + reason + "\n");
        j.error_reported = true;
        if (!opts.no_stats) stats::append_all(records);
        if (logger) {
            logger->event({{"type", "file_done"},
                           {"file", j.path},
                           {"status", "error"},
                           {"reason", reason}});
        }
    } else {
        const Candidate& best = j.best;
        uint64_t best_cost = best.cost;

        std::string winner_fail;
        if (opts.verify == Verify::Winner) {
            auto eit = j.envs.find(best.format);
            std::optional<Env> env_copy;
            if (eit != j.envs.end()) env_copy = eit->second;
            std::string wav_path = j.session->ref_wav_path();
            lk.unlock();
            std::string werr;
            if (!env_copy) {
                werr = "internal: no Env for " + best.format;
            } else {
                werr = validate_candidate(wav_path, best.path, *env_copy,
                                          &j.kill_requested);
            }
            lk.lock();
            if (j.kill_requested.load(std::memory_order_relaxed) ||
                proc::cancelled() || proc::aborted()) {
                j.summary.path = j.path;
                j.summary.status = "stopped";
                j.summary.detail = i18n::str("removed from queue");
                discard_job_tmp(j);
                obs::sink()->mark_stopped(j.idx);
                return;
            }
            if (!werr.empty()) {
                winner_fail = i18n::fmt("winner %s/%s failed verification: %s",
                                        best.format.c_str(), best.variant.c_str(),
                                        werr.c_str());
                j.failures.insert(j.failures.begin(), winner_fail);
            }
        }

        if (!winner_fail.empty()) {
            for (auto& r : records) {
                if (r["format"] == best.format && r["variant"] == best.variant) r["winner"] = true;
            }
            if (!opts.no_stats) stats::append_all(records);
            j.summary.status = "error";
            j.summary.detail = winner_fail;
            obs::sink()->error_file(j.idx, "ERROR " + j.path + " — " + winner_fail + "\n");
            j.error_reported = true;
            if (logger) {
                logger->event({{"type", "file_done"},
                               {"file", j.path},
                               {"status", "error"},
                               {"reason", winner_fail},
                               {"original", j.probe.size},
                               {"best", best_cost},
                               {"format", best.format},
                               {"variant", best.variant}});
            }
        } else {
            double savings = 100.0 * (1.0 - (double)best_cost / (double)j.probe.size);
            if (savings < 0.0 && j.mode != JobMode::Restore) savings = 0.0;
            for (auto& r : records) {
                if (r["format"] == best.format && r["variant"] == best.variant) r["winner"] = true;
            }
            if (!opts.no_stats) stats::append_all(records);

            if (j.mode == JobMode::Restore) {
                snprintf(buf, sizeof(buf), "%s",
                         i18n::fmt("OK   %s: restore to %s/%s (%.1f MB -> %.1f MB, %.1f%%)\n",
                                   j.base.c_str(), best.format.c_str(), best.variant.c_str(),
                                   j.probe.size / 1048576.0, best_cost / 1048576.0, savings).c_str());
            } else {
                snprintf(buf, sizeof(buf), "%s",
                         i18n::fmt("OK   %s: %.1f MB -> %.1f MB (%.1f%%), %s/%s\n", j.base.c_str(),
                                   j.probe.size / 1048576.0, best_cost / 1048576.0, savings,
                                   best.format.c_str(), best.variant.c_str()).c_str());
            }
            msg = buf;

            j.summary.status = "ok";
            j.summary.original = j.probe.size;
            j.summary.best = best_cost;
            j.summary.savings_pct = savings;
            j.summary.best_format = best.format;
            j.summary.best_variant = best.variant;

            if (j.mode == JobMode::Restore) {
                std::string ext = fmt_ext(best.format, *fmts);
                bool delivered = false;
                if (opts.dry_run) {
                    j.summary.detail = i18n::str("dry-run — no write");
                    delivered = true;
                } else if (!j.target_dir.empty()) {
                    std::string rel_dir = util::dir_name(j.rel);
                    std::string dst_dir =
                        rel_dir.empty() ? j.target_dir
                                        : util::join_path(j.target_dir, rel_dir);
                    util::mkdirs(dst_dir);
                    std::string dst = util::join_path(dst_dir, j.base_ne + "." + ext);
                    std::error_code ec;
                    auto pit = j.fmt_plans.find(best.format);
                    std::string sc_src =
                        pit != j.fmt_plans.end()
                            ? pit->second.sidecar_path
                            : best.path + ".tags.zip";
                    lk.unlock();
                    fs::rename(fs::u8path(best.path), fs::u8path(dst), ec);
                    if (ec) {
                        if (util::copy_file(best.path, dst)) {
                            util::remove_file(best.path);
                            ec.clear();
                        }
                    }
                    if (!ec && best.sidecar > 0) {
                        deliver_sidecar(sc_src, true, dst_dir, j.base_ne, &msg);
                    }
                    lk.lock();
                    if (!ec) {
                        j.out_path = dst;
                        j.summary.detail = i18n::str("restored: ") + dst;
                        delivered = true;
                    } else {
                        j.summary.status = "error";
                        j.summary.replacement_error = ec.message();
                        j.summary.detail =
                            i18n::str("restore failed, could not write to ") + dst_dir;
                        msg += i18n::fmt(
                            "      ! could not write the restored file to %s (%s)\n",
                            dst.c_str(), ec.message().c_str());
                    }
                } else if (j.ts.complete) {
                    std::string new_path = util::join_path(j.dir, j.base_ne + "." + ext);
                    std::string tmp_name =
                        util::join_path(j.dir, "." + j.base_ne + ".llao-tmp." + ext);
                    util::remove_file(tmp_name);
                    if (util::copy_file(best.path, tmp_name)) {
                        util::ReplaceResult rr =
                            util::replace_file(j.path, tmp_name, new_path);
                        if (!rr.ok) {
                            j.summary.status = "error";
                            if (!util::remove_file(tmp_name)) {
                                msg += i18n::fmt(
                                    "      ! could not remove the temporary candidate "
                                    "(%s); it was left in place\n",
                                    tmp_name.c_str());
                            }
                            msg += rr.original_lost
                                ? i18n::fmt("      ! COULD NOT REPLACE the file; original "
                                            "removed and the candidate remains at %s (%s)\n",
                                            rr.backup.c_str(), rr.error.c_str())
                                : i18n::fmt("      ! could not replace the file (%s)\n",
                                            rr.error.c_str());
                            j.summary.detail = i18n::str("could not replace the file");
                            j.summary.replacement_error = rr.error;
                        } else {
                            auto pit = j.fmt_plans.find(best.format);
                            std::string sc_src =
                                pit != j.fmt_plans.end() ? pit->second.sidecar_path
                                                         : best.path + ".tags.zip";
                            deliver_sidecar(sc_src, best.sidecar > 0, j.dir, j.base_ne,
                                            &msg);
                            j.out_path = new_path;
                            j.summary.replaced = true;
                            j.summary.detail =
                                i18n::str("replaced in place: ") + best.format + "/" + best.variant;
                            delivered = true;
                        }
                    } else {
                        j.summary.status = "error";
                        msg += i18n::str(
                            "      ! could not copy the candidate into the file folder\n");
                        j.summary.detail =
                            i18n::str("could not copy the candidate into the file folder");
                    }
                } else {
                    msg += i18n::str(
                        "      ! not replaced: the container tags cannot be fully preserved\n");
                    j.summary.detail =
                        i18n::str("container tags cannot be fully preserved — no replacement");
                }
                if (delivered && j.summary.status != "error") {
                    msg += i18n::str("      -> restored");
                    if (!j.target_dir.empty() && !j.out_path.empty())
                        msg += " to " + j.out_path;
                    msg += "\n";
                }
            } else if (!opts.dry_run && j.mode == JobMode::Optimize && !j.target_dir.empty()) {
                std::string ext = fmt_ext(best.format, *fmts);
                std::string rel_dir = util::dir_name(j.rel);
                std::string dst_dir =
                    rel_dir.empty() ? j.target_dir
                                    : util::join_path(j.target_dir, rel_dir);
                util::mkdirs(dst_dir);
                std::string dst = util::join_path(dst_dir, j.base_ne + "." + ext);
                std::error_code ec;
                auto pit = j.fmt_plans.find(best.format);
                std::string sc_src =
                    pit != j.fmt_plans.end() ? pit->second.sidecar_path
                                             : best.path + ".tags.zip";
                lk.unlock();
                fs::rename(fs::u8path(best.path), fs::u8path(dst), ec);
                if (ec) {
                    if (util::copy_file(best.path, dst)) {
                        util::remove_file(best.path);
                        ec.clear();
                    }
                }
                if (!ec && best.sidecar > 0) {
                    deliver_sidecar(sc_src, true, dst_dir, j.base_ne, &msg);
                }
                lk.lock();
                if (!ec) {
                    j.out_path = dst;
                    j.summary.detail = i18n::str("optimized to: ") + dst;
                    msg += i18n::fmt("      -> optimized to %s\n", dst.c_str());
                } else {
                    j.summary.status = "error";
                    j.summary.replacement_error = ec.message();
                    j.summary.detail = i18n::str("could not write to ") + dst_dir;
                    msg += i18n::fmt(
                        "      ! could not write the optimized file to %s (%s)\n",
                        dst.c_str(), ec.message().c_str());
                }
            } else if (!opts.dry_run && best_cost < j.probe.size && j.ts.complete) {
                std::string ext = fmt_ext(best.format, *fmts);
                std::string new_path =
                    util::join_path(j.dir, j.base_ne + "." + ext);
                std::string tmp_name =
                    util::join_path(j.dir, "." + j.base_ne + ".llao-tmp." + ext);
                util::remove_file(tmp_name);
                lk.unlock();
                bool copied = util::copy_file(best.path, tmp_name);
                util::ReplaceResult rr;
                if (copied)
                    rr = util::replace_file(j.path, tmp_name, new_path);
                lk.lock();
                if (copied) {
                    if (!rr.ok) {
                        j.summary.status = "error";
                        if (!util::remove_file(tmp_name)) {
                            msg += i18n::fmt(
                                "      ! could not remove the temporary candidate "
                                "(%s); it was left in place\n",
                                tmp_name.c_str());
                        }
                        if (rr.original_lost) {
                            msg += i18n::fmt(
                                "      ! COULD NOT REPLACE the file; original removed and "
                                "the candidate remains at %s (%s)\n",
                                rr.backup.c_str(), rr.error.c_str());
                            j.summary.detail = i18n::str("could not replace the file");
                            j.summary.replacement_error = rr.error;
                        } else {
                            msg += i18n::fmt("      ! could not replace the file (%s)\n",
                                             rr.error.c_str());
                            j.summary.detail = i18n::str("could not replace the file");
                            j.summary.replacement_error = rr.error;
                        }
                    } else {
                        auto pit = j.fmt_plans.find(best.format);
                        std::string sc_src =
                            pit != j.fmt_plans.end() ? pit->second.sidecar_path
                                                     : best.path + ".tags.zip";
                        deliver_sidecar(sc_src, best.sidecar > 0, j.dir, j.base_ne, &msg);
                        msg += i18n::str("      -> replaced in place: ") + best.format + "/" +
                               best.variant + "\n";
                        j.out_path = new_path;
                        j.summary.replaced = true;
                        j.summary.detail =
                            i18n::str("replaced in place: ") + best.format + "/" + best.variant;
                    }
                } else {
                    j.summary.status = "error";
                    msg += i18n::str("      ! could not copy the candidate into the file folder\n");
                    j.summary.detail =
                        i18n::str("could not copy the candidate into the file folder");
                }
            } else if (best_cost < j.probe.size && !j.ts.complete) {
                msg += i18n::str("      ! not replaced: the container tags cannot be fully preserved\n");
                j.summary.detail = i18n::str("container tags cannot be fully preserved — no replacement");
            } else if (opts.dry_run) {
                j.summary.detail = i18n::str("dry-run — no replacement");
            } else {
                j.summary.detail = i18n::str("size did not decrease — no replacement");
            }

            if (logger) {
                logger->event({{"type", "file_done"},
                               {"file", j.path},
                               {"status", j.summary.status},
                               {"replaced", j.summary.replaced},
                               {"replacement_error", j.summary.replacement_error},
                               {"original", j.probe.size},
                               {"best", best_cost},
                               {"format", best.format},
                               {"variant", best.variant}});
            }
        }
    }

    if (j.best_valid) util::remove_file(j.best.path);
    discard_job_tmp(j);

    if (j.session) {
        if (logger) {
            logger->event({{"type", "tmp_cleanup_warn"}, {"file", j.path}});
        }
        j.session.reset();
    }

    if (!msg.empty()) obs::sink()->log(msg);
    if (j.summary.status == "error") {
        if (!j.error_reported) {
            std::string reason = j.summary.detail.empty()
                                     ? i18n::str("error ignored")
                                     : j.summary.detail;
            obs::sink()->error_file(j.idx, "ERROR " + j.path + " — " + reason + "\n");
            j.error_reported = true;
        }
    } else if (j.summary.status == "stopped") obs::sink()->mark_stopped(j.idx);
    else {
        obs::sink()->end_file(j.idx, j.summary.savings_pct);
        if (!j.out_path.empty()) obs::sink()->out_file(j.idx, j.out_path);
    }
    if (j.summary.status == "error") {
        if (opts.ignore_errors || opts.mode == SessionMode::Daemon) {
            if (j.summary.detail.empty()) j.summary.detail = i18n::str("error ignored");
        } else {
            count_error_locked(j);
            abort.store(true);
            proc::abort_all();
        }
    }
}

// ---------------------------------------------------------------------------
// worker: основной цикл воркера
// ---------------------------------------------------------------------------

void Runner::worker() {
    util::set_thread_below_normal();
    workers_alive++;
    for (;;) {
        Work w;
        {
            std::unique_lock<std::mutex> lk(qm);
            cv.wait_for(lk, std::chrono::milliseconds(50), [&] {
                return proc::cancelled() || shutdown_requested.load() ||
                       variant_launchable_locked() || prep_allowed_locked() ||
                       deferred_retry_locked() ||
                       (opts->mode == SessionMode::OneShot && all_done_locked());
            });
            if (proc::cancelled()) break;
            if (shutdown_requested.load()) break;
            if (opts->mode == SessionMode::OneShot && all_done_locked()) break;
            if (abort.load()) break;
            if (!take_work_locked(&w)) continue;
            if (w.kind == WorkKind::Variant)
                obs::sink()->task(w.idx, w.task, obs::TaskState::Running);
        }

        if (w.kind == WorkKind::Prep) {
            obs::sink()->prep(w.idx);
            std::string perr;
            try {
                prep_file(*w.job);
            } catch (const std::exception& exc) {
                perr = exc.what();
            } catch (...) {
                perr = "unknown exception during prep";
            }
            if (proc::cancelled() || proc::aborted()) break;
            if (!perr.empty()) {
                FileJob& j = *w.job;
                std::lock_guard<std::mutex> jl(*j.m);
                j.summary.path = j.path;
                j.summary.status = "error";
                j.summary.detail = perr;
                        obs::sink()->error_file(w.idx,
                                                 "ERROR [" + j.path + "]: " + perr + "\n");
                        j.error_reported = true;
            }
            bool finish_now = false;
            std::vector<obs::TaskInfo> infos;
            {
                std::lock_guard<std::mutex> lk(qm);
                FileJob& j = *w.job;
                if (j.deferred && j.tasks.empty()) {
                    j.prep_done = false;
                    j.prep_running = false;
                    if (prep_active > 0) prep_active--;
                    cv.notify_all();
                } else {
                    j.prep_done = true;
                    j.prep_running = false;
                    if (prep_active > 0) prep_active--;
                    if (j.tasks.empty() || j.cancelled) {
                        j.done = true;
                        total_done++;
                        finish_now = true;
                    } else {
                        infos.reserve(j.tasks.size());
                        for (auto& td : j.tasks) {
                            const auto& f = (*fmts)[td.fmt_idx];
                            const auto& v = f.variants[td.variant_idx];
                            infos.push_back({f.id, v.id, v.args, v.note});
                        }
                        obs::sink()->set_tasks(w.idx, infos);
                        if (!j.excluded_fmts.empty())
                            obs::sink()->set_excluded(w.idx, j.excluded_fmts);
                    }
                }
                cv.notify_all();
            }
            if (finish_now && !proc::cancelled() && !proc::aborted()) finalize_file(*w.job);
        } else {
            std::string verr;
            VariantOutcome oc = VariantOutcome::Failed;
            try {
                oc = run_variant(*w.job, w.task);
            } catch (const std::exception& exc) {
                verr = exc.what();
            } catch (...) {
                verr = "unknown exception during variant";
            }
            if (proc::cancelled() || proc::aborted()) break;
            if (!opts->ignore_errors && opts->mode != SessionMode::Daemon &&
                (oc == VariantOutcome::Failed || !verr.empty())) {
                count_error(*w.job);
            }
            obs::sink()->task(w.idx, w.task,
                         oc == VariantOutcome::Ok ? obs::TaskState::Ok
                                                   : obs::TaskState::Failed);
            bool last = false;
            {
                std::lock_guard<std::mutex> lk(qm);
                FileJob& j = *w.job;
                if (!verr.empty()) {
                    std::lock_guard<std::mutex> jl(*j.m);
                    j.failures.push_back("variant: " + verr);
                    j.variant_errors++;
                                    obs::sink()->error_file(j.idx,
                                                            "ERROR [" + j.path + "]: " + verr + "\n");
                    j.error_reported = true;
                }
                j.completed++;
                if (w.disk_reserved > 0) {
                    rm.release_disk(w.disk_reserved);
                    w.disk_reserved = 0;
                }
                if (j.completed == j.tasks.size() ||
                    (j.cancelled && j.completed == j.released)) {
                    j.done = true;
                    total_done++;
                    last = true;
                }
                cv.notify_all();
            }
            if (last && !proc::cancelled() && !proc::aborted()) finalize_file(*w.job);
        }
    }
    workers_alive--;
}

}  // namespace optimize
