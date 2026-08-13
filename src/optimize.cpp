#include "optimize.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <set>
#include <thread>

#include "config.h"
#include "i18n.h"
#include "media.h"
#include "proc.h"
#include "report.h"
#include "out.h"
#include "stats.h"
#include "tags.h"
#include "tool.h"
#include "util.h"

namespace optimize {

namespace fs = std::filesystem;
namespace json = nlohmann;

namespace {

std::mutex g_print_mutex;

void print_locked(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_print_mutex);
    out::text(stdout, s);
}

bool is_audio_file(const std::string& path) {
    static const std::set<std::string> exts = {
        "flac", "wv",   "ofr",   "tak", "tta",  "m4a", "alac", "aac", "mp4",
        "ape",  "wma",  "la",    "oga", "ogg",  "opus", "mp3",  "wav", "aif",
        "aiff", "mp2",  "ac3",   "dts", "wavpack", "mka", "mkv", "webm",
    };
    std::string ext = util::to_lower(util::base_name(path));
    size_t dot = ext.find_last_of('.');
    if (dot == std::string::npos) return false;
    return exts.count(ext.substr(dot + 1)) != 0;
}

void collect_files(const std::string& p, std::vector<std::string>& out, std::string* err) {
    if (util::file_exists(p)) {
        if (is_audio_file(p)) out.push_back(p);
        return;
    }
    if (!util::dir_exists(p)) {
        *err = i18n::fmt("no such file or folder: %s", p.c_str());
        return;
    }
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(fs::u8path(p), ec)) {
        if (ec) break;
        if (e.is_regular_file()) {
            std::string f = e.path().u8string();
            if (is_audio_file(f)) out.push_back(f);
        }
    }
}

std::string tmp_dir() { return util::join_path(util::exe_dir(), "tmp"); }

std::string base_no_ext(const std::string& path) {
    std::string b = util::base_name(path);
    size_t dot = b.find_last_of('.');
    if (dot == std::string::npos) return b;
    return b.substr(0, dot);
}

// Короткий хеш полного пути: чтобы tmp-файлы параллельных задач не конфликтовали,
// когда в разных папках лежат файлы с одинаковым именем (FNV-1a 64).
std::string tmp_token(const std::string& path) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : path) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buf[24];
    snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h);
    return buf;
}

// Расширение файла в нижнем регистре (без точки).
static std::string lower_ext(const std::string& path) {
    std::string b = util::base_name(path);
    size_t dot = b.find_last_of('.');
    if (dot == std::string::npos) return "";
    return util::to_lower(b.substr(dot + 1));
}

