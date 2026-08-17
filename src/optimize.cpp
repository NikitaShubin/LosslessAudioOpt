#include "optimize.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
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
#include "status.h"
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

// Число потоков из опций: целое jobs — как есть, вещественное — множитель числа
// ядер; 0/отрицательное — авто (2× ядра). Не меньше 1.
int resolve_jobs(double jobs, bool jobs_float) {
    unsigned hw = std::thread::hardware_concurrency();
    if (hw < 1) hw = 1;
    double n;
    if (jobs <= 0) {
        n = hw * 2.0;  // авто
    } else if (jobs_float) {
        n = hw * jobs;  // множитель ядер
    } else {
        n = jobs;  // точное число потоков
    }
    int j = n <= 1.0 ? 1 : (int)(n + 0.5);
    return j;
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

// Декод исходника собственным декодером формата (без скачивания: только кэш/PATH).
// Возвращает true, если out_wav создан; иначе false и out_wav удалён.
// Для наших форматов родной декодер надёжнее ffmpeg (есть известное расхождение
// ffmpeg и wvunpack для wavpack 5.9), а некоторые форматы (OptimFROG высоких
// пресетов) ffmpeg вообще не разбирает — родной декод используется и как фолбэк probe.
static bool decode_source_native(const config::Format* src_fmt, const std::string& path,
                                 const std::string& out_wav) {
    if (!src_fmt) return false;
    tool::Status sst = tool::ensure(*src_fmt, false, "[" + src_fmt->id + "] ");
    if (sst.path.empty()) return false;
    Env senv;
    senv.fmt = src_fmt;
    senv.encoder = sst.path;
    senv.decoder = decoder_path(*src_fmt, sst.path);
    std::vector<std::string> sargs =
        build_cmd(src_fmt->decode_cmd, senv.decoder, path, out_wav, {},
                  src_fmt->engine_codec, src_fmt->engine_container);
    proc::Result dr = proc::run(sargs, senv.decode_timeout);
    bool ok = dr.started && !dr.timed_out && dr.exit_code == 0 && util::file_exists(out_wav);
    if (!ok) util::remove_file(out_wav);
    return ok;
}

// Кодирование кандидата (без валидации). Возвращает пустую строку при успехе,
// иначе текст ошибки. При успехе candidate существует и непуст.
std::string encode_candidate(const std::string& wav, const std::string& candidate,
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
    return {};
}

// Полная валидация кандидата: builtin-проверка формата (flac -t и т.п.) + декод
// и побитовое сравнение PCM с эталонным WAV (потоковое, без загрузки в память).
// Возвращает пустую строку при успехе, иначе текст ошибки.
std::string validate_candidate(const std::string& wav, const std::string& candidate,
                               const Env& env) {
    const config::Format& f = *env.fmt;
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
    bool same = media::wav_data_compare(wav, dec_wav, &perr);
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

// Текстовое имя режима верификации (для stats.json).
static const char* verify_name(Verify v) {
    switch (v) {
        case Verify::All: return "all";
        case Verify::Winner: return "winner";
        default: return "none";
    }
}

// Нативные типы тегов формата (из tag.system конфига).
static std::vector<tags::TagType> native_types(const config::Format& f) {
    std::vector<tags::TagType> v;
    tags::TagType t = tags::tag_type_from_string(f.tag_system);
    if (t != tags::TagType::unknown) v.push_back(t);
    return v;
}

// ---------------------------------------------------------------------------
// Планировщик: prep (подготовка файла) + варианты (сжатие) в пуле потоков.
// Очередь строго упорядочена: сначала все варианты файла 0, затем файла 1 и т.д.
// Окно W = jobs ограничивает число задач одного файла "в полёте", поэтому prep
// следующего файла начинается, когда у текущего остаётся меньше W вариантов —
// ядра не простаивают, а tmp не разрастается.
// ---------------------------------------------------------------------------

struct FmtPlan {
    tags::TagPlan plan;
    uint64_t sidecar_size = 0;  // 0 — sidecar не нужен (или не удался)
    std::string sidecar_path;   // tmp/<tok>_<base>.<fmt>.tags.zip (общий для формата)
};

struct TaskDesc {
    size_t fmt_idx = 0;
    size_t variant_idx = 0;
};

// Итог одного варианта: успех, ошибка или отмена (для статусбара).
enum class VariantOutcome { Ok, Failed, Cancelled };

struct FileJob {
    size_t idx = 0;
    std::string path;
    std::string base;
    std::string base_ne;
    std::string dir;
    std::string tok;

    // --- планировщик (под g_qm) ---
    std::vector<TaskDesc> tasks;
    size_t released = 0;
    size_t completed = 0;
    bool prep_done = false;
    bool done = false;

    // --- подготовка (один поток prep, до выпуска задач) ---
    bool prep_ok = false;
    std::string ref_wav;
    uint64_t ref_size = 0;   // размер эталонного WAV (оценка размера dec.wav задачи)
    int bits = 16;
    media::Probe probe;
    tags::TagSet ts;
    std::map<std::string, Env> envs;             // fmt_id -> кодер/декодер
    std::map<std::string, FmtPlan> fmt_plans;    // fmt_id -> план тегов + sidecar

    // --- результаты (под m) ---
    std::unique_ptr<std::mutex> m;
    bool any_passed = false;
    bool best_valid = false;
    Candidate best;
    size_t best_order = SIZE_MAX;
    std::vector<json::json> stat_records;
    std::vector<std::string> failures;
    std::vector<std::string> exclusions;
    int variant_errors = 0;   // операционные сбои вариантов (кодирование/валидация/теги)
    int tool_errors = 0;      // утилиты форматов недоступны
    report::FileSummary summary;
};

struct Runner {
    const Options* opts = nullptr;
    const std::vector<config::Format>* fmts = nullptr;
    report::Logger* logger = nullptr;
    std::string ffprobe;
    std::string ffmpeg;
    std::string tmp;
    int window = 1;
    size_t n_files = 0;

    // --- кэш ресурсов для адаптивного окна (обновляется не чаще 10 с) ---
    std::chrono::steady_clock::time_point res_last{};
    uint64_t tmp_budget = 8ull << 30;  // бюджет tmp: min(8 ГБ, свободно/4), не меньше 1 ГБ
    uint64_t ram_budget = 0;           // бюджет RAM: 50% доступной памяти

    std::mutex qm;
    std::condition_variable cv;
    size_t next_prep = 0;
    int prep_active = 0;  // число выполняющихся prep (могут идти параллельно)
    size_t total_done = 0;
    std::vector<FileJob> jobs;
    std::atomic<int> failed{0};
    std::atomic<bool> abort{false};  // при ошибке файла без --ignore-errors: прекращаем прогон

    bool all_done_locked() const { return total_done == n_files; }

    // Есть ли хоть один готовый файл с невыпущенными задачами (для cv-предиката;
    // бюджеты окна проверяет сам take_work_locked).
    bool variant_pending_locked() const {
        for (size_t i = 0; i < n_files; i++) {
            const FileJob& j = jobs[i];
            if (!j.done && j.prep_done && j.released < j.tasks.size()) return true;
        }
        return false;
    }

    // Задач «в полёте» (выпущено, но не завершено) по всем файлам.
    size_t in_flight_locked() const {
        size_t rel = 0, cpl = 0;
        for (const auto& j : jobs) {
            rel += j.released;
            cpl += j.completed;
        }
        return rel - cpl;
    }

    // След одной задачи «в полёте»: dec.wav (~размера эталона) + кандидат (~ref/2).
    static uint64_t task_footprint(uint64_t ref_size) {
        return ref_size + ref_size / 2;
    }

    // Обновляет кэш ресурсов (диск/RAM) не чаще 10 с.
    void refresh_resources_locked() {
        auto now = std::chrono::steady_clock::now();
        if (res_last != std::chrono::steady_clock::time_point{} &&
            now - res_last < std::chrono::seconds(10))
            return;
        uint64_t free = util::disk_free_bytes(tmp);
        uint64_t b = free / 4;
        if (b > (8ull << 30)) b = 8ull << 30;       // не больше 8 ГБ
        if (b < (1ull << 30)) b = 1ull << 30;       // но и не меньше 1 ГБ
        tmp_budget = b;
        uint64_t ram = util::avail_ram_bytes();
        ram_budget = ram > (2ull << 30) ? ram / 2 : 0;  // 50% доступной памяти
        res_last = now;
    }

    // Окно (лимит «в полёте») для одного файла: число потоков, бюджет tmp
    // (свободное место на диске) и бюджет RAM по следу задачи.
    uint64_t file_cap_locked(const FileJob& j) const {
        uint64_t cap = (uint64_t)window;
        if (j.ref_size > 0) {
            uint64_t foot = task_footprint(j.ref_size);
            uint64_t by_tmp = tmp_budget / foot;
            if (by_tmp < 1) by_tmp = 1;
            cap = std::min<uint64_t>(cap, by_tmp);
            if (ram_budget > 0) {
                uint64_t by_ram = ram_budget / foot;
                if (by_ram < 1) by_ram = 1;
                cap = std::min<uint64_t>(cap, by_ram);
            }
        }
        return cap;
    }

    // Prep следующего файла: файлы готовятся строго по порядку (next_prep++),
    // параллельно — не больше числа потоков (window). Распаковка начинается
    // сразу, как только окно освобождается, а не привязана к числу ещё не
    // запущенных файлов: при window >= n_files все файлы готовятся одновременно.
    // Prep идёт в фоне, чтобы ядра не простаивали в ожидании распаковки;
    // выпуск вариантов — всегда по приоритету файлов.
    bool prep_allowed_locked() const {
        if (abort.load()) return false;
        if (proc::cancelled()) return false;
        if (next_prep >= n_files) return false;
        if (prep_active >= window) return false;
        return true;
    }

    enum class WorkKind { None, Prep, Variant };
    struct Work {
        WorkKind kind = WorkKind::None;
        size_t idx = 0;
        size_t task = 0;
    };

    // Вызывается под qm. Свободный воркер берёт очередной незанятый вариант
    // самого раннего готового файла (строгий приоритет по индексу: даже если
    // более поздний файл распаковался раньше, как только более ранний готов —
    // его варианты обрабатываются в первую очередь). Задачи не выпускаются,
    // пока не освободится место в окне (лимиты потоков/диска/RAM). Когда
    // варианты брать нечего — берётся prep следующего файла.
    bool take_work_locked(Work* w) {
        if (abort.load()) return false;
        refresh_resources_locked();
        size_t in_flight = in_flight_locked();
        for (size_t i = 0; i < n_files; i++) {
            const FileJob& j = jobs[i];
            if (j.done || !j.prep_done) continue;
            if (j.released >= j.tasks.size()) continue;
            if (in_flight >= file_cap_locked(j)) break;  // окно занято — варианты не выпускаем
            *w = {WorkKind::Variant, i, j.released};
            jobs[i].released++;
            return true;
        }
        if (prep_allowed_locked()) {
            size_t i = next_prep++;
            prep_active++;
            *w = {WorkKind::Prep, i, 0};
            return true;
        }
        return false;
    }

    // --- подготовка файла: проба, теги, эталонный WAV, список задач ---
    void prep_file(FileJob& j) {
        const Options& opts = *this->opts;
        const auto& fmts = *this->fmts;

        if (proc::cancelled()) return;
        if (ffprobe.empty() || ffmpeg.empty()) {
            j.summary.path = j.path;
            j.summary.status = "error";
            j.summary.detail = i18n::str("ffprobe/ffmpeg unavailable (bin/ffmpeg/ or PATH)");
            status::log(i18n::str("ERROR: ffprobe/ffmpeg unavailable (bin/ffmpeg/ or PATH)\n"));
            return;
        }

        j.ref_wav = util::join_path(tmp, j.tok + "_" + j.base_ne + ".ref.wav");
        bool src_decoded = false;

        media::Probe probe = media::probe_file(j.path, ffprobe);
        if (!probe.ok) {
            // ffprobe не смог прочитать файл (напр. OptimFROG высоких пресетов ffmpeg
            // не разбирает). Пробуем родной декодер формата по расширению: расжимаем в
            // WAV и пробируем уже WAV — его ffprobe читает всегда.
            const config::Format* src_fmt = find_source_fmt(probe, j.path, fmts);
            if (src_fmt && decode_source_native(src_fmt, j.path, j.ref_wav)) {
                probe = media::probe_file(j.ref_wav, ffprobe);
                if (probe.ok) {
                    probe.format_name = src_fmt->id;
                    probe.size = util::file_size(j.path);
                    src_decoded = true;
                }
            }
            if (!probe.ok) {
                util::remove_file(j.ref_wav);
                j.summary.path = j.path;
                j.summary.status = "error";
                j.summary.detail = probe.error;
                if (logger) {
                    logger->event({{"type", "file_done"},
                                   {"file", j.path},
                                   {"status", "error"},
                                   {"reason", probe.error}});
                }
                status::error("ERROR " + j.path + " — " + probe.error + "\n");
                    return;
            }
        }
        j.probe = probe;
        if (logger) {
            logger->event({{"type", "file_start"}, {"file", j.path}});
        }
        if (probe.has_video) {
            j.summary.path = j.path;
            j.summary.status = "skip";
            j.summary.detail = i18n::str("video stream present (not audio)");
            if (logger) {
                logger->event({{"type", "file_done"},
                               {"file", j.path},
                               {"status", "skip"},
                               {"reason", "video stream present"}});
            }
            status::log("SKIP " + j.path + " — " + i18n::str("video stream present (not audio)") + "\n");
            return;
        }
        if (!probe.is_lossless() && !opts.allow_lossy) {
            j.summary.path = j.path;
            j.summary.status = "skip";
            j.summary.detail =
                i18n::fmt("lossy input (codec %s), use --allow-lossy", probe.codec_name.c_str());
            if (logger) {
                logger->event({{"type", "file_done"},
                               {"file", j.path},
                               {"status", "skip"},
                               {"reason", "lossy input"}});
            }
            status::log("SKIP " + j.path + " — " + j.summary.detail + "\n");
            return;
        }

        j.ts = tags::extract_tags(j.path, probe);
        int bits = probe.bits_per_sample;
        if (bits <= 0) bits = 16;
        j.bits = bits;

        // Декод исходника собственным декодером формата: для наших форматов это надёжнее
        // ffmpeg (есть известное расхождение ffmpeg и wvunpack для wavpack 5.9). Иначе — ffmpeg.
        std::string derr;
        bool decoded = src_decoded;
        if (!decoded) {
            const config::Format* src_fmt = find_source_fmt(probe, j.path, fmts);
            if (src_fmt) decoded = decode_source_native(src_fmt, j.path, j.ref_wav);
        }
        if (!decoded) decoded = media::decode_to_wav(j.path, j.ref_wav, ffmpeg, bits, &derr);
        if (!decoded) {
            util::remove_file(j.ref_wav);
            j.summary.path = j.path;
            j.summary.status = "error";
            j.summary.detail = i18n::str("decode to reference WAV: ") + derr;
            if (logger) {
                logger->event({{"type", "file_done"},
                               {"file", j.path},
                               {"status", "error"},
                               {"reason", j.summary.detail}});
            }
            status::error("ERROR " + j.path + " — " + j.summary.detail + "\n");
            return;
        }
        j.ref_size = util::file_size(j.ref_wav);

        // Задачи: формат + вариант. Порядок = детерминированный тай-брейк.
        for (size_t fi = 0; fi < fmts.size(); fi++) {
            const config::Format& f = fmts[fi];
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
                j.failures.push_back(f.id + ": " + i18n::str("out of caps") + " (" + why + ")");
                j.exclusions.push_back(f.id + ": " + i18n::str("out of caps") + " (" + why + ")");
                continue;
            }

            tool::Status st = tool::ensure(f, !opts.no_download, "[" + f.id + "] ");
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
            j.envs[f.id] = env;

            for (size_t vi = 0; vi < f.variants.size(); vi++) {
                j.tasks.push_back({fi, vi});
            }
        }
        j.prep_ok = true;
    }

    // --- один вариант: кодирование, валидация, теги, жадный отбор ---
    VariantOutcome run_variant(FileJob& j, size_t task_idx) {
        const Options& opts = *this->opts;
        const TaskDesc& td = j.tasks[task_idx];
        const config::Format& f = (*fmts)[td.fmt_idx];
        const config::Variant& v = f.variants[td.variant_idx];
        const Env& env = j.envs[f.id];

        if (proc::cancelled()) return VariantOutcome::Cancelled;

        std::string cand_name = j.tok + "_" + j.base_ne + "." + f.id + "." + v.id + "." +
                                f.extension;
        std::string candidate = util::join_path(tmp, cand_name);
        util::remove_file(candidate);

        json::json rec = {
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
        };

        auto record_error = [&](const std::string& err) {
            std::lock_guard<std::mutex> lk(*j.m);
            j.failures.push_back(f.id + "/" + v.id + ": " + err);
            j.variant_errors++;
            rec["status"] = "error";
            rec["error"] = err;
            if (logger) {
                logger->event({{"type", "candidate"},
                               {"file", j.path},
                               {"format", f.id},
                               {"variant", v.id},
                               {"status", "error"},
                               {"error", err}});
            }
            j.stat_records.push_back(std::move(rec));
        };

        // В режиме All каждый кандидат полностью проверяется здесь.
        // Winner/None — только кодирование; победитель валидируется в finalize_file
        // (режим Winner) или не проверяется вовсе (None).
        std::string verr = encode_candidate(j.ref_wav, candidate, v.args, env);
        if (verr.empty() && opts.verify == Verify::All)
            verr = validate_candidate(j.ref_wav, candidate, env);
        if (proc::cancelled()) {
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

        // План тегов и sidecar: зависит от (файл, формат), не от варианта.
        // Sidecar пишется один раз на формат и общий для всех его вариантов.
        FmtPlan fp;
        {
            std::lock_guard<std::mutex> lk(*j.m);
            auto it = j.fmt_plans.find(f.id);
            if (it == j.fmt_plans.end()) {
                fp.plan = tags::plan_tags(j.ts, native_types(f), f.tag_caps, false);
                if (!fp.plan.sidecar.empty()) {
                    std::string sc_base = util::join_path(tmp, j.tok + "_" + j.base_ne + "." +
                                                                f.id);
                    util::remove_file(sc_base + ".tags.zip");
                    std::string terr;
                    fp.sidecar_size = tags::write_sidecar(sc_base, fp.plan.sidecar, &terr);
                    if (!terr.empty()) {
                        util::remove_file(sc_base + ".tags.zip");
                        fp.sidecar_size = 0;
                    }
                    fp.sidecar_path = sc_base + ".tags.zip";
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
            terr = tags::write_group(candidate, f.id, gtype, grp);
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

        // Финальная проверка тегов
        if (cand.has_tags && j.ts.present) {
            std::string verr2 = tags::validate_groups(candidate, f.id, fp.plan.embed, ffprobe);
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
        if (logger) {
            logger->event({{"type", "candidate"},
                           {"file", j.path},
                           {"format", f.id},
                           {"variant", v.id},
                           {"status", "ok"},
                           {"size", cand.size},
                           {"sidecar", cand.sidecar},
                           {"cost", cand.cost}});
        }

        // Жадный отбор: лучший держим, проигравших удаляем сразу.
        {
            std::lock_guard<std::mutex> lk(*j.m);
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

    // --- финализация файла: отчёт, замена исходника, чистка tmp ---
    // Вызывается ровно один раз (потоком, завершившим последнюю задачу).
    void finalize_file(FileJob& j) {
        const Options& opts = *this->opts;

        std::lock_guard<std::mutex> lk(*j.m);
        std::vector<json::json> records = std::move(j.stat_records);
        j.summary.path = j.path;
        j.summary.exclusions = j.exclusions;

        std::string msg;
        char buf[512];

        if (j.summary.status == "error") {
            // ошибка уже зафиксирована (ffprobe/ffmpeg, probe, декод, исключение)
        } else if (!j.any_passed) {
            // Победителя нет. Операционные сбои (упали все варианты, утилиты недоступны) —
            // это ошибка файла; «чистый» skip остаётся только для намеренных причин
            // (нет подходящих кандидатов из-за caps и т.п.).
            bool hard = j.variant_errors > 0 || j.tool_errors > 0;
            std::string reason =
                j.failures.empty() ? i18n::str("no suitable candidates") : j.failures[0];
            if (hard) {
                j.summary.status = "error";
                j.summary.detail = reason;
                status::error("ERROR " + j.path + " — " + reason + "\n");
                if (!opts.no_stats) stats::append_all(records);
                if (logger) {
                    logger->event({{"type", "file_done"},
                                   {"file", j.path},
                                   {"status", "error"},
                                   {"reason", reason}});
                }
            } else {
                j.summary.status = "skip";
                j.summary.detail = reason;
                msg = "SKIP " + j.base + " — " + reason + "\n";
                if (!opts.no_stats) stats::append_all(records);
                if (logger) {
                    logger->event({{"type", "file_done"},
                                   {"file", j.path},
                                   {"status", "skip"},
                                   {"reason", reason}});
                }
            }
        } else {
            const Candidate& best = j.best;
            uint64_t best_cost = best.cost;

            // Режим Winner: кандидаты при отборе не проверялись — валидируем победителя
            // перед заменой. Провал — это ошибка файла (а не «неудачный вариант»): исходник
            // не заменяется, разбор причин обязателен.
            std::string winner_fail;
            if (opts.verify == Verify::Winner) {
                auto eit = j.envs.find(best.format);
                std::string werr =
                    eit == j.envs.end() ? "internal: no Env for " + best.format
                                        : validate_candidate(j.ref_wav, best.path, eit->second);
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
                status::error("ERROR " + j.path + " — " + winner_fail + "\n");
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
                for (auto& r : records) {
                    if (r["format"] == best.format && r["variant"] == best.variant) r["winner"] = true;
                }
                if (!opts.no_stats) stats::append_all(records);

                snprintf(buf, sizeof(buf), "%s",
                         i18n::fmt("OK   %s: %.1f MB -> %.1f MB (%.1f%%), %s/%s\n", j.base.c_str(),
                                   j.probe.size / 1048576.0, best_cost / 1048576.0, savings,
                                   best.format.c_str(), best.variant.c_str()).c_str());
                msg = buf;

                j.summary.status = "ok";
                j.summary.original = j.probe.size;
                j.summary.best = best_cost;
                j.summary.savings_pct = savings;
                j.summary.best_format = best.format;
                j.summary.best_variant = best.variant;

                // Замена на месте: исходник трогаем только здесь.
                if (!opts.dry_run && best_cost < j.probe.size && j.ts.complete) {
                    std::string ext = fmt_ext(best.format, *fmts);
                    std::string new_path =
                        util::join_path(j.dir, j.base_ne + "." + ext);
                    std::string tmp_name =
                        util::join_path(j.dir, "." + j.base_ne + ".llao-tmp." + ext);
                    std::string bak_name =
                        util::join_path(j.dir, "." + j.base_ne + ".llao-bak." + ext);
                    if (util::copy_file(best.path, tmp_name)) {
                        // Безопасная замена: оригинал переносится в .bak, кандидат — на место
                        // оригинала (с новым расширением формата); при сбое — rollback.
                        // Перезапись исходника через copy исключена.
                        util::ReplaceResult rr =
                            util::replace_file(j.path, tmp_name, bak_name, new_path);
                        if (!rr.ok) {
                            // Замена сорвалась — это ошибка файла: считается в failed
                            // и останавливает прогон (см. ниже, блок по status == "error").
                            j.summary.status = "error";
                            if (!util::remove_file(tmp_name)) {
                                msg += i18n::fmt(
                                    "      ! could not remove the temporary candidate "
                                    "(%s); it was left in place\n",
                                    tmp_name.c_str());
                            }
                            if (rr.original_lost) {
                                msg += i18n::fmt(
                                    "      ! COULD NOT REPLACE the file; the original was NOT "
                                    "restored in place and is saved as %s (%s)\n",
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
                            if (best.sidecar > 0) {
                                auto pit = j.fmt_plans.find(best.format);
                                std::string sc_src =
                                    pit != j.fmt_plans.end() ? pit->second.sidecar_path
                                                             : best.path + ".tags.zip";
                                std::string sc_dst =
                                    util::join_path(j.dir, j.base_ne + ".tags.zip");
                                if (!util::copy_file(sc_src, sc_dst))
                                    msg += i18n::str(
                                        "      ! could not copy the sidecar (tags) next to the file\n");
                            }
                            msg += i18n::str("      -> replaced in place: ") + best.format + "/" +
                                   best.variant + "\n";
                            j.summary.replaced = true;
                            j.summary.detail =
                                i18n::str("replaced in place: ") + best.format + "/" + best.variant;
                        }
                    } else {
                        // Кандидата не удалось даже скопировать в папку файла —
                        // это ошибка файла (см. блок по status == "error").
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

        // Чистка временных файлов файла.
        util::remove_file(j.ref_wav);
        for (const auto& [id, fp] : j.fmt_plans)
            if (fp.sidecar_size > 0) util::remove_file(fp.sidecar_path);
        if (j.best_valid) util::remove_file(j.best.path);

        if (!msg.empty()) status::log(msg);
        if (j.summary.status == "error") status::mark_error(j.idx);
        else if (j.summary.status == "skip") status::mark_skip(j.idx);
        else status::end_file(j.idx);
        if (j.summary.status == "error") {
            if (opts.ignore_errors) {
                // Игнорируем ошибку: файл помечается skip, прогон продолжается.
                j.summary.status = "skip";
                if (j.summary.detail.empty()) j.summary.detail = i18n::str("error ignored");
            } else {
                failed++;
                abort = true;
            }
        }
    }

    void worker() {
        for (;;) {
            Work w;
            {
                std::unique_lock<std::mutex> lk(qm);
                cv.wait_for(lk, std::chrono::milliseconds(50), [&] {
                    return proc::cancelled() || variant_pending_locked() ||
                           prep_allowed_locked() || all_done_locked();
                });
                if (proc::cancelled() || all_done_locked()) break;
                if (abort.load()) break;
                if (!take_work_locked(&w)) continue;
                if (w.kind == WorkKind::Variant)
                    status::task(w.idx, w.task, status::TaskState::Running);
            }

            if (w.kind == WorkKind::Prep) {
                status::prep(w.idx);
                std::string perr;
                try {
                    prep_file(jobs[w.idx]);
                } catch (const std::exception& exc) {
                    perr = exc.what();
                }
                if (proc::cancelled()) break;  // отмена — счётчики не трогаем, tmp почистит main
                if (!perr.empty()) {
                    FileJob& j = jobs[w.idx];
                    std::lock_guard<std::mutex> jl(*j.m);
                    j.summary.path = j.path;
                    j.summary.status = "error";
                    j.summary.detail = perr;
                            status::error("ERROR [" + j.path + "]: " + perr + "\n");
                }
                bool finish_now = false;
                size_t n_tasks = 0;
                {
                    std::lock_guard<std::mutex> lk(qm);
                    FileJob& j = jobs[w.idx];
                    j.prep_done = true;
                    if (prep_active > 0) prep_active--;
                    if (j.tasks.empty()) {
                        j.done = true;
                        total_done++;
                        finish_now = true;
                    } else {
                        n_tasks = j.tasks.size();
                    }
                    cv.notify_all();
                }
                if (!finish_now) status::set_tasks(w.idx, n_tasks);
                if (finish_now && !proc::cancelled()) finalize_file(jobs[w.idx]);
            } else {
                std::string verr;
                VariantOutcome oc = VariantOutcome::Failed;
                try {
                    oc = run_variant(jobs[w.idx], w.task);
                } catch (const std::exception& exc) {
                    verr = exc.what();
                }
                if (proc::cancelled()) break;  // отмена — счётчики не трогаем
                status::task(w.idx, w.task,
                             oc == VariantOutcome::Ok ? status::TaskState::Ok
                                                       : status::TaskState::Failed);
                bool last = false;
                {
                    std::lock_guard<std::mutex> lk(qm);
                    FileJob& j = jobs[w.idx];
                    if (!verr.empty()) {
                        std::lock_guard<std::mutex> jl(*j.m);
                        j.failures.push_back("variant: " + verr);
                        j.variant_errors++;
                                    status::error("ERROR [" + j.path + "]: " + verr + "\n");
                    }
                    j.completed++;
                    if (j.completed == j.tasks.size()) {
                        j.done = true;
                        total_done++;
                        last = true;
                    }
                    cv.notify_all();
                }
                if (last && !proc::cancelled()) finalize_file(jobs[w.idx]);
            }
        }
    }
};

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

    int jobs = resolve_jobs(opts.jobs, opts.jobs_float);

    // Статусбар входит в альтернативный буфер: всё, что печатается после init(),
    // в интерактивном режиме попадает на экран статусбара, поэтому init вызываем
    // до вывода диагностики, а сами диагностические строки — только в линейном режиме.
    status::init(files.size(), opts.no_status);

    std::string tmp = tmp_dir();
    util::mkdirs(tmp);

    // Журнал runs/*.jsonl — только в режиме отладки (stats.json накапливается всегда).
    report::Logger logger(opts.debug ? util::join_path(util::exe_dir(), "runs")
                                     : std::string());
    if (opts.debug) {
        if (logger.ok()) {
            if (!status::interactive()) out::print("Log: %s\n", logger.path().c_str());
        } else {
            out::error("WARNING: could not open the JSONL log (runs/)\n");
        }
    }

    if (!status::interactive())
        out::print("Files: %zu, threads: %d\n", files.size(), jobs);

    if (logger.ok()) {
        logger.event({{"type", "run_start"},
                      {"jobs", jobs},
                      {"files", files.size()},
                      {"dry_run", opts.dry_run},
                      {"verify", verify_name(opts.verify)},
                      {"ignore_errors", opts.ignore_errors},
                      {"report", opts.report_path}});
    }

    Runner r;
    r.opts = &opts;
    r.fmts = &fmts;
    r.logger = logger.ok() ? &logger : nullptr;
    r.ffprobe = media::find_ffprobe();
    r.ffmpeg = media::find_ffmpeg();
    r.tmp = tmp;
    r.window = jobs;
    r.n_files = files.size();
    r.jobs.resize(files.size());
    for (size_t i = 0; i < files.size(); i++) {
        r.jobs[i].idx = i;
        r.jobs[i].path = files[i];
        r.jobs[i].base = util::base_name(files[i]);
        r.jobs[i].base_ne = base_no_ext(files[i]);
        r.jobs[i].dir = util::dir_name(files[i]);
        r.jobs[i].tok = tmp_token(files[i]);
        r.jobs[i].m = std::make_unique<std::mutex>();
    }
    for (size_t i = 0; i < files.size(); i++) status::begin_file(i, r.jobs[i].base);

    std::vector<std::thread> threads;
    for (int i = 0; i < jobs; i++) threads.emplace_back(&Runner::worker, &r);
    for (auto& t : threads) t.join();

    status::shutdown();

    if (proc::cancelled()) {
        out::print("%s", i18n::str("Interrupted by user\n").c_str());
    }

    // Сводка по файлам, которые не удалось заменить. Печатается после shutdown(),
    // когда альтернативный буфер уже восстановлен — иначе текст пропадёт при
    // прерывании/изменении размера окна.
    {
        bool any = false;
        for (auto& j : r.jobs) {
            if (j.summary.replacement_error.empty()) continue;
            if (!any) out::print("%s", i18n::str("Replacement failed:\n").c_str());
            out::print("  %s — %s\n", j.path.c_str(), j.summary.replacement_error.c_str());
            any = true;
        }
    }

    // Убираем временные файлы (эталонные WAV и кандидаты).
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::u8path(tmp), ec)) {
        if (ec) break;
        if (e.is_regular_file()) fs::remove(e.path(), ec);
    }

    if (logger.ok()) {
        logger.event({{"type", "run_end"},
                      {"done", r.total_done},
                      {"failed", r.failed.load()}});
    }

    std::vector<report::FileSummary> summaries;
    for (auto& j : r.jobs) summaries.push_back(j.summary);

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
    if (r.failed.load() > 0 && !opts.ignore_errors) {
        out::error("Aborted: %d file(s) failed. Fix the issues above or re-run with "
                   "--ignore-errors to skip such files.\n",
                   r.failed.load());
    }
    return r.failed.load() > 0 ? 1 : 0;
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

    std::string base = util::base_name(path);
    std::string base_ne = base_no_ext(path);
    std::string dir = util::dir_name(path);
    std::string tmp = tmp_dir();
    util::mkdirs(tmp);
    std::string tok = tmp_token(path);
    std::string src_wav = util::join_path(tmp, tok + "_" + base_ne + ".src.wav");

    media::Probe probe = media::probe_file(path, ffprobe);
    bool src_decoded = false;
    if (!probe.ok && lower_ext(path) != util::to_lower(target.extension)) {
        // ffprobe не смог прочитать файл (напр. OptimFROG высоких пресетов ffmpeg
        // не разбирает). Пробуем родной декодер формата по расширению: расжимаем в
        // WAV и пробируем уже WAV — его ffprobe читает всегда.
        const config::Format* src_fmt = find_source_fmt(probe, path, fmts);
        if (src_fmt && src_fmt->id != target.id && decode_source_native(src_fmt, path, src_wav)) {
            probe = media::probe_file(src_wav, ffprobe);
            if (probe.ok) {
                probe.format_name = src_fmt->id;
                probe.size = util::file_size(path);
                src_decoded = true;
            }
        }
        if (!probe.ok) {
            util::remove_file(src_wav);
            print_locked("ERROR " + path + " — " + probe.error + "\n");
            return 1;
        }
    }
    if (lower_ext(path) == util::to_lower(target.extension)) {
        print_locked("SKIP " + path + " — " + i18n::str("already the format ") + target.id + "\n");
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
    const config::Format* src_fmt = find_source_fmt(probe, path, fmts);
    std::string dec_err;
    bool decoded = src_decoded;
    if (!decoded && src_fmt && src_fmt->id != target.id) decoded = decode_source_native(src_fmt, path, src_wav);
    if (!decoded) decoded = media::decode_to_wav(path, src_wav, ffmpeg, bits, &dec_err);
    if (!decoded) {
        util::remove_file(src_wav);
        print_locked("ERROR " + path + " — " + i18n::str("decode to reference WAV: ") + dec_err + "\n");
        return 1;
    }

    tool::Status st = tool::ensure(target, !no_download, "[" + target.id + "] ");
    if (st.path.empty()) {
        util::remove_file(src_wav);
        print_locked(i18n::fmt("ERROR %s — utility %s unavailable (%s)\n", path.c_str(),
                                target.id.c_str(), st.status.c_str()));
        return 1;
    }

    Env env;
    env.fmt = &target;
    env.encoder = st.path;
    env.decoder = decoder_path(target, st.path);

    std::string cand_name = tok + "_" + base_ne + ".restore." + target.extension;
    std::string candidate = util::join_path(tmp, cand_name);
    util::remove_file(candidate);

    // Restore всегда выполняет полную проверку кандидата (единственный вариант).
    std::string verr = encode_candidate(src_wav, candidate, variant.args, env);
    if (verr.empty()) verr = validate_candidate(src_wav, candidate, env);
    util::remove_file(src_wav);
    if (!verr.empty()) {
        util::remove_file(candidate);
        print_locked("ERROR " + path + " — " + verr + "\n");
        return 1;
    }
    uint64_t size = util::file_size(candidate);
    if (size == 0) {
        util::remove_file(candidate);
        print_locked("ERROR " + path + " — " + i18n::str("empty file") + "\n");
        return 1;
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
        print_locked("ERROR " + path + " — " + i18n::str("tags: ") + terr + "\n");
        return 1;
    }
    if (has_tags && ts.present) {
        std::string v2 = tags::validate_groups(candidate, target.id, plan.embed, ffprobe);
        if (!v2.empty()) {
            util::remove_file(candidate);
            if (sidecar) util::remove_file(candidate + ".tags.zip");
            print_locked("ERROR " + path + " — " + i18n::str("tag validation: ") + v2 + "\n");
            return 1;
        }
    }

    // Новый файл в целевом формате создаётся рядом с исходником; источник удаляется
    // только после полной записи нового файла. Потеря данных исключена: даже если
    // удаление не пройдёт, останутся оба файла (replace_file на месте здесь не подходит —
    // restore меняет имя файла, а не содержимое).
    std::string new_path = util::join_path(dir, base_ne + "." + target.extension);
    if (!util::copy_file(candidate, new_path)) {
        print_locked(i18n::fmt("ERROR %s — could not copy the candidate into the folder\n", path.c_str()));
        return 1;
    }
    if (!util::remove_file(path)) {
        print_locked(i18n::fmt("WARNING %s — the new file is saved as %s, but the old file "
                               "could not be removed and is left in place\n",
                               path.c_str(), new_path.c_str()));
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

    int jobs = resolve_jobs(opts.jobs, opts.jobs_float);
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
