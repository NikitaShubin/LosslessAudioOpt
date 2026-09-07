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
#include <optional>
#include <set>
#include <thread>
#include <unordered_set>

#include "config.h"
#include "i18n.h"
#include "media.h"
#include "obs.h"
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

// Приводит путь к единому виду для сравнения при дедупликации. Демон получает
// пути и через HTTP (разделители «/»), и из файловой системы wine («\» в смеси
// со «/» от пользовательских корней), поэтому сравниваются только
// нормализованные значения — иначе дубли вложенных папок не распознаются.
std::string norm_path(const std::string& p) {
    std::string s = p;
    for (auto& c : s)
        if (c == '\\') c = '/';
    return s;
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

// Известная расширения входных файлов. Строится из formats/*.json (без
// хардкода списка) плюс базовые lossless-исходники (wav/aiff/ogg), которые
// разрешены как вход всегда, даже если для них нет отдельного config.
std::set<std::string> supported_extensions(const std::vector<config::Format>& fmts) {
    std::set<std::string> s;
    for (const auto& f : fmts)
        if (!f.extension.empty()) s.insert(util::to_lower(f.extension));
    // Входные форматы (lossy и прочие, не являющиеся целевыми кодеками) — из
    // formats/inputs.json, без хардкода. Добавление нового входа не требует
    // перекомпиляции.
    for (const auto& e : config::input_extensions()) s.insert(e);
    s.insert("wav");
    s.insert("aiff");
    s.insert("ogg");
    return s;
}

bool is_supported_file(const std::string& path, const std::set<std::string>& exts) {
    std::string base = util::to_lower(util::base_name(path));
    size_t dot = base.find_last_of('.');
    if (dot == std::string::npos) return false;
    return exts.count(base.substr(dot + 1)) != 0;
}

// Собранный файл: полный путь + путь относительно заданного в параметрах корня
// (для файла-аргумента — просто имя). rel используется в статусбаре.
struct FileItem {
    std::string path;
    std::string rel;
};

void collect_files(const std::string& p, std::vector<FileItem>& out, std::string* err,
                   const std::vector<config::Format>& fmts) {
    std::set<std::string> exts = supported_extensions(fmts);
    if (util::file_exists(p)) {
        if (is_supported_file(p, exts)) out.push_back({p, util::base_name(p)});
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
            if (is_supported_file(f, exts)) {
                std::string rel = fs::relative(fs::u8path(f), fs::u8path(p), ec).u8string();
                if (ec || rel.empty()) rel = util::base_name(f);
                out.push_back({f, rel});
            }
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

// Каталог временных файлов текущего процесса: base/<pid>. Отдельная
// подпапка на процесс исключает конфликты имён между параллельными прогонами
// llao (tok — хэш пути — у них одинаковый) и упрощает очистку: после прогона
// подпапка удаляется целиком, а чужие подпапки не трогаются.
// Используется только монолитным CLI (optimize/restore): демон работает
// single-instance и единолично владеет базовым tmp, поэтому сессия пишет
// напрямую в базу без pid-уровня (см. Engine::init в serve-режиме).
static std::string base_tmp_dir(const std::string& custom) {
    return custom.empty() ? tmp_dir() : custom;
}

std::string session_tmp_dir(const std::string& custom) {
    std::string d = util::join_path(base_tmp_dir(custom), util::process_id());
    util::mkdirs(d);
    return d;
}

// Удаляет подпапку временных файлов текущего процесса целиком (рекурсивно).
// Вызывается при старте (остатки после обрыва) и после завершения прогона.
void clear_session_tmp_dir_impl(const std::string& custom) {
    std::error_code ec;
    fs::remove_all(fs::u8path(session_tmp_dir(custom)), ec);
}

// Удаляет базовый tmp-каталог целиком (подпапки всех сессий/PID) и создаёт его
// заново. Используется демоном: он единственный процесс, работающий со своим
// базовым tmp («--tmp» или exe_dir/tmp), поэтому при старте убирает всё —
// включая остатки аварийно завершённых сессий и следы сирот прошлых запусков.
void clear_tmp_base(const std::string& custom) {
    std::string d = base_tmp_dir(custom);
    std::error_code ec;
    fs::remove_all(fs::u8path(d), ec);
    util::mkdirs(d);
}

// ---------------------------------------------------------------------------
// FileSession: RAII-обёртка для tmp-файлов одного исходного файла.
// Создаёт tmp-поддиректорию по токену пути; деструктор ГАРАНТИРУЕТ очистку
// через remove_all — неважно, кто и когда создал файлы внутри.
// ---------------------------------------------------------------------------

class FileSession {
public:
    FileSession(const std::string& original_path, const std::string& tmp_base)
        : path_(original_path) {
        std::string tok = tmp_token(path_);
        dir_ = util::join_path(tmp_base, tok);
        util::mkdirs(dir_);
    }

    ~FileSession() { cleanup(); }

    FileSession(const FileSession&) = delete;
    FileSession& operator=(const FileSession&) = delete;

    FileSession(FileSession&& o) noexcept
        : dir_(std::move(o.dir_)), path_(std::move(o.path_)) {
        o.dir_.clear();
    }

    FileSession& operator=(FileSession&& o) noexcept {
        if (this != &o) {
            cleanup();
            dir_ = std::move(o.dir_);
            path_ = std::move(o.path_);
            o.dir_.clear();
        }
        return *this;
    }

    const std::string& dir() const { return dir_; }
    const std::string& original_path() const { return path_; }

    // Путь к референсному WAV (decode исходника).
    std::string ref_wav_path() const {
        return util::join_path(dir_, "ref.wav");
    }

    // Путь к кандидату: <dir>/<fmt>.<variant>.<ext>
    std::string candidate_path(const std::string& fmt_id,
                               const std::string& variant_id,
                               const std::string& ext) const {
        return util::join_path(dir_, fmt_id + "." + variant_id + "." + ext);
    }

    // Путь к sidecar: <dir>/<fmt>.tags.zip
    std::string sidecar_path(const std::string& fmt_id) const {
        return util::join_path(dir_, fmt_id + ".tags.zip");
    }

    // Явное удаление tmp-директории (до деструктора, для раннего освобождения
    // места). После вызова dir_ пуста — деструктор ничего не удалит. Если папку
    // не удалось убрать (занятость/ошибка ФС), папка остаётся в tmp до следующего
    // clear_tmp_base — это важно видеть в логе для диагностики «кучи .dec.wav».
    void cleanup() {
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

    bool ok() const { return !dir_.empty(); }

private:
    std::string dir_;   // tmp/<tok>/  (у демона — прямо в базе tmp, без pid)
    std::string path_;  // оригинальный путь к файлу
};

struct DiskBudget {
    std::mutex m;
    uint64_t reserved = 0;
    uint64_t min_free = 1ull << 30;  // 1 ГБ страховой запас

    bool try_reserve(const std::string& tmp_path, uint64_t bytes) {
        std::lock_guard<std::mutex> lk(m);
        uint64_t free = util::disk_free_bytes(tmp_path);
        uint64_t avail = free > (reserved + min_free) ? free - reserved - min_free : 0;
        if (bytes > avail) return false;
        reserved += bytes;
        return true;
    }

    void release(uint64_t bytes) {
        std::lock_guard<std::mutex> lk(m);
        reserved = (bytes >= reserved) ? 0 : reserved - bytes;
    }
};

// Оценка размера WAV-файла по данным ffprobe (до декодирования).
static uint64_t estimated_wav_bytes(const media::Probe& probe, int bits) {
    int bps = bits > 0 ? bits : 16;
    uint64_t ch = probe.channels > 0 ? (uint64_t)probe.channels : 2;
    uint64_t sr = probe.sample_rate > 0 ? (uint64_t)probe.sample_rate : 44100;
    double dur = probe.duration > 0.0 ? probe.duration : 60.0;
    return 44 + ch * (bps / 8) * sr * (uint64_t)(dur + 1.0);
}

// Пиковый след файла на диске (уровень 1 — файловый бюджет).
static uint64_t file_peak_bytes(uint64_t wav, Verify v) {
    return wav * ((v == Verify::None) ? 3 : 4);
}

// Инкрементальный след одного варианта (уровень 2 — задачевый бюджет).
static uint64_t variant_peak_bytes(uint64_t wav, Verify v) {
    return wav * ((v == Verify::None) ? 1 : 2);
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
    cands.push_back(util::find_in_path(name + ".exe"));
    for (const auto& c : cands) {
        if (c.empty() || !util::file_exists(c)) continue;
#ifdef _WIN32
        if (!util::is_pe(c)) continue;
#endif
        return c;
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
    int bits = 16;
};

// Имя PCM-кодека ffmpeg для заданной битности (16/24/32).
static const char* pcm_codec(int bits) {
    if (bits > 24) return "pcm_s32le";
    if (bits > 16) return "pcm_s24le";
    return "pcm_s16le";
}

// ---------------------------------------------------------------------------
// Алиас исходного файла для ANSI-декодеров (la.exe, ofr.exe и т.п.)
// ---------------------------------------------------------------------------

enum class DecodeStatus { Ok, NeedsCopy, Failed };

// Декод исходника собственным декодером формата (без скачивания: только кэш/PATH).
// Перед вызовом декодера создаёт алиас (symlink → hardlink → оригинал) в каталоге
// out_wav, чтобы ANSI-API декодеров не ломались на не-ASCII путях.
// Алиас удаляется сразу после завершения декодера.
// Возвращает:
//   Ok        — out_wav создан
//   Failed    — декод упал (алиас создавался; проблема не в имени)
//   NeedsCopy — декод упал + алиас не создался (нужна полная копия файла)
static DecodeStatus decode_source_native(const config::Format* src_fmt, const std::string& path,
                                         const std::string& out_wav, int bits,
                                         const std::atomic<bool>* kill = nullptr) {
    if (!src_fmt) return DecodeStatus::Failed;
    tool::Status sst = tool::ensure(*src_fmt, false, "[" + src_fmt->id + "] ", kill);
    if (kill && kill->load(std::memory_order_relaxed)) return DecodeStatus::Failed;
    if (sst.path.empty()) return DecodeStatus::Failed;

    // Нормализуем разделители в пути исходника: на Windows-сборке под wine входной
    // путь может быть смешанным («/tmp/x\file.ofr» — слеши + бэкслеш). Строгие
    // нативные кодеки (OptimFROG и др.) такой путь не находят (FILENOTFOUND),
    // хотя ffmpeg/ffprobe его переносят. Приводим к единому «/» — безопасно и на
    // Windows (API принимает слеши), и на Linux.
    std::string src = path;
    for (auto& c : src) if (c == '\\') c = '/';

    std::string input_path = src;
    bool alias_created = false;
    std::string alias_path;
#ifndef _WIN32
    // Алиас (симлинк/хардлинк) полезен на Linux: короткое имя с правильным
    // расширением обходит проблемы декодеров со спецсимволами в путях.
    // На Windows НЕ используем: CreateSymbolicLinkW с Unix-целью («/tmp/...»)
    // не резолвится под wine — декодер получает битый reparse-point и падает
    // с FILENOTFOUND. Отдаём нормализованный исходный путь напрямую (он корректен
    // в любой форме — confirmed ручными тестами).
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

    // Удаляем алиас сразу — он больше не нужен (WAV уже создан или декод провалился).
    if (alias_created) util::remove_file(alias_path);

    if (ok) return DecodeStatus::Ok;
    return alias_created ? DecodeStatus::Failed : DecodeStatus::NeedsCopy;
}

// Кодирование кандидата (без валидации). Возвращает пустую строку при успехе,
// иначе текст ошибки. При успехе candidate существует и непуст.
// monitor — опциональный stall detection (файл не растёт + CPU ≈ 0 → kill).
std::string encode_candidate(const std::string& wav, const std::string& candidate,
                             const std::vector<std::string>& params, const Env& env,
                             const proc::OutputMonitor& monitor = {},
                             const std::atomic<bool>* kill = nullptr) {
    const config::Format& f = *env.fmt;
    std::vector<std::string> encode_args =
        build_cmd(f.encode_cmd, env.encoder, wav, candidate, params, f.engine_codec,
                  f.engine_container);
    // Если monitor задан с hard_timeout_sec — он заменяет encode_timeout
    // (пропорциональный лимит вместо фиксированного).
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

// Удаление файла валидации (dec.wav) с длинным ретраем. Wine (wineserver) и
// антивирус удерживают свежезаписанный файл дольше, чем покрывает базовая
// remove_file (~3 сек): handle декодера закрывается с задержкой, и одиночная
// попытка оставляет «кучу dec.wav» на диске до конца job. Повторяем нарастающими
// паузами до ~20 сек.
bool remove_dec_wav(const std::string& p) {
    if (util::remove_file(p)) return true;
    for (int attempt = 0; attempt < 4; attempt++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2500 * (attempt + 1)));
        if (!util::file_exists(p)) return true;
        if (util::remove_file(p)) return true;
    }
    return false;
}

// Полная валидация кандидата: builtin-проверка формата (flac -t и т.п.) + декод
// и побитовое сравнение PCM с эталонным WAV (потоковое, без загрузки в память).
// Возвращает пустую строку при успехе, иначе текст ошибки.
std::string validate_candidate(const std::string& wav, const std::string& candidate,
                               const Env& env, const std::atomic<bool>* kill = nullptr) {
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
        // ffmpeg при записи WAV выбирает pcm_s16le по умолчанию. Для источников
        // глубже 16 бит явно задаём глубину PCM, иначе data-чанк не совпадёт
        // с эталонным WAV (для 24-бит alac/tta это даёт "PCM не совпадает").
        dec_args = {env.decoder, "-y", "-loglevel", "error", "-i", candidate,
                    "-c:a", pcm_codec(env.bits), dec_wav};
    } else {
        dec_args = build_cmd(f.decode_cmd, env.decoder, candidate, dec_wav, {}, f.engine_codec,
                             f.engine_container);
    }
    proc::Result dr = proc::run(dec_args, env.decode_timeout, "", {}, kill);
    if (!dr.started || dr.timed_out || dr.exit_code != 0) {
        std::string out = util::trim(dr.output);
        // Диагностика «кучи .dec.wav»: фиксируем момент, когда декодер не
        // довёл файл до конца, — именно такие софт-прерывания (таймаут, kill,
        // крах wine-декодера) оставляют частично записанный dec.wav на диске.
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
        // Даже длинный ретрай не помог (процесс-декодер по-прежнему держит
        // handle). Файл останется до конца обработки — папку сессии целиком
        // уберёт FileSession::cleanup при финализации (или clear_tmp_base при
        // старте; в рамках живой сессии это последний рубеж).
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

// Расширение формата по id.
static std::string fmt_ext(const std::string& id, const std::vector<config::Format>& fmts) {
    for (const auto& f : fmts)
        if (f.id == id) return f.extension;
    return id;
}

// Доставка sidecar рядом с доставленным файлом <dir>/<base_ne>.<ext>.
// Единая транзакция, согласованная с персистентностью очереди (см. util.cpp
// replace_file): кандидат копируется в .llao-tmp.tags.zip рядом, старый
// sidecar удаляется, новый переименовывается. Если sidecar не нужен
// (need=false), а старый <base_ne>.tags.zip лежит на диске — удаляем его:
// теги уже встроены в файл, лишний архив оставался бы устаревшим.
// Возвращает true если весь процесс прошёл штатно; *note пополняется
// предупреждением при сбое.
static bool deliver_sidecar(const std::string& sc_src, bool need,
                            const std::string& dir, const std::string& base_ne,
                            std::string* note) {
    const std::string dst = util::join_path(dir, base_ne + ".tags.zip");
    const std::string tmp = util::join_path(dir, "." + base_ne + ".llao-tmp.tags.zip");
    util::remove_file(tmp);  // зачистка артефакта прерванного запуска
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
    std::string rel;   // путь относительно корня параметров (для статусбара)
    std::string base_ne;
    std::string dir;
    std::string tok;

    // Режим задачи (демон): обычная оптимизация или восстановление в один формат.
    JobMode mode = JobMode::Optimize;
    std::string target_dir;  // пусто = замена на месте; иначе — корень вывода
    std::string restore_to;  // для Restore: id целевого формата (пусто = по умолчанию)

    // --- RAII-сессия tmp-файлов (создаётся в prep_file) ---
    std::unique_ptr<FileSession> session;

    // --- планировщик (под g_qm) ---
    std::vector<TaskDesc> tasks;
    size_t released = 0;
    size_t completed = 0;
    bool prep_done = false;
    bool prep_running = false;  // prep активно выполняется в worker (под g_qm)
    bool done = false;
    bool cancelled = false;  // файл снят из очереди (remove/cancel-file): не запускать
    // Мгновенная остановка связанных процессов (remove/shutdown): проверяется
    // в цикле опроса proc::run (200 мс). Адрес стабилен (jobs — unique_ptr).
    std::atomic<bool> kill_requested{false};
    // Файл вошёл в фазу финализации (finalize_file выполняется на воркере).
    // Ставится до начала любых долгих операций (валидация победителя/доставка).
    // Позволяет remove_file доставить kill_requested строке, у которой done уже
    // истинно, но финализация ещё идёт (иначе она была бы «неостановимой»).
    std::atomic<bool> finalizing{false};

    // --- подготовка (один поток prep, до выпуска задач) ---
    bool prep_ok = false;
    uint64_t ref_size = 0;   // размер эталонного WAV (оценка размера dec.wav задачи)
    uint64_t wav_est = 0;    // оценка размера WAV по probe (для бюджета)
    uint64_t peak_file = 0;  // файловый бюджет (file_peak_bytes)
    bool deferred = false;   // try_reserve не прошёл — повторить позже
    // Повтор отложенного prep не раньше этого момента (анти-спин: воркер не
    // должен стучаться в бюджет по кругу, когда tmp-диск переполнен).
    std::chrono::steady_clock::time_point defer_until{};
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
    std::vector<std::string> excluded_fmts;  // форматы, исключённые по caps (жёлтые точки)
    int variant_errors = 0;   // операционные сбои вариантов (кодирование/валидация/теги)
    int tool_errors = 0;      // утилиты форматов недоступны
    bool error_counted = false;  // под m: ошибка файла уже учтена в failed (без двойного счёта)
    report::FileSummary summary;
    // Задача в режиме Restore, вход уже в целевом формате: prep пометил файл ok
    // и не строил задач — finalize не должен выбирать победителя/менять файл.
    bool early_ok = false;
    std::string out_path;  // фактический путь результата (после записи в цель/на место)
    // Ошибка файла уже доложена через obs::sink()->error_file (единая модель):
    // finalize не должен эмитить второе событие для того же файла.
    bool error_reported = false;
};

// ---------------------------------------------------------------------------
// ResourceManager: брокер ресурсов (дисковое пространство, планирование).
// Принимает заявки от FileSession, удовлетворяет/ставит на паузу.
// Правила выделения могут меняться без правки FileSession.
// ---------------------------------------------------------------------------

struct ResourceRequest {
    uint64_t disk_bytes = 0;
    enum class Status { Granted, Deferred, Denied };
    Status status = Status::Deferred;
};

class ResourceManager {
public:
    ResourceManager() = default;

    ResourceManager(const std::string& tmp_path, int max_workers, uint64_t min_free = 1ull << 30)
        : tmp_path_(tmp_path), max_workers_(max_workers) {
        disk_.min_free = min_free;
    }

    // --- Дисковые ресурсы ---
    ResourceRequest request_disk(uint64_t bytes) {
        ResourceRequest req;
        req.disk_bytes = bytes;
        req.status = disk_.try_reserve(tmp_path_, bytes)
                         ? ResourceRequest::Status::Granted
                         : ResourceRequest::Status::Deferred;
        return req;
    }

    void release_disk(uint64_t bytes) { disk_.release(bytes); }

    // --- Планирование (определения — ниже) ---
    bool can_start_new_file(size_t next_prep, int prep_active,
                            const std::vector<std::unique_ptr<FileJob>>& jobs, bool aborted) const;
    bool can_start_new_variant(const FileJob& j, int window, bool aborted) const;

    void on_prep_started() { active_preps_++; }
    void on_prep_completed() { if (active_preps_ > 0) active_preps_--; }

    const std::string& tmp_path() const { return tmp_path_; }
    int max_workers() const { return max_workers_; }

    void set_max_workers(int w) { max_workers_ = w; }
    void set_tmp_path(const std::string& p) { tmp_path_ = p; }

private:
    DiskBudget disk_;
    std::string tmp_path_;
    int max_workers_ = 1;
    int active_preps_ = 0;
};

// --- Определения методов ResourceManager ---

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
    // Есть ли хотя бы один файл, ещё не прошедший prep? (динамическая очередь:
    // jobs может расти, поэтому не `next_prep < jobs.size()`, а живой поиск.)
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

struct Runner {
    const Options* opts = nullptr;
    const std::vector<config::Format>* fmts = nullptr;
    report::Logger* logger = nullptr;
    std::string ffprobe;
    std::string ffmpeg;
    std::string tmp;
    int window = 1;

    ResourceManager rm;  // брокер ресурсов: диск + планирование
    // --- кэш ресурсов для адаптивного окна (обновляется не чаще 10 с) ---
    std::chrono::steady_clock::time_point res_last{};
    uint64_t ram_budget = 0;           // бюджет RAM: 50% доступной памяти

    std::mutex qm;
    std::condition_variable cv;
    size_t next_prep = 0;
    int prep_active = 0;  // число выполняющихся prep (могут идти параллельно)
    size_t total_done = 0;
    std::vector<std::unique_ptr<FileJob>> jobs;
    // Дедуп «по сессии»: полные пути файлов, принятых в очередь и не удалённых
    // из неё. Снимаются при remove/cancel/clear-done — после удаления файл
    // снова можно добавить. Так добавление родительской папки после уже
    // обработанной вложенной не дублирует файлы ни в каком состоянии.
    std::unordered_set<std::string> seen_paths_;
    std::atomic<int> failed{0};
    std::atomic<bool> abort{false};  // при ошибке файла без --ignore-errors: прекращаем прогон
    std::atomic<bool> shutdown_requested{false};  // демон: остановить воркеры (graceful shutdown)
    std::atomic<bool> queue_paused{false};        // демон: не запускать новые задачи (pause/resume)
    std::atomic<int> workers_alive{0};            // число живых воркеров (диагностика)

    // Создаёт FileJob из FileItem. idx — позиция в векторе jobs (уже назначена).
    void make_job(FileJob& j, size_t idx, const FileItem& it) {
        j.idx = idx;
        j.path = it.path;
        j.base = util::base_name(it.path);
        j.rel = it.rel;
        j.base_ne = base_no_ext(it.path);
        j.dir = util::dir_name(it.path);
        j.tok = tmp_token(it.path);
        j.m = std::make_unique<std::mutex>();
    }

    // Добавляет файлы в очередь на лету (демон). Режим и целевая папка пачки
    // фиксируются при добавлении и хранятся в FileJob (см. AddOptions).
    std::vector<size_t> append_files(const std::vector<FileItem>& items,
                                     const AddOptions& ao = {}) {
        std::vector<size_t> idx;
        std::vector<std::string> labels;
        {
            std::lock_guard<std::mutex> lk(qm);
            // Дедуп по полному пути (в рамках сессии): файл уже принимался в
            // очередь в любом состоянии (queued/prep/running/ok/stopped) и не
            // был удалён из неё. Добавление родительской папки после уже
            // обработанной вложенной НЕ дублирует файлы; повторно добавить
            // можно после remove/clear-done — путь снимается из seen_paths_.
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
            // begin_file/job_meta/added эмитятся ПОД qm, до cv.notify_all():
            // воркер, проснувшийся по уведомлению, может сразу выпустить
            // task(Running)/prep — но ещё не может приобрести qm, поэтому
            // строки всегда создаются в зеркале РАНЬШЕ любых событий по файлу.
            // Иначе на llao+WSL наблюдалась гонка: task(Running) до begin_file.
            for (size_t k = 0; k < idx.size(); k++) {
                const FileJob& j = *jobs[idx[k]];
                obs::sink()->begin_file(idx[k], labels[k]);
                // Режим и целевая папка пачки — метаданные строки (бейдж в вебе).
                obs::sink()->job_meta(idx[k],
                                      j.mode == JobMode::Restore ? "restore" : "optimize",
                                      j.target_dir);
            }
            if (idx.size() > 1) obs::sink()->files_added(idx, labels);
            cv.notify_all();
        }
        return idx;
    }

    // Пауза/продолжение всей очереди (демон). Приостанавливает запуск новых
    // задач; активные процессы дорабатывают. Вызывается вне qm.
    void pause_queue() {
        queue_paused.store(true);
        cv.notify_all();
    }
    void resume_queue() {
        queue_paused.store(false);
        cv.notify_all();
    }
    bool is_paused() const { return queue_paused.load(); }

    // Позиция задания по стабильному idx (FileJob::idx). Внешние команды
    // (remove/cancel) оперируют idx, а не позициями: после reorder они
    // различаются. Вызывать под qm. Возвращает SIZE_MAX, если нет.
    size_t find_pos_locked(size_t idx) const {
        for (size_t p = 0; p < jobs.size(); p++)
            if (jobs[p]->idx == idx) return p;
        return SIZE_MAX;
    }

    // Снимает файл из очереди (remove/cancel-file). idx — стабильный id.
    // Для файла, ещё не запущенного (pending: не в prep, задач не выпущено)
    // — немедленно завершает его (done + skip, tmp чистится). Для уже
    // запущенного — помечает cancelled: новые задачи не запускаются,
    // активные процессы убиваются через kill_requested. Вызывается вне qm.
    void remove_file(size_t idx) {
        std::lock_guard<std::mutex> lk(qm);
        size_t pos = find_pos_locked(idx);
        if (pos == SIZE_MAX) return;
        FileJob& j = *jobs[pos];
        // Удаление из очереди (любого состояния) снимает «запрет на повторное
        // добавление»: после remove/cancel/clear-done файл можно добавить снова.
        seen_paths_.erase(norm_path(j.path));
        if (j.done) {
            // Файл уже помечен завершённым, но если он ещё в финализации
            // (валидация победителя/доставка с долгими процессами) — доводим
            // kill_requested, чтобы finalize штатно развернулся в stopped и
            // почистил tmp. Уже полностью завершённые строки не трогаем.
            if (j.finalizing.load(std::memory_order_relaxed))
                j.kill_requested.store(true, std::memory_order_relaxed);
            return;
        }
        if (!j.prep_done && !j.prep_running && j.released == 0) {
            // pending: снимаем сразу.
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
            // запущен: запрещаем новые задачи, активные процессы убиваем
            // мгновенно через kill_requested (проверяется в proc::run).
            j.kill_requested.store(true, std::memory_order_relaxed);
            std::lock_guard<std::mutex> jl(*j.m);
            j.cancelled = true;
        }
    }

    // Единая точка очистки временных файлов job'а: освобождает дисковый бюджет
    // и удаляет подпапку сессии (RAII FileSession). Вызывается из всех веток,
    // где работа над файлом прекращена — штатно, при снятии из очереди или по
    // исключению — чтобы подпапка tmp не оставалась в рамках живой сессии.
    void discard_job_tmp(FileJob& j) {
        if (j.peak_file > 0) {
            rm.release_disk(j.peak_file);
            j.peak_file = 0;
        }
        if (j.session) j.session.reset();
    }

    // Переупорядочивает очередь (reorder): ids — стабильные idx файлов
    // (FileJob::idx) в новом порядке. Допускается подмножество (напр. только
    // видимые клиенту строки): перечисленные встают первыми в заданном
    // порядке, остальные сохраняют относительный порядок в хвосте.
    // Для уже запущенных порядок не влияет на текущую задачу, но меняет
    // приоритет последующих. Вызывается вне qm.
    bool reorder(const std::vector<size_t>& ids) {
        std::lock_guard<std::mutex> lk(qm);
        if (ids.empty() || ids.size() > jobs.size()) return false;
        // Позиция каждого idx в текущем векторе (idx монотонны, вектор
        // только растёт — idx всегда < jobs.size()).
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
        // idx — стабильный id файла, не переназначаем; порядок — порядок вектора
        jobs = std::move(reordered);
        next_prep = 0;  // курсор переинициализируется (find_next_prep всё равно сканирует)
        cv.notify_all();
        return true;
    }

    bool all_done_locked() const { return total_done == jobs.size(); }

    // Учитывает ошибку файла в счётчике failed (не чаще одного раза на файл) и
    // останавливает прогон. Вызывается при статусе "error" в finalize_file и при
    // операционной ошибке варианта в worker (когда финализации может не случиться).
    void count_error(FileJob& j) {
        {
            std::lock_guard<std::mutex> jl(*j.m);
            count_error_locked(j);
        }
        // В режиме демона ошибка файла не прерывает очередь (семантика
        // вечного --ignore-errors): только учитываем в счётчике failed.
        if (opts->ignore_errors || opts->mode == SessionMode::Daemon) return;
        abort.store(true);
        proc::abort_all();  // не ждём завершения активных процессов — прерываем их
        cv.notify_all();
    }

    // j.m уже удерживается вызывающим (finalize_file).
    void count_error_locked(FileJob& j) {
        if (!j.error_counted) {
            j.error_counted = true;
            failed++;
        }
    }

    // Есть ли хоть один готовый файл с невыпущенными задачами и свободным
    // личным окном (для cv-предиката). Бюджет диска проверяется в take_work.
    bool variant_launchable_locked() {
        if (abort.load()) return false;
        if (queue_paused.load() && opts->mode == SessionMode::Daemon) return false;
        for (size_t i = 0; i < jobs.size(); i++) {
            if (rm.can_start_new_variant(*jobs[i], window, false))
                return true;
        }
        return false;
    }

    // Наступает ли срок повторения отложенного prep (дефер-файл, чьё время
    // defer_until уже прошло) — для cv-предиката и анти-спина.
    bool deferred_retry_locked() {
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

    // Находит следующий файл, требующий prep (не done, не prep_done), начиная
    // с курсора next_prep по кругу. Динамическая очередь: файлы могут быть
    // добавлены (append_files) — они всегда c индексом >= текущего, курсор
    // их естественно достигнет; повторный круг ловит промежуточные (deferred).
    bool find_next_prep_locked(size_t* out) {
        if (jobs.empty()) return false;
        auto now = std::chrono::steady_clock::now();
        for (size_t k = 0; k < jobs.size(); k++) {
            size_t i = (next_prep + k) % jobs.size();
            FileJob& j = *jobs[i];
            if (j.done || j.prep_done || j.prep_running) continue;
            // Отложенный файл, чей срок повторения ещё не наступил, не берём:
            // без бюджета его нельзя декодировать, а находиться в активном цикле
            // он не должен (тот же анти-спин, что и в take_work_locked).
            if (j.deferred && now < j.defer_until) continue;
            *out = i;
            return true;
        }
        return false;
    }

    // Prep разрешён, если есть хотя бы 1 распакованный файл без задач
    // («запасной») — либо все распакованные уже выпустили задачи.
    bool prep_allowed_locked() const {
        if (queue_paused.load() && opts->mode == SessionMode::Daemon) return false;
        return rm.can_start_new_file(next_prep, prep_active, jobs, abort.load());
    }

    enum class WorkKind { None, Prep, Variant };
    struct Work {
        WorkKind kind = WorkKind::None;
        size_t idx = 0;        // стабильный id файла (FileJob::idx)
        FileJob* job = nullptr; // указатель на FileJob (стабильный heap-адрес)
        size_t task = 0;
        // Резерв дискового бюджета, выданный этой задаче (variant_peak).
        // Освобождается ровно один раз при её завершении — учёт точный
        // при любом чередовании воркеров (раньше release пропускался для
        // последней задачи, а reserve зависел от jf в момент взятия —
        // бюджет утекал и очередь вставала навсегда).
        uint64_t disk_reserved = 0;
    };

    // Вызывается под qm. Свободный воркер берёт очередной незанятый вариант.
    // Приоритет: варианты (уже распакованных файлов) > распаковка нового файла.
    // Задачевый бюджет (variant_peak) запрашивается для 2-го и последующих
    // воркеров файла; первый воркер работает в рамках файлового бюджета.
    bool take_work_locked(Work* w) {
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
                // Анти-спин: с момента отложения прошло меньше времени, чем
                // установил prep_file (500 мс) — не стучимся в бюджет по кругу.
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

    // --- подготовка файла: проба, теги, эталонный WAV, список задач ---
    void prep_file(FileJob& j) {
        const Options& opts = *this->opts;
        const auto& fmts = *this->fmts;

        // Если это повторный вызов (deferred retry), budgets уже зарезервирован
        // в take_work_locked. При любом раннем выходе — освобождаем.
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

        // RAII-сессия: tmp-поддиректория для всех временных файлов этого исходника.
        j.session = std::make_unique<FileSession>(j.path, tmp);
        std::string ref_wav = j.session->ref_wav_path();
        bool src_decoded = false;

        media::Probe probe = media::probe_file(j.path, ffprobe, &j.kill_requested);
        if (proc::aborted() || j.kill_requested.load(std::memory_order_relaxed)) {
            release_deferred_budget();
            return;
        }
        if (!probe.ok) {
            const config::Format* src_fmt = find_source_fmt(probe, j.path, fmts);
            if (src_fmt && src_fmt->id != probe.format_name) {
                DecodeStatus ds = decode_source_native(src_fmt, j.path, ref_wav, 16);
                if (ds == DecodeStatus::NeedsCopy) {
                    std::string copy = util::join_path(j.session->dir(), "src_copy." + lower_ext(j.path));
                    if (util::copy_file(j.path, copy)) {
                        ds = decode_source_native(src_fmt, copy, ref_wav, 16);
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

        // Теги: встроенные + сайдкар .tags.zip (для повторной оптимизации
        // уже оптимизированного файла, напр. la+tags.zip). Ищем сайдкар для
        // любого входного файла, не только для папок.
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

        // Восстановление: вход уже в целевом формате — пережимать нечего, файл
        // завершается сразу (ok без замены). Проверка по расширению файла, как
        // в монолитном restore (эталон клиентского поведения).
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
                return;  // итоговое сообщение/событие/stats сформирует finalize_file
            }
        }

        // Файловый бюджет: оценка WAV + пиковый след. Если не влезает —
        // помечаем deferred и выходим (без error). При повторном вызове (deferred retry)
        // бюджет уже зарезервирован в take_work_locked — пропускаем.
        j.wav_est = estimated_wav_bytes(probe, bits);
        if (j.peak_file == 0) {
            j.peak_file = file_peak_bytes(j.wav_est, opts.verify);
            if (rm.request_disk(j.peak_file).status != ResourceRequest::Status::Granted) {
                j.deferred = true;
                // Бюджет не зарезервирован — сбрасываем, чтобы повторный prep
                // (через бюджетную prep-ветку take_work) запросил его снова.
                j.peak_file = 0;
                j.defer_until =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                return;
            }
        }

        // Декод исходника собственным декодером формата: для наших форматов это надёжнее
        // ffmpeg (есть известное расхождение ffmpeg и wvunpack для wavpack 5.9). Иначе — ffmpeg.
        std::string derr;
        bool decoded = src_decoded;
        if (!decoded) {
            const config::Format* src_fmt = find_source_fmt(probe, j.path, fmts);
            if (src_fmt) {
                DecodeStatus ds = decode_source_native(src_fmt, j.path, ref_wav, bits,
                                                       &j.kill_requested);
                if (ds == DecodeStatus::NeedsCopy) {
                    std::string copy = util::join_path(j.session->dir(),
                                                       "src_copy." + lower_ext(j.path));
                    if (util::copy_file(j.path, copy)) {
                        ds = decode_source_native(src_fmt, copy, ref_wav, bits,
                                                  &j.kill_requested);
                        util::remove_file(copy);
                    }
                }
                decoded = (ds == DecodeStatus::Ok);
            }
        }
        if (!decoded)
            decoded = media::decode_to_wav(j.path, ref_wav, ffmpeg, bits, &derr,
                                           &j.kill_requested);
        if (proc::aborted() || j.kill_requested.load(std::memory_order_relaxed)) {
            release_deferred_budget();
            if (j.session) j.session->cleanup();
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

        // Задачи: формат + вариант. Порядок = детерминированный тай-брейк.
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
            // Восстановление: фильтруем по единственному целевому формату.
            if (j.mode == JobMode::Restore && !j.restore_to.empty() &&
                f.id != j.restore_to)
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
                // Восстановление берёт максимальное сжатие (последний вариант).
                if (j.mode == JobMode::Restore && vi + 1 < f.variants.size()) continue;
                j.tasks.push_back({fi, vi});
            }
            // Восстановление: вариантов/форматов больше не требуется.
            if (j.mode == JobMode::Restore && !j.restore_to.empty()) break;
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

        if (proc::cancelled() || proc::aborted() ||
            j.kill_requested.load(std::memory_order_relaxed))
            return VariantOutcome::Cancelled;

        // CPU-снимок в начале задачи: разность на момент записи rec — затраты на
        // этот вариант (внешние процессы через proc::run + собственный поток),
        // не зависящие от планировщика/приоритета окна.
        uint64_t cpu0 = proc::child_cpu_ms() + proc::thread_cpu_ms();

        std::string candidate = j.session->candidate_path(f.id, v.id, f.extension);
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
            rec["cpu_ms"] = proc::child_cpu_ms() + proc::thread_cpu_ms() - cpu0;
            if (logger) {
                logger->event({{"type", "candidate"},
                               {"file", j.path},
                               {"format", f.id},
                               {"variant", v.id},
                               {"status", "error"},
                               {"error", err},
                               {"cpu_ms", rec["cpu_ms"]}});
            }
            j.stat_records.push_back(std::move(rec));
        };

        // Stall detection: если файл не растёт и CPU ≈ 0 120 сек → kill.
        // Hard timeout: max(1800, wav_bytes/50KBps) — защита от infinite loop.
        proc::OutputMonitor mon;
        mon.path = candidate;
        mon.stall_timeout_sec = 120;
        mon.hard_timeout_sec = env.encode_timeout > 0 ? env.encode_timeout : 1800;
        if (j.ref_size > 0) {
            // Консервативная оценка: 50 КБ/с для самого медленного кодека.
            uint64_t proportional = j.ref_size / 50000;
            if (proportional > (uint64_t)mon.hard_timeout_sec)
                mon.hard_timeout_sec = (int)std::min(proportional, (uint64_t)7200);
        }

        // В режиме All каждый кандидат полностью проверяется здесь.
        // Winner/None — только кодирование; победитель валидируется в finalize_file
        // (режим Winner) или не проверяется вовсе (None).
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

        // План тегов и sidecar: зависит от (файл, формат), не от варианта.
        // Sidecar пишется один раз на формат и общий для всех его вариантов.
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
                    // write_sidecar ожидает базу без .tags.zip — передаём dir + fmt_id
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

        // Финальная проверка тегов
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
        if (logger) {
            logger->event({{"type", "candidate"},
                           {"file", j.path},
                           {"format", f.id},
                           {"variant", v.id},
                           {"status", "ok"},
                           {"size", cand.size},
                           {"sidecar", cand.sidecar},
                           {"cost", cand.cost},
                           {"cpu_ms", rec["cpu_ms"]}});
        }

        // Жадный отбор: лучший держим, проигравших удаляем сразу. Кандидат,
        // который не меньше исходного файла, бесполезен — удаляем сразу, даже
        // если он единственный (победителем его не делаем). Sidecar не трогаем:
        // он общий для формата и может ещё понадобиться другим вариантам.
        {
            std::lock_guard<std::mutex> lk(*j.m);
            if (j.mode == JobMode::Restore) {
                // Восстановление не про экономию: победителем становится
                // единственный кандидат, даже если он больше исходника.
                if (j.best_valid) util::remove_file(j.best.path);
                j.best = cand;
                j.best_order = cand.order;
                j.best_valid = true;
                j.any_passed = true;
                j.stat_records.push_back(std::move(rec));
                return VariantOutcome::Ok;
            }
            if (cand.cost >= j.probe.size) {
                // Доставка в целевую папку (optimize+target_dir): кандидат не
                // обязан быть меньше исходника — лучший сохраняется и будет
                // скопирован в цель (полная конвертация пачки).
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

    // --- финализация файла: отчёт, замена исходника, чистка tmp ---
    // Вызывается ровно один раз (потоком, завершившим последнюю задачу).
    void finalize_file(FileJob& j) {
        const Options& opts = *this->opts;
        // Показываем, что строка вошла в финализацию: remove_file (см.) сможет
        // доставить kill_requested даже при done==true. Погасить флаг не нужно —
        // файл после финализации остаётся done.
        j.finalizing.store(true, std::memory_order_relaxed);

        std::unique_lock<std::mutex> lk(*j.m);
        std::vector<json::json> records = std::move(j.stat_records);
        if (j.cancelled) {
            // Файл снят из очереди во время обработки (remove/cancel-file):
            // доработавшие задачи игнорируются, замена запрещена, tmp чистится.
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

        // Restore, вход уже в целевом формате: prep пометил файл ok и не строил
        // задач — победителей нет, доставлять нечего, но ветка выполняется для
        // итогового статуса/сообщения так же, как обычный прогон.

        std::string msg;
        char buf[512];

        if (j.summary.status == "error") {
            // ошибка уже зафиксирована (ffprobe/ffmpeg, probe, декод, исключение)
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
            // Победителя нет: либо ни один вариант не прошёл валидацию, либо все
            // прошедшие были не меньше исходного файла (и удалены при отборе).
            // Операционные сбои (упали все варианты, утилиты недоступны) —
            // это ошибка файла; «чистый» skip остаётся для намеренных причин
            // (нет подходящих кандидатов из-за caps, все >= оригинала и т.п.).
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
                // Все этапы оптимизации прошли без ошибок, но ни один кандидат
                // не оказался меньше исходного (или все прошедшие валидацию были
                // не меньше). Это удовлетворительный результат прогона: файл
                // остаётся на месте, но рассматривается как успешно завершённый.
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
            // Ошибка хотя бы одного варианта — это ошибка файла: победитель не
            // заменяется, прогон останавливается (см. блок по status == "error").
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

            // Режим Winner: кандидаты при отборе не проверялись — валидируем победителя
            // перед заменой. Провал — это ошибка файла (а не «неудачный вариант»): исходник
            // не заменяется, разбор причин обязателен.
            std::string winner_fail;
            if (opts.verify == Verify::Winner) {
                // Валидация победителя — долгая операция (полный декод в WAV и
                // побайтовое сравнение с референсом). Не удерживаем j.m на её
                // время: иначе snapshot (RPC демона: cancel/stop/restart/counters)
                // блокировался бы на всё время decode+compare. Копия Env снимается
                // под локом, сама проверка исполняется вне критической секции и
                // прерываема через kill_requested (per-file стоп). Если по ходу
                // пришла отмена — финализация сворачивается в stopped ниже.
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
                // Доставка в цель при оптимизации может быть не меньше исходника
                // (полная конвертация пачки) — такие файлы показываются как 0.0%.
                // Восстановление при этом не трогаем: пересжатие в FLAC обычно
                // Увеличивает размер, и честный (отрицательный) процент — норма
                // режима, UI рисует его нейтральным серым.
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

                // Замена на месте: исходник трогаем только здесь.
                if (j.mode == JobMode::Restore) {
                    // Восстановление: единственный кандидат переносится в место
                    // назначения (целевая папка либо на место исходника). Отсечки
                    // по размеру нет — restore не про экономию, а про формат.
                    std::string ext = fmt_ext(best.format, *fmts);
                    bool delivered = false;
                    if (opts.dry_run) {
                        j.summary.detail = i18n::str("dry-run — no write");
                        delivered = true;
                    } else if (!j.target_dir.empty()) {
                        // Целевая папка: структура повторяет rel-подкаталог пачки,
                        // исходник остаётся на месте.
                        std::string rel_dir = util::dir_name(j.rel);
                        std::string dst_dir =
                            rel_dir.empty() ? j.target_dir
                                            : util::join_path(j.target_dir, rel_dir);
                        util::mkdirs(dst_dir);
                        std::string dst = util::join_path(dst_dir, j.base_ne + "." + ext);
                        std::error_code ec;
                        // Путь кандидата sidecar (в tmp-сессии) фиксируем ДО переноса
                        // самого кандидата: fallback best.path+".tags.zip" перестанет
                        // существовать после rename best.path -> dst.
                        auto pit = j.fmt_plans.find(best.format);
                        std::string sc_src =
                            pit != j.fmt_plans.end()
                                ? pit->second.sidecar_path
                                : best.path + ".tags.zip";
                        // Перенос на том же томе (rename), иначе копия + удаление.
                        // Не удерживаем j.m: файл готов (best.path), перенос/копия
                        // на одном диске не должна блокировать snapshot (RPC).
                        lk.unlock();
                        fs::rename(fs::u8path(best.path), fs::u8path(dst), ec);
                        if (ec) {
                            if (util::copy_file(best.path, dst)) {
                                util::remove_file(best.path);
                                ec.clear();
                            }
                        }
                        if (!ec && best.sidecar > 0) {
                            // Транзакционная доставка sidecar в целевую папку.
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
                        // Замена на месте (схема, согласованная с персистентностью):
                        // кандидат копируется в .llao-tmp.<ext>, старый файл
                        // удаляется, кандидат переименовывается на место; sidecar
                        // доставляется той же транзакцией (см. deliver_sidecar).
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
                    // Целевая папка: копируем лучшего кандидата в target_dir/<rel-подкаталог>,
                    // оригинал остаётся на месте. При доставке в цель кандидат не обязан
                    // быть меньше исходника (полная конвертация пачки).
                    std::string ext = fmt_ext(best.format, *fmts);
                    std::string rel_dir = util::dir_name(j.rel);
                    std::string dst_dir =
                        rel_dir.empty() ? j.target_dir
                                        : util::join_path(j.target_dir, rel_dir);
                    util::mkdirs(dst_dir);
                    std::string dst = util::join_path(dst_dir, j.base_ne + "." + ext);
                    std::error_code ec;
                    // Путь кандидата sidecar фиксируем ДО переноса самого кандидата.
                    auto pit = j.fmt_plans.find(best.format);
                    std::string sc_src =
                        pit != j.fmt_plans.end() ? pit->second.sidecar_path
                                                 : best.path + ".tags.zip";
                    // Не удерживаем j.m на переносе/копии в цель: файл готов,
                    // доставка на одном диске не должна блокировать snapshot (RPC).
                    lk.unlock();
                    fs::rename(fs::u8path(best.path), fs::u8path(dst), ec);
                    if (ec) {
                        // Чужая файловая система: копия + удаление tmp-кандидата.
                        if (util::copy_file(best.path, dst)) {
                            util::remove_file(best.path);
                            ec.clear();
                        }
                    }
                    if (!ec && best.sidecar > 0) {
                        // Транзакционная доставка sidecar в целевую папку.
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
                    // Физическая замена не удерживает j.m: копия кандидата в папку
                    // файла и rename могут быть заметными по времени, snapshot (RPC)
                    // не должен ждать их завершения.
                    lk.unlock();
                    bool copied = util::copy_file(best.path, tmp_name);
                    util::ReplaceResult rr;
                    if (copied)
                        rr = util::replace_file(j.path, tmp_name, new_path);
                    lk.lock();
                    if (copied) {
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

        // Освобождаем файловый бюджет и чистим временные файлы.
        // Удаляем побеждшего кандидата из tmp (он уже скопирован на место).
        if (j.best_valid) util::remove_file(j.best.path);
        // RAII: remove_all почистит ref.wav, dec.wav, sidecar, losers.
        discard_job_tmp(j);

        // Инвариант: после корректного завершения job'а временные файлы обязаны
        // быть удалены. Если что-то их пересоздало — добиваем и отмечаем.
        if (j.session) {
            if (logger) {
                logger->event({{"type", "tmp_cleanup_warn"}, {"file", j.path}});
            }
            j.session.reset();
        }

        if (!msg.empty()) obs::sink()->log(msg);
        // Единая модель ошибки: событие уже эмитил error_file на месте сбоя.
        // Если путь ошибки не доложил причину (редкий fallback) — доложим по
        // summary.detail; двойного события error_file быть не должно.
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
            // Итоговый путь (целевая папка / новое расширение) — для label UI.
            if (!j.out_path.empty()) obs::sink()->out_file(j.idx, j.out_path);
        }
        if (j.summary.status == "error") {
            if (opts.ignore_errors || opts.mode == SessionMode::Daemon) {
                // Реальная ошибка файла в демон-режиме не прерывает очередь:
                // файл остаётся со статусом error, прогон продолжается.
                if (j.summary.detail.empty()) j.summary.detail = i18n::str("error ignored");
            } else {
                count_error_locked(j);
                abort.store(true);
                proc::abort_all();  // не ждём завершения активных процессов — прерываем их
            }
        }
    }

    void worker() {
        // Вспомогательные потоки не должны мешать интерфейсу (ввод/отрисовка):
        // под нагрузкой на CPU статусбар иначе заметно тормозит.
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
                if (shutdown_requested.load()) break;   // демон: штатное завершение
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
                if (proc::cancelled() || proc::aborted()) break;  // отмена — счётчики не трогаем, tmp почистит main
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
                std::vector<obs::TaskInfo> infos;  // метаданные задач (для set_tasks)
                {
                    std::lock_guard<std::mutex> lk(qm);
                    FileJob& j = *w.job;
                    // Отложенный файл (не хватило дискового бюджета): не закрываем,
                    // оставляем кандидатом на повторный prep, когда бюджет освободится.
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
                            // Контракт порядка событий: set_tasks/set_excluded
                            // обязаны дойти до зеркала РАНЬШЕ любого task(Running)
                            // по этому файлу. Эмитим их под qm, до cv.notify_all():
                            // воркер варианта не может выпустить задачу, пока мы
                            // держим блокировку (иначе на многопоточности на LLaO
                            // было видно task(Running) ДО set_tasks — см. golden).
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
                if (proc::cancelled() || proc::aborted()) break;  // отмена/прерывание — счётчики не трогаем
                // Без --ignore-errors (и не в режиме демона) любая ошибка
                // варианта останавливает прогон: воркеры перестают брать новые
                // задачи, а файл ниже финализируется как ошибка (см. finalize_file).
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
                    // Резерв этой задачи возвращается всегда и ровно один раз
                    // (см. take_work_locked) — в т.ч. для последней задачи
                    // файла, иначе бюджет утекает и очередь встаёт навсегда.
                    if (w.disk_reserved > 0) {
                        rm.release_disk(w.disk_reserved);
                        w.disk_reserved = 0;
                    }
                    // Файл завершён, когда обработаны все задачи — либо, если
                    // он снят из очереди (cancelled), когда обработаны все
                    // уже выпущенные задачи (остальные не будут запущены).
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
};

}  // namespace

void clear_session_tmp_dir(const std::string& custom) {
    clear_session_tmp_dir_impl(custom);
}

// ---------------------------------------------------------------------------
// Engine: долгоживущий движок для демона (Phase 0).
// Владеет копией Options, набором форматов, Runner + пулом воркеров.
// Принимает файлы на лету (add), отдаёт снимок очереди (snapshot),
// позволяет управлять очередью (remove/reorder/pause/resume) и завершает
// воркеры (shutdown).
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

    // Загрузка конфигов и сортировка по статистике (общая для run() и демона).
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

    // Собирает входы в FileItems. Возвращает false, если не найдено ни одного.
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

    // Демон всегда работает в режиме Daemon: ошибки файлов не прерывают
    // очередь, воркеры живут до shutdown, а не до опустошения очереди.
    i.opts = opts;
    i.opts.mode = SessionMode::Daemon;

    if (!i.load_formats(err)) return 1;

    std::vector<FileItem> items;
    i.collect(initial_inputs, items, nullptr);

    i.jobs = resolve_jobs(i.opts.jobs, i.opts.jobs_float);

    // Изолированный tmp-каталог демона. Демон — единственный процесс
    // (single-instance) и единственный хозяин базового tmp, поэтому пишем
    // прямо в базу без подпапки по PID: подпапка одного файла — tmp/<hash>/.
    // При старте убираем базу целиком: очищаются остатки аварийно завершённых
    // сессий (kill -9, крах) и следы сирот прошлых запусков.
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
        // Симметрия с append_files(демон): у каждой строки есть job_meta.
        // Здесь это всегда первичная пачка одноразового прогона (mode=optimize,
        // без целевой папки); демон стартует с пустым initial_inputs и никогда
        // не проходит этот путь.
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
    // Мгновенная остановка: убить активные дочерние процессы сразу
    // (проверяется в цикле опроса proc::run), затем ждать воркеры.
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
    // Демон — единственный процесс: после остановки воркеров временные файлы
    // никому не нужны, убираем всю базу целиком.
    clear_tmp_base(i.opts.tmp_dir);
    i.started = false;
}

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

    // Изолированный каталог tmp/<pid>: чужие прогоны не пересекаются. Остатки
    // своей подпапки (обрыв прошлого запуска с тем же PID) убираем заранее.
    clear_session_tmp_dir(opts.tmp_dir);
    std::string tmp = session_tmp_dir(opts.tmp_dir);

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

    // Сводка по файлам, которые не удалось заменить. Печатается после остановки
    // всех воркеров, когда их вывод уже завершился, чтобы текст не перемешался.
    {
        bool any = false;
        for (auto& _j : r.jobs) { auto& j = *_j;
            if (j.summary.replacement_error.empty()) continue;
            if (!any) out::print("%s", i18n::str("Replacement failed:\n").c_str());
            out::print("  %s — %s\n", j.path.c_str(), j.summary.replacement_error.c_str());
            any = true;
        }
    }

    // Убираем свою подпапку временных файлов целиком (эталонные WAV, кандидаты).
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
// Восстановление (restore): декод -> пережатие в целевой формат -> теги обратно
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

    // RAII-сессия для tmp-файлов этого restore.
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

    // Restore всегда выполняет полную проверку кандидата (единственный вариант).
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
        variant = &target->variants.back();  // последний = максимальное сжатие
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