// Ищет формат источника по имени формата ffprobe и расширению. Возвращает nullptr,
// если формат не из наших конфигов (mp3, wma, …) — тогда декодируем через ffmpeg.
static const config::Format* find_source_fmt(const media::Probe& probe, const std::string& path,
                                             const std::vector<config::Format>& fmts) {
    std::string fn = util::to_lower(probe.format_name);
    std::string ext = lower_ext(path);
    for (const auto& f : fmts)
        if (f.id == fn) return &f;
    for (const auto& f : fmts)
        if (!ext.empty() && ext == util::to_lower(f.extension)) return &f;
    for (const auto& f : fmts)
        if (fn.find(f.id) != std::string::npos) return &f;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Команды
// ---------------------------------------------------------------------------

static std::string subst(const std::string& s, const std::string& key, const std::string& val) {
    std::string r = s;
    size_t p;
    while ((p = r.find(key)) != std::string::npos) r.replace(p, key.size(), val);
    return r;
}

static std::vector<std::string> build_cmd(const std::vector<std::string>& tmpl,
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

// Путь к декодеру формата: для ffmpeg-форматов — тот же ffmpeg; иначе — рядом
// с кодером или из PATH.
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
    for (const auto& c : cands) {
        if (!c.empty() && util::file_exists(c) && util::is_pe(c)) return c;
    }
    return encoder;  // fallback
}

// ---------------------------------------------------------------------------
// Валидация кандидата
// ---------------------------------------------------------------------------

struct Env {
    const config::Format* fmt = nullptr;
    std::string encoder;
    std::string decoder;
    int encode_timeout = 1800;
    int decode_timeout = 1800;
    int verify_timeout = 600;
};

// Возвращает пустую строку при успехе, иначе текст ошибки.
std::string encode_and_validate(const std::string& wav, const std::string& candidate,
                                const std::vector<std::string>& params, const Env& env) {
    const config::Format& f = *env.fmt;
    std::vector<std::string> encode_args =
        build_cmd(f.encode_cmd, env.encoder, wav, candidate, params, f.engine_codec,
                  f.engine_container);
    proc::Result r = proc::run(encode_args, env.encode_timeout);
    if (!r.started) return i18n::str("could not launch the encoder: ") + r.error;
    if (r.timed_out) return i18n::str("encoder exceeded the timeout");
    if (r.exit_code != 0) {
        std::string out = util::trim(r.output);
        return i18n::fmt("encoder returned code %d", r.exit_code) +
               (out.empty() ? "" : ": " + out.substr(0, 2000));
    }
    if (!util::file_exists(candidate)) return i18n::str("encoder did not create the file");

    if (f.verify_kind == "builtin" && !f.verify_cmd.empty()) {
        std::vector<std::string> vargs = build_cmd(f.verify_cmd, env.decoder, candidate, "",
                                                   {}, f.engine_codec, f.engine_container);
        proc::Result vr = proc::run(vargs, env.verify_timeout);
        if (!vr.started || vr.timed_out || vr.exit_code != 0) {
            return i18n::str("built-in verification failed") +
                   (util::trim(vr.output).empty() ? "" : ": " + util::trim(vr.output).substr(0, 1000));
        }
    }

    std::string dec_wav = candidate + ".dec.wav";
    std::vector<std::string> dec_args =
        build_cmd(f.decode_cmd, env.decoder, candidate, dec_wav, {}, f.engine_codec,
                  f.engine_container);
    proc::Result dr = proc::run(dec_args, env.decode_timeout);
    if (!dr.started || dr.timed_out || dr.exit_code != 0) {
        if (util::file_exists(dec_wav)) util::remove_file(dec_wav);
        std::string out = util::trim(dr.output);
        return i18n::fmt("candidate decode failed (code %d)", dr.exit_code) +
               (out.empty() ? "" : ": " + out.substr(0, 1000));
    }
    std::string perr;
    bool same = media::wav_pcm_equal(wav, dec_wav, &perr);
    util::remove_file(dec_wav);
    if (!same) return i18n::str("PCM does not match the source: ") + perr;
    return {};
}

// Расширение формата по id.
static std::string fmt_ext(const std::string& id, const std::vector<config::Format>& fmts) {
    for (const auto& f : fmts)
        if (f.id == id) return f.extension;
    return id;
}

// Нативные типы тегов формата (из tag.system конфига).
static std::vector<tags::TagType> native_types(const config::Format& f) {
    std::vector<tags::TagType> v;
    tags::TagType t = tags::tag_type_from_string(f.tag_system);
    if (t != tags::TagType::unknown) v.push_back(t);
    return v;
}

// ---------------------------------------------------------------------------
// Обработка одного файла
// ---------------------------------------------------------------------------

int process_one(const std::string& path, const Options& opts,
                const std::vector<config::Format>& fmts, report::Logger* logger,
                report::FileSummary* out) {
    std::string ffprobe = media::find_ffprobe();
    std::string ffmpeg = media::find_ffmpeg();
    if (ffprobe.empty() || ffmpeg.empty()) {
        print_locked(i18n::str("ERROR: ffprobe/ffmpeg unavailable (bin/ffmpeg/ or PATH)\n"));
        out->status = "error";
        out->detail = i18n::str("ffprobe/ffmpeg unavailable (bin/ffmpeg/ or PATH)");
        return 1;
    }

    if (logger) {
        logger->event({{"type", "file_start"}, {"file", path}});
    }

    media::Probe probe = media::probe_file(path, ffprobe);
    if (!probe.ok) {
        print_locked("SKIP " + path + " — " + probe.error + "\n");
        out->status = "skip";
        out->detail = probe.error;
        if (logger) {
            logger->event({{"type", "file_done"},
                           {"file", path},
                           {"status", "skip"},
                           {"reason", probe.error}});
        }
        return 0;
    }
    if (probe.has_video) {
        print_locked("SKIP " + path + " — " + i18n::str("video stream present (not audio)") + "\n");
        out->status = "skip";
        out->detail = i18n::str("video stream present (not audio)");
        if (logger) {
            logger->event({{"type", "file_done"},
                           {"file", path},
                           {"status", "skip"},
                           {"reason", "video stream present"}});
        }
        return 0;
    }
    if (!probe.is_lossless() && !opts.allow_lossy) {
        print_locked("SKIP " + path + " — " +
                     i18n::fmt("lossy input (codec %s), use --allow-lossy", probe.codec_name.c_str()) + "\n");
        out->status = "skip";
        out->detail = i18n::fmt("lossy input (codec %s), use --allow-lossy", probe.codec_name.c_str());
        if (logger) {
            logger->event({{"type", "file_done"},
                           {"file", path},
                           {"status", "skip"},
                           {"reason", "lossy input"}});
        }
        return 0;
    }

    tags::TagSet ts = tags::extract_tags(path, probe);
    int bits = probe.bits_per_sample;
    if (bits <= 0) bits = 16;

    std::string base = util::base_name(path);
    std::string dir = util::dir_name(path);
    std::string tmp = tmp_dir();
    util::mkdirs(tmp);
    std::string tok = tmp_token(path);

    std::string ref_wav = util::join_path(tmp, tok + "_" + base_no_ext(path) + ".ref.wav");
    std::string derr;
    // Декод исходника собственным декодером формата: для наших форматов это надёжнее
    // ffmpeg (есть известное расхождение ffmpeg и wvunpack для wavpack 5.9). Иначе — ffmpeg.
    bool decoded = false;
    const config::Format* src_fmt = find_source_fmt(probe, path, fmts);
    if (src_fmt) {
        // Для декода источника скачивание не запускаем: только кэш/PATH, иначе ffmpeg.
        tool::Status sst = tool::ensure(*src_fmt, false, "[" + src_fmt->id + "] ");
        if (!sst.path.empty()) {
            Env senv;
            senv.fmt = src_fmt;
            senv.encoder = sst.path;
            senv.decoder = decoder_path(*src_fmt, sst.path);
            std::vector<std::string> sargs =
                build_cmd(src_fmt->decode_cmd, senv.decoder, path, ref_wav, {},
                          src_fmt->engine_codec, src_fmt->engine_container);
            proc::Result dr = proc::run(sargs, senv.decode_timeout);
            decoded = dr.started && !dr.timed_out && dr.exit_code == 0 &&
                      util::file_exists(ref_wav);
            if (!decoded) util::remove_file(ref_wav);
        }
    }
    if (!decoded) decoded = media::decode_to_wav(path, ref_wav, ffmpeg, bits, &derr);
    if (!decoded) {
        print_locked("SKIP " + path + " — " + i18n::str("decode to reference WAV: ") + derr + "\n");
        out->status = "skip";
        out->detail = i18n::str("decode to reference WAV: ") + derr;
        if (logger) {
            logger->event({{"type", "file_done"},
                           {"file", path},
                           {"status", "skip"},
                           {"reason", out->detail}});
        }
        return 0;
    }

    // Кандидаты
    std::vector<Candidate> passed;
    std::vector<std::string> failures;
    std::vector<json::json> stat_records;

    for (const auto& f : fmts) {
        if (!f.enabled) continue;
        if (!opts.formats.empty() &&
            std::find(opts.formats.begin(), opts.formats.end(), f.id) == opts.formats.end())
            continue;

        // caps
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
            failures.push_back(f.id + ": " + i18n::str("out of caps") + " (" + why + ")");
            out->exclusions.push_back(f.id + ": " + i18n::str("out of caps") + " (" + why + ")");
            continue;
        }

        tool::Status st = tool::ensure(f, !opts.no_download, "[" + f.id + "] ");
        if (st.path.empty()) {
            failures.push_back(f.id + ": " + i18n::str("utility unavailable") + " (" + st.status + ")");
            out->exclusions.push_back(f.id + ": " + i18n::str("utility unavailable") + " (" + st.status + ")");
            continue;
        }

        Env env;
        env.fmt = &f;
        env.encoder = st.path;
        env.decoder = decoder_path(f, st.path);

        for (const auto& variant : f.variants) {
            std::string cand_name = tok + "_" + base_no_ext(path) + "." + f.id + "." + variant.id + "." +
                                    f.extension;
            std::string candidate = util::join_path(tmp, cand_name);
            util::remove_file(candidate);

            json::json rec = {
                {"file", path},
                {"source_format", probe.format_name},
                {"codec_name", probe.codec_name},
                {"source_size", probe.size},
                {"channels", probe.channels},
                {"sample_rate", probe.sample_rate},
                {"bits", bits},
                {"duration", probe.duration},
                {"format", f.id},
                {"variant", variant.id},
            };

            std::string verr = encode_and_validate(ref_wav, candidate, variant.args, env);
            if (!verr.empty()) {
                util::remove_file(candidate);
                failures.push_back(f.id + "/" + variant.id + ": " + verr);
                rec["status"] = "error";
                rec["error"] = verr;
                if (logger) {
                    logger->event({{"type", "candidate"},
                                   {"file", path},
                                   {"format", f.id},
                                   {"variant", variant.id},
                                   {"status", "error"},
                                   {"error", verr}});
                }
                stat_records.push_back(std::move(rec));
                continue;
            }
            uint64_t size = util::file_size(candidate);
            if (size == 0) {
                util::remove_file(candidate);
                failures.push_back(f.id + "/" + variant.id + ": " + i18n::str("empty file"));
                rec["status"] = "error";
                rec["error"] = i18n::str("empty file");
                stat_records.push_back(std::move(rec));
                continue;
            }

            // Теги: план встраивания/сайдкара (сжатие: без мержа групп).
            Candidate cand;
            cand.format = f.id;
            cand.variant = variant.id;
            cand.size = size;
            tags::TagPlan plan = tags::plan_tags(ts, native_types(f), f.tag_caps, false);
            std::string terr;
            uint64_t sidecar = 0;
            bool has_tags = false;
            for (const auto& [gtype, grp] : plan.embed) {
                terr = tags::write_group(candidate, f.id, gtype, grp);
                if (!terr.empty()) break;
            }
            uint64_t cost = size;
            if (terr.empty() && !plan.embed.empty()) {
                size = util::file_size(candidate);
                has_tags = true;
                cost = size;
            }
            if (terr.empty() && !plan.sidecar.empty()) {
                sidecar = tags::write_sidecar(candidate, plan.sidecar, &terr);
                if (terr.empty()) cost = size + sidecar;
            }
            cand.size = size;
            cand.sidecar = sidecar;
            cand.has_tags = has_tags;
            cand.cost = cost;
            if (!terr.empty()) {
                util::remove_file(candidate);
                if (cand.sidecar) util::remove_file(candidate + ".tags.zip");
                failures.push_back(f.id + "/" + variant.id + ": " + i18n::str("tags: ") + terr);
                rec["status"] = "error";
                rec["error"] = i18n::str("tags: ") + terr;
                if (logger) {
                    logger->event({{"type", "candidate"},
                                   {"file", path},
                                   {"format", f.id},
                                   {"variant", variant.id},
                                   {"status", "error"},
                                   {"error", terr}});
                }
                stat_records.push_back(std::move(rec));
                continue;
            }

            // Финальная проверка тегов
            if (cand.has_tags && ts.present) {
                std::string verr2 = tags::validate_groups(candidate, f.id, plan.embed, ffprobe);
                if (!verr2.empty()) {
                    util::remove_file(candidate);
                    if (cand.sidecar) util::remove_file(candidate + ".tags.zip");
                    failures.push_back(f.id + "/" + variant.id + ": " + i18n::str("tag validation: ") + verr2);
                    rec["status"] = "error";
                    rec["error"] = i18n::str("tag validation: ") + verr2;
                    stat_records.push_back(std::move(rec));
                    continue;
                }
            }

            rec["result_size"] = cand.size;
            rec["sidecar_size"] = cand.sidecar;
            rec["cost"] = cand.cost;
            rec["has_tags"] = cand.has_tags;
            rec["status"] = "ok";
            if (logger) {
                logger->event({{"type", "candidate"},
                               {"file", path},
                               {"format", f.id},
                               {"variant", variant.id},
                               {"status", "ok"},
                               {"size", cand.size},
                               {"sidecar", cand.sidecar},
                               {"cost", cand.cost}});
            }
            stat_records.push_back(std::move(rec));
            passed.push_back(cand);
        }
    }

    util::remove_file(ref_wav);

    // Отчёт
    std::string msg;
    char buf[512];
    if (passed.empty()) {
        snprintf(buf, sizeof(buf), "%s", i18n::fmt("SKIP %s — no suitable candidates", base.c_str()).c_str());
        msg = buf;
        if (!failures.empty()) msg += " (" + failures[0] + ")";
        msg += "\n";
        print_locked(msg);

        out->path = path;
        out->status = "skip";
        out->detail = failures.empty() ? i18n::str("no suitable candidates") : failures[0];
        if (!opts.no_stats) stats::append_all(stat_records);
        if (logger) {
            logger->event({{"type", "file_done"},
                           {"file", path},
                           {"status", "skip"},
                           {"reason", out->detail}});
        }
        return 0;
    }

    std::sort(passed.begin(), passed.end(),
              [](const Candidate& a, const Candidate& b) { return a.cost < b.cost; });

    const Candidate& best = passed.front();
    uint64_t best_cost = best.cost;
    for (auto& r : stat_records) {
        if (r["format"] == best.format && r["variant"] == best.variant) r["winner"] = true;
    }
    if (!opts.no_stats) stats::append_all(stat_records);

    snprintf(buf, sizeof(buf), "%s",
             i18n::fmt("OK   %s: %.1f MB -> %.1f MB (%.1f%%), %s/%s\n", base.c_str(),
                       probe.size / 1048576.0, best_cost / 1048576.0,
                       100.0 * (1.0 - (double)best_cost / probe.size), best.format.c_str(),
                       best.variant.c_str()).c_str());
    msg = buf;

    out->path = path;
    out->status = "ok";
    out->original = probe.size;
    out->best = best_cost;
    out->savings_pct = 100.0 * (1.0 - (double)best_cost / probe.size);
    out->best_format = best.format;
    out->best_variant = best.variant;

    // Замена на месте
    if (!opts.dry_run && best_cost < probe.size && ts.complete) {
        std::string ext = fmt_ext(best.format, fmts);
        std::string src_name =
            util::join_path(tmp, tok + "_" + base_no_ext(path) + "." + best.format + "." + best.variant + "." +
                                      ext);
        std::string new_path = util::join_path(dir, base_no_ext(path) + "." + ext);
        std::string tmp_name = util::join_path(dir, "." + base_no_ext(path) + ".llao-tmp." + ext);
        if (util::copy_file(src_name, tmp_name)) {
            if (util::remove_file(path)) {
                std::error_code ec;
                fs::rename(fs::u8path(tmp_name), fs::u8path(new_path), ec);
                if (ec) {
                    msg += i18n::fmt("      ! could not rename (the file remained in the folder as %s)\n",
                                      util::base_name(tmp_name).c_str());
                } else {
                    if (best.sidecar) {
                        std::string sc_src = src_name + ".tags.zip";
                        std::string sc_dst = util::join_path(dir, base_no_ext(path) + ".tags.zip");
                        if (!util::copy_file(sc_src, sc_dst))
                            msg += i18n::str("      ! could not copy the sidecar (tags) next to the file\n");
                    }
                    msg += i18n::str("      -> replaced in place: ") + best.format + "/" + best.variant + "\n";
                    out->replaced = true;
                    out->detail = i18n::str("replaced in place: ") + best.format + "/" + best.variant;
                }
            } else {
                util::remove_file(tmp_name);
                msg += i18n::str("      ! could not replace the file (access denied)\n");
                out->detail = i18n::str("could not replace the file (access denied)");
            }
        } else {
            msg += i18n::str("      ! could not copy the candidate into the file folder\n");
            out->detail = i18n::str("could not copy the candidate into the file folder");
        }
    } else if (best_cost < probe.size && !ts.complete) {
        msg += i18n::str("      ! not replaced: the container tags cannot be fully preserved\n");
        out->detail = i18n::str("container tags cannot be fully preserved — no replacement");
    } else if (opts.dry_run) {
        out->detail = i18n::str("dry-run — no replacement");
    } else {
        out->detail = i18n::str("size did not decrease — no replacement");
    }

    print_locked(msg);
    if (logger) {
        logger->event({{"type", "file_done"},
                       {"file", path},
                       {"status", out->status},
                       {"replaced", out->replaced},
                       {"original", probe.size},
                       {"best", best_cost},
                       {"format", best.format},
                       {"variant", best.variant}});
    }
    return 0;
}

}  // namespace

