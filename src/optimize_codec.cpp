#include "optimize_internal.h"

#include <chrono>
#include <filesystem>
#include <thread>

#include "i18n.h"
#include "media.h"
#include "obs.h"
#include "proc.h"
#include "tool.h"
#include "util.h"

namespace optimize {

// ---------------------------------------------------------------------------
// FileSession: RAII-обёртка для tmp-файлов одного исходного файла.
// ---------------------------------------------------------------------------

FileSession::FileSession(const std::string& original_path, const std::string& tmp_base)
    : path_(original_path) {
    dir_ = util::join_path(tmp_base, session_cookie());
    util::mkdirs(dir_);
}

FileSession::~FileSession() { cleanup(); }

FileSession::FileSession(FileSession&& o) noexcept
    : dir_(std::move(o.dir_)), path_(std::move(o.path_)) {
    o.dir_.clear();
}

FileSession& FileSession::operator=(FileSession&& o) noexcept {
    if (this != &o) {
        cleanup();
        dir_ = std::move(o.dir_);
        path_ = std::move(o.path_);
        o.dir_.clear();
    }
    return *this;
}

std::string FileSession::ref_wav_path() const {
    return util::join_path(dir_, "ref.wav");
}

std::string FileSession::candidate_path(const std::string& fmt_id,
                                        const std::string& variant_id,
                                        const std::string& ext) const {
    return util::join_path(dir_, fmt_id + "." + variant_id + "." + ext);
}

std::string FileSession::sidecar_path(const std::string& fmt_id) const {
    return util::join_path(dir_, fmt_id + ".tags.zip");
}

void FileSession::cleanup() {
    if (!dir_.empty()) {
        std::error_code ec;
        fs::remove_all(fs::u8path(dir_), ec);
        for (int attempt = 0; attempt < 3 && ec; attempt++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1200 * (attempt + 1)));
            ec.clear();
            fs::remove_all(fs::u8path(dir_), ec);
        }
        if (ec) {
            obs::sink()->log("WARN: FileSession cleanup failed for '" + dir_ +
                             "': " + ec.message() + "\n");
        } else {
            obs::sink()->log("[tmp] session dir '" + dir_ + "' removed\n");
        }
        dir_.clear();
    }
}

// ---------------------------------------------------------------------------
// DiskBudget
// ---------------------------------------------------------------------------

bool DiskBudget::try_reserve(const std::string& tmp_path, uint64_t bytes) {
    std::lock_guard<std::mutex> lk(m);
    uint64_t free = util::disk_free_bytes(tmp_path);
    uint64_t avail = free > (reserved + min_free) ? free - reserved - min_free : 0;
    if (bytes > avail) return false;
    reserved += bytes;
    return true;
}

void DiskBudget::release(uint64_t bytes) {
    std::lock_guard<std::mutex> lk(m);
    reserved = (bytes >= reserved) ? 0 : reserved - bytes;
}

// ---------------------------------------------------------------------------
// ResourceManager
// ---------------------------------------------------------------------------

ResourceManager::ResourceManager(const std::string& tmp_path, int max_workers, uint64_t min_free)
    : tmp_path_(tmp_path), max_workers_(max_workers) {
    disk_.min_free = min_free;
}

ResourceRequest ResourceManager::request_disk(uint64_t bytes) {
    ResourceRequest req;
    req.disk_bytes = bytes;
    req.status = disk_.try_reserve(tmp_path_, bytes)
                     ? ResourceRequest::Status::Granted
                     : ResourceRequest::Status::Deferred;
    return req;
}

void ResourceManager::release_disk(uint64_t bytes) { disk_.release(bytes); }

bool ResourceManager::can_start_new_file(size_t /*next_prep*/, int prep_active_r,
                                          const std::vector<std::unique_ptr<FileJob>>& jobs,
                                          bool aborted) const {
    if (aborted) return false;
    if (proc::cancelled()) return false;
    if (prep_active_r >= max_workers_) return false;
    bool has_idle = false;
    for (const auto& jp : jobs) { const FileJob& j = *jp;
        if (j.done || !j.prep_done || j.cancelled) continue;
        if (j.released == 0) { has_idle = true; break; }
    }
    if (has_idle) {
        for (const auto& jp : jobs) { const FileJob& j = *jp;
            if (j.done || !j.prep_done || j.cancelled) continue;
            if (j.released < j.tasks.size()) return false;
        }
    }
    for (const auto& jp : jobs) { const FileJob& j = *jp;
        if (!j.done && !j.prep_done) return true;
    }
    return false;
}