int run(const Options& opts) {
    std::vector<config::Format> fmts;
    try {
        fmts = config::load_all();
    } catch (const std::exception& exc) {
        out::error("ERROR: %s\n", exc.what());
        return 1;
    }

    // Ранжирование форматов по накопленной статистике: наиболее вероятные
    // победители конвертируются первыми. Форматы без статистики — в конце,
    // в порядке конфигов (stable_sort сохраняет их взаимный порядок).
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

    std::vector<std::string> files;
    for (const auto& p : opts.inputs) {
        std::string err;
        collect_files(p, files, &err);
        if (!err.empty()) out::error("WARNING: %s\n", err.c_str());
    }
    if (files.empty()) {
        out::error("ERROR: no audio files found\n");
        return 1;
    }

    int jobs = opts.jobs;
    if (jobs <= 0) {
        unsigned hw = std::thread::hardware_concurrency();
        jobs = hw > 0 ? (int)hw : 1;
    }
    if (jobs > (int)files.size()) jobs = (int)files.size();

    util::mkdirs(tmp_dir());

    // Журнал runs/*.jsonl — только в режиме отладки (stats.json накапливается всегда).
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
                      {"report", opts.report_path}});
    }

    std::atomic<int> failed{0};
    std::atomic<size_t> next{0};
    std::mutex m;
    size_t done = 0;
    std::vector<report::FileSummary> summaries;
    std::vector<std::thread> threads;
    std::function<void()> worker = [&]() {
        for (;;) {
            size_t idx;
            {
                std::lock_guard<std::mutex> lk(m);
                if (next >= files.size()) break;
                idx = next++;
            }
            report::FileSummary fs;
            int rc = 0;
            try {
                rc = process_one(files[idx], opts, fmts, &logger, &fs);
            } catch (const std::exception& exc) {
                out::error("ERROR [%s]: %s\n", files[idx].c_str(), exc.what());
                fs.status = "error";
                fs.detail = exc.what();
                rc = 1;
            }
            if (rc != 0) failed++;
            {
                std::lock_guard<std::mutex> lk(m);
                done++;
                summaries.push_back(std::move(fs));
            }
        }
    };
    for (int i = 0; i < jobs; i++) threads.emplace_back(worker);
    for (auto& t : threads) t.join();

    // Убираем временные файлы (эталонные WAV и кандидаты).
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::u8path(tmp_dir()), ec)) {
        if (ec) break;
        if (e.is_regular_file()) fs::remove(e.path(), ec);
    }

    if (logger.ok()) {
        logger.event({{"type", "run_end"},
                      {"done", done},
                      {"failed", failed.load()}});
    }

    if (!opts.report_path.empty()) {
        std::string rp = opts.report_path;
        if (util::dir_exists(rp) || util::ends_with(rp, "/") || util::ends_with(rp, "\\")) {
            util::mkdirs(rp);
            rp = util::join_path(rp, "llao-report-" + report::timestamp() + ".txt");
        }
        report::write_report(rp, summaries);
    }

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

    out::print("Done: %zu files processed, errors: %d\n", done, failed.load());
    return failed.load() > 0 ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Восстановление (restore): декод -> пережатие в целевой формат -> теги обратно
// ---------------------------------------------------------------------------

static int restore_one(const std::string& path, const config::Format& target,
                       const config::Variant& variant, bool no_download, bool allow_lossy,
                       const std::vector<config::Format>& fmts) {
    std::string ffprobe = media::find_ffprobe();
    std::string ffmpeg = media::find_ffmpeg();
    if (ffprobe.empty() || ffmpeg.empty()) {
        print_locked(i18n::str("ERROR: ffprobe/ffmpeg unavailable (bin/ffmpeg/ or PATH)\n"));
        return 1;
    }

    media::Probe probe = media::probe_file(path, ffprobe);
    if (!probe.ok) {
        print_locked("SKIP " + path + " — " + probe.error + "\n");
        return 0;
    }
    if (probe.has_video) {
        print_locked("SKIP " + path + " — " + i18n::str("video stream present (not audio)") + "\n");
        return 0;
    }
    if (!probe.is_lossless() && !allow_lossy) {
        print_locked("SKIP " + path + " — " +
                     i18n::fmt("lossy input (codec %s), use --allow-lossy", probe.codec_name.c_str()) + "\n");
        return 0;
    }
    if (lower_ext(path) == util::to_lower(target.extension)) {
        print_locked("SKIP " + path + " — " + i18n::str("already the format ") + target.id + "\n");
        return 0;
    }

    std::string base = util::base_name(path);
    std::string base_ne = base_no_ext(path);
    std::string dir = util::dir_name(path);
    std::string tmp = tmp_dir();
    util::mkdirs(tmp);
    std::string tok = tmp_token(path);

    // Теги: источник — встроенные группы + sidecar (объединение групп).
    tags::TagSet embed_ts = tags::extract_tags(path, probe);
    tags::TagSet ts;
    std::string serr;
    tags::TagSet side_ts;
    bool have_sidecar = tags::read_sidecar(path, side_ts, &serr);
    if (have_sidecar) ts = tags::merge_tags(std::move(embed_ts), side_ts);
    else ts = std::move(embed_ts);

    int bits = probe.bits_per_sample;
    if (bits <= 0) bits = 16;

    // Декод исходника собственным декодером формата: для наших форматов это надёжнее
    // ffmpeg (есть известное расхождение ffmpeg и wvunpack для wavpack 5.9). Иначе — ffmpeg.
    std::string src_wav = util::join_path(tmp, tok + "_" + base_ne + ".src.wav");
    const config::Format* src_fmt = find_source_fmt(probe, path, fmts);
    std::string dec_err;
    bool decoded = false;
    if (src_fmt && src_fmt->id != target.id) {
        // Для декода источника скачивание не запускаем: только кэш/PATH, иначе ffmpeg.
        tool::Status sst = tool::ensure(*src_fmt, false, "[" + src_fmt->id + "] ");
        if (!sst.path.empty()) {
            Env senv;
            senv.fmt = src_fmt;
            senv.encoder = sst.path;
            senv.decoder = decoder_path(*src_fmt, sst.path);
            std::vector<std::string> sargs =
                build_cmd(src_fmt->decode_cmd, senv.decoder, path, src_wav, {},
                          src_fmt->engine_codec, src_fmt->engine_container);
            proc::Result dr = proc::run(sargs, senv.decode_timeout);
            decoded = dr.started && !dr.timed_out && dr.exit_code == 0 &&
                      util::file_exists(src_wav);
            if (!decoded) util::remove_file(src_wav);
        }
    }
    if (!decoded) decoded = media::decode_to_wav(path, src_wav, ffmpeg, bits, &dec_err);
    if (!decoded) {
        util::remove_file(src_wav);
        print_locked("SKIP " + path + " — " + i18n::str("decode to reference WAV: ") + dec_err + "\n");
        return 0;
    }

    tool::Status st = tool::ensure(target, !no_download, "[" + target.id + "] ");
    if (st.path.empty()) {
        util::remove_file(src_wav);
        print_locked(i18n::fmt("SKIP %s — utility %s unavailable (%s)\n", path.c_str(),
                                target.id.c_str(), st.status.c_str()));
        return 0;
    }

    Env env;
    env.fmt = &target;
    env.encoder = st.path;
    env.decoder = decoder_path(target, st.path);

    std::string cand_name = tok + "_" + base_ne + ".restore." + target.extension;
    std::string candidate = util::join_path(tmp, cand_name);
    util::remove_file(candidate);

    std::string verr = encode_and_validate(src_wav, candidate, variant.args, env);
    util::remove_file(src_wav);
    if (!verr.empty()) {
        util::remove_file(candidate);
        print_locked("SKIP " + path + " — " + verr + "\n");
        return 0;
    }
    uint64_t size = util::file_size(candidate);
    if (size == 0) {
        util::remove_file(candidate);
        print_locked("SKIP " + path + " — " + i18n::str("empty file") + "\n");
        return 0;
    }

    // Теги: план встраивания/сайдкара (восстановление: с мержем групп).
    tags::TagPlan plan = tags::plan_tags(ts, native_types(target), target.tag_caps, true);
    std::string terr;
    uint64_t sidecar = 0;
    bool has_tags = false;
    for (const auto& [gtype, grp] : plan.embed) {
        terr = tags::write_group(candidate, target.id, gtype, grp);
        if (!terr.empty()) break;
    }
    if (terr.empty()) {
        if (!plan.embed.empty()) {
            size = util::file_size(candidate);
            has_tags = true;
        }
        if (!plan.sidecar.empty()) {
            sidecar = tags::write_sidecar(candidate, plan.sidecar, &terr);
        }
    }
    if (!terr.empty()) {
        util::remove_file(candidate);
        print_locked("SKIP " + path + " — " + i18n::str("tags: ") + terr + "\n");
        return 0;
    }
    if (has_tags && ts.present) {
        std::string v2 = tags::validate_groups(candidate, target.id, plan.embed, ffprobe);
        if (!v2.empty()) {
            util::remove_file(candidate);
            if (sidecar) util::remove_file(candidate + ".tags.zip");
            print_locked("SKIP " + path + " — " + i18n::str("tag validation: ") + v2 + "\n");
            return 0;
        }
    }

    // Замена на месте.
    std::string new_path = util::join_path(dir, base_ne + "." + target.extension);
    std::string tmp_name = util::join_path(dir, "." + base_ne + ".llao-restore-tmp." + target.extension);
    if (!util::copy_file(candidate, tmp_name)) {
        print_locked(i18n::fmt("!    %s — could not copy the candidate into the folder\n", base.c_str()));
        return 0;
    }
    if (!util::remove_file(path)) {
        util::remove_file(tmp_name);
        print_locked(i18n::fmt("!    %s — could not replace the file (access denied)\n", base.c_str()));
        return 0;
    }
    std::error_code ec;
    fs::rename(fs::u8path(tmp_name), fs::u8path(new_path), ec);
    if (ec) {
        print_locked(i18n::fmt("!    %s — could not rename (the .llao-restore-tmp file remains)\n", base.c_str()));
        return 0;
    }

    std::string old_sc = util::join_path(dir, base_ne + ".tags.zip");
    if (sidecar > 0) {
        if (util::copy_file(candidate + ".tags.zip", old_sc)) {
            print_locked(i18n::fmt("OK   %s -> %s (%s), tags in sidecar\n", base.c_str(),
                                    target.id.c_str(), target.extension.c_str()));
        } else {
            print_locked(i18n::fmt("!    %s -> %s, but the sidecar was not copied\n", base.c_str(),
                                    target.id.c_str()));
        }
    } else {
        util::remove_file(old_sc);
        print_locked(i18n::fmt("OK   %s -> %s (%s): %d KB, tags embedded\n", base.c_str(),
                                target.id.c_str(), target.extension.c_str(), (int)(size / 1024)));
    }
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
        variant = &target->variants.back();  // последний = максимальное сжатие
    }
    if (!variant) {
        out::error("ERROR: format '%s' has no compression variants\n", target->id.c_str());
        return 1;
    }

    std::vector<std::string> files;
    for (const auto& p : opts.inputs) {
        std::string err;
        collect_files(p, files, &err);
        if (!err.empty()) out::error("WARNING: %s\n", err.c_str());
    }
    if (files.empty()) {
        out::error("ERROR: no audio files found\n");
        return 1;
    }

    int jobs = opts.jobs;
    if (jobs <= 0) {
        unsigned hw = std::thread::hardware_concurrency();
        jobs = hw > 0 ? (int)hw : 1;
    }
    if (jobs > (int)files.size()) jobs = (int)files.size();

    util::mkdirs(tmp_dir());
    out::print("Restoring to %s (variant %s): %zu files, threads: %d\n", target->id.c_str(),
                variant->id.c_str(), files.size(), jobs);

    std::atomic<int> failed{0};
    std::atomic<size_t> next{0};
    std::mutex m;
    size_t done = 0;
    std::vector<std::thread> threads;
    std::function<void()> worker = [&]() {
        for (;;) {
            size_t idx;
            {
                std::lock_guard<std::mutex> lk(m);
                if (next >= files.size()) break;
                idx = next++;
            }
            try {
                if (restore_one(files[idx], *target, *variant, opts.no_download, opts.allow_lossy,
                                fmts) != 0)
                    failed++;
            } catch (const std::exception& exc) {
                out::error("ERROR [%s]: %s\n", files[idx].c_str(), exc.what());
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

    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::u8path(tmp_dir()), ec)) {
        if (ec) break;
        if (e.is_regular_file()) fs::remove(e.path(), ec);
    }

    out::print("Done: %zu files restored, errors: %d\n", done, failed.load());
    return failed.load() > 0 ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Список вариантов сжатия из formats/*.json (без конвертации)
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