bool ResourceManager::can_start_new_variant(const FileJob& j, int window,
                                             bool aborted) const {
    if (aborted) return false;
    if (j.done || !j.prep_done) return false;
    if (j.cancelled) return false;
    if (j.released >= j.tasks.size()) return false;
    if (j.released - j.completed >= (size_t)window) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Команды кодека
// ---------------------------------------------------------------------------

std::string subst(const std::string& s, const std::string& key, const std::string& val) {
    std::string r = s;
    size_t p;
    while ((p = r.find(key)) != std::string::npos) r.replace(p, key.size(), val);
    return r;
}

std::vector<std::string> build_cmd(const std::vector<std::string>& tmpl,
                                   const std::string& binary, const std::string& in,
                                   const std::string& out,
                                   const std::vector<std::string>& params,
                                   const std::string& codec, const std::string& container) {
    std::vector<std::string> args;
    for (size_t i = 0; i < tmpl.size(); i++) {
        if (i == 0) {
            args.push_back(binary);
            continue;
        }
        const std::string& a = tmpl[i];
        if (a == "{params}") {
            for (const auto& p : params) args.push_back(p);
            continue;
        }
        std::string r = subst(a, "{input}", in);
        r = subst(r, "{output}", out);
        r = subst(r, "{codec}", codec);
        r = subst(r, "{container}", container);
        args.push_back(r);
    }
    return args;
}

std::string decoder_path(const config::Format& fmt, const std::string& encoder) {
    if (fmt.engine_kind == "ffmpeg") return encoder;
    std::string name = fmt.engine_decoder_executable;
    if (name.empty()) return encoder;
    std::vector<std::string> cands;
    std::string enc_dir = util::dir_name(encoder);
    if (!enc_dir.empty()) {
        cands.push_back(util::join_path(enc_dir, name + ".exe"));
        cands.push_back(util::join_path(enc_dir, name));
    }
    cands.push_back(util::find_in_path(name));
    cands.push_back(util::find_in_path(name + ".exe"));
    for (const auto& c : cands) {
        if (c.empty() || !util::file_exists(c)) continue;
#ifdef _WIN32
        if (!util::is_pe(c)) continue;
#endif
        return c;
    }
    return encoder;
}

const char* pcm_codec(int bits) {
    if (bits > 24) return "pcm_s32le";
    if (bits > 16) return "pcm_s24le";
    return "pcm_s16le";
}

// ---------------------------------------------------------------------------
// Декод исходника + валидация кандидата
// ---------------------------------------------------------------------------

DecodeStatus decode_source_native(const config::Format* src_fmt, const std::string& path,
                                  const std::string& out_wav, int bits,
                                  const std::atomic<bool>* kill) {
    if (!src_fmt) return DecodeStatus::Failed;
    tool::Status sst = tool::ensure(*src_fmt, false, "[" + src_fmt->id + "] ", kill);
    if (kill && kill->load(std::memory_order_relaxed)) return DecodeStatus::Failed;
    if (sst.path.empty()) return DecodeStatus::Failed;

    std::string src = path;
    for (auto& c : src) if (c == '\\') c = '/';

    std::string input_path = src;
    bool alias_created = false;
    std::string alias_path;
#ifndef _WIN32
    alias_path = util::join_path(util::dir_name(out_wav),
                                 "src_link." + lower_ext(src));
    if (util::create_readonly_symlink(src, alias_path)) {
        input_path = alias_path;
        alias_created = true;
    } else if (util::create_hardlink(src, alias_path)) {
        input_path = alias_path;
        alias_created = true;
    }
#endif

    Env senv;
    senv.fmt = src_fmt;
    senv.encoder = sst.path;
    senv.decoder = decoder_path(*src_fmt, sst.path);
    senv.bits = bits;
    std::vector<std::string> sargs;
    if (src_fmt->engine_kind == "ffmpeg") {
        sargs = {senv.decoder, "-y", "-loglevel", "error", "-i", input_path,
                 "-c:a", pcm_codec(bits), out_wav};
    } else {
        sargs = build_cmd(src_fmt->decode_cmd, senv.decoder, input_path, out_wav, {},
                          src_fmt->engine_codec, src_fmt->engine_container);
    }
    proc::Result dr = proc::run(sargs, senv.decode_timeout, "", {}, kill);
    bool ok = dr.started && !dr.timed_out && dr.exit_code == 0 && util::file_exists(out_wav);
    if (!ok) util::remove_file(out_wav);

    if (alias_created) util::remove_file(alias_path);

    if (ok) return DecodeStatus::Ok;
    return alias_created ? DecodeStatus::Failed : DecodeStatus::NeedsCopy;
}

std::string encode_candidate(const std::string& wav, const std::string& candidate,
                             const std::vector<std::string>& params, const Env& env,
                             const proc::OutputMonitor& monitor,
                             const std::atomic<bool>* kill) {
    const config::Format& f = *env.fmt;
    std::vector<std::string> encode_args =
        build_cmd(f.encode_cmd, env.encoder, wav, candidate, params, f.engine_codec,
                  f.engine_container);
    int effective_timeout = monitor.hard_timeout_sec > 0 ? 0 : env.encode_timeout;
    proc::Result r = proc::run(encode_args, effective_timeout, "", monitor, kill);
    if (!r.started) return i18n::str("could not launch the encoder: ") + r.error;
    if (r.stalled) return i18n::str("encoder stalled (no progress)");
    if (r.timed_out) return i18n::str("encoder exceeded the timeout");
    if (r.exit_code != 0) {
        std::string out = util::trim(r.output);
        return i18n::fmt("encoder returned code %d", r.exit_code) +
               (out.empty() ? "" : ": " + out);
    }
    if (!util::file_exists(candidate)) return i18n::str("encoder did not create the file");
    return {};
}

bool remove_dec_wav(const std::string& p) {
    if (util::remove_file(p)) return true;
    for (int attempt = 0; attempt < 4; attempt++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2500 * (attempt + 1)));
        if (!util::file_exists(p)) return true;
        if (util::remove_file(p)) return true;
    }
    return false;
}

std::string validate_candidate(const std::string& wav, const std::string& candidate,
                               const Env& env, const std::atomic<bool>* kill) {
    const config::Format& f = *env.fmt;
    if (f.verify_kind == "builtin" && !f.verify_cmd.empty()) {
        std::vector<std::string> vargs = build_cmd(f.verify_cmd, env.decoder, candidate, "",
                                                   {}, f.engine_codec, f.engine_container);
        proc::Result vr = proc::run(vargs, env.verify_timeout, "", {}, kill);
        if (!vr.started || vr.timed_out || vr.exit_code != 0) {
            return i18n::str("built-in verification failed") +
                   (util::trim(vr.output).empty() ? "" : ": " + util::trim(vr.output));
        }
    }

    std::string dec_wav = candidate + ".dec.wav";
    std::vector<std::string> dec_args;
    if (f.engine_kind == "ffmpeg") {
        dec_args = {env.decoder, "-y", "-loglevel", "error", "-i", candidate,
                    "-c:a", pcm_codec(env.bits), dec_wav};
    } else {
        dec_args = build_cmd(f.decode_cmd, env.decoder, candidate, dec_wav, {}, f.engine_codec,
                             f.engine_container);
    }
    proc::Result dr = proc::run(dec_args, env.decode_timeout, "", {}, kill);
    if (!dr.started || dr.timed_out || dr.exit_code != 0) {
        std::string out = util::trim(dr.output);
        obs::sink()->log("[v] decode FAIL candidate='" + candidate + "' rc=" +
                         std::to_string(dr.exit_code) + " timed_out=" +
                         (dr.timed_out ? "1" : "0") + " out='" + out + "'\n");
        if (util::file_exists(dec_wav)) {
            bool rm = remove_dec_wav(dec_wav);
            obs::sink()->log("[v] decode-fail cleanup '" + dec_wav + "' => " +
                             (rm ? "removed" : "REMOVE_FAILED") + "\n");
        }
        return i18n::fmt("candidate decode failed (code %d)", dr.exit_code) +
               (out.empty() ? "" : ": " + out);
    }
    std::string perr;
    bool same = media::wav_data_compare(wav, dec_wav, &perr);
    if (!remove_dec_wav(dec_wav)) {
        obs::sink()->log("WARN: could not remove " + dec_wav +
                         " (kept until job end)\n");
    } else {
        obs::sink()->log("[v] compare '" + candidate + "' => " +
                         (same ? "identical" : "DIFFERENT") + ", rm '" + dec_wav +
                         "' ok\n");
    }
    if (!same) return i18n::str("PCM does not match the source: ") + perr;
    return {};
}

bool deliver_sidecar(const std::string& sc_src, bool need,
                     const std::string& dir, const std::string& base_ne,
                     std::string* note) {
    const std::string dst = util::join_path(dir, base_ne + ".tags.zip");
    const std::string tmp = util::join_path(dir, "." + base_ne + ".llao-tmp.tags.zip");
    util::remove_file(tmp);
    if (!need) {
        if (util::file_exists(dst)) {
            if (!util::remove_file(dst)) {
                *note += i18n::str(
                    "      ! could not remove the stale sidecar (tags) next to the file\n");
                return false;
            }
        }
        return true;
    }
    if (!util::copy_file(sc_src, tmp)) {
        *note += i18n::str("      ! could not copy the sidecar (tags) next to the file\n");
        return false;
    }
    util::ReplaceResult rr = util::replace_file(dst, tmp, dst);
    if (!rr.ok) {
        if (rr.original_lost)
            *note += i18n::fmt(
                "      ! could not put the sidecar in place (remains at %s: %s)\n",
                rr.backup.c_str(), rr.error.c_str());
        else
            *note += i18n::str(
                "      ! could not replace the sidecar (tags) next to the file\n");
        return false;
    }
    return true;
}

}  // namespace optimize
