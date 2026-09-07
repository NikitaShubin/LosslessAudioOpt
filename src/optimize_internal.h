#pragma once
// Внутренний заголовок модуля оптимизации: общие типы и объявления функций,
// разделяемые между optimize_util.cpp / optimize_codec.cpp / optimize_runner.cpp.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "config.h"
#include "media.h"
#include "optimize.h"
#include "proc.h"
#include "report.h"
#include "tags.h"

namespace optimize {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Общие типы
// ---------------------------------------------------------------------------

// Собранный файл: полный путь + путь относительно заданного корня.
struct FileItem {
    std::string path;
    std::string rel;
};

enum class DecodeStatus { Ok, NeedsCopy, Failed };

// Оценка размера WAV-файла по данным ffprobe (до декодирования).
uint64_t estimated_wav_bytes(const media::Probe& probe, int bits);

// Пиковый след файла на диске (уровень 1 — файловый бюджет).
uint64_t file_peak_bytes(uint64_t wav, Verify v);

// Инкрементальный след одного варианта (уровень 2 — задачевый бюджет).
uint64_t variant_peak_bytes(uint64_t wav, Verify v);

// ---------------------------------------------------------------------------
// Утилиты
// ---------------------------------------------------------------------------

std::string norm_path(const std::string& p);
int resolve_jobs(double jobs, bool jobs_float);
std::set<std::string> supported_extensions(const std::vector<config::Format>& fmts);
bool is_supported_file(const std::string& path, const std::set<std::string>& exts);
void collect_files(const std::string& p, std::vector<FileItem>& out, std::string* err,
                   const std::vector<config::Format>& fmts);
std::string base_tmp_dir(const std::string& custom);
std::string tmp_dir();
std::string base_no_ext(const std::string& path);
std::string tmp_token(const std::string& path);
std::string session_cookie();
void reset_session_counter();
std::string session_tmp_dir(const std::string& custom);
void clear_session_tmp_dir_impl(const std::string& custom);
void clear_tmp_base(const std::string& custom);
std::string lower_ext(const std::string& path);
const config::Format* find_source_fmt(const media::Probe& probe, const std::string& path,
                                      const std::vector<config::Format>& fmts);

// ---------------------------------------------------------------------------
// Кодек: Env, кодирование, декодирование, валидация
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

const char* pcm_codec(int bits);

std::string subst(const std::string& s, const std::string& key, const std::string& val);
std::vector<std::string> build_cmd(const std::vector<std::string>& tmpl,
                                   const std::string& binary, const std::string& in,
                                   const std::string& out,
                                   const std::vector<std::string>& params,
                                   const std::string& codec, const std::string& container);
std::string decoder_path(const config::Format& fmt, const std::string& encoder);
DecodeStatus decode_source_native(const config::Format* src_fmt, const std::string& path,
                                  const std::string& out_wav, int bits,
                                  const std::atomic<bool>* kill = nullptr,
                                  const proc::OutputMonitor* mon = nullptr);
std::string encode_candidate(const std::string& wav, const std::string& candidate,
                             const std::vector<std::string>& params, const Env& env,
                             const proc::OutputMonitor& monitor = {},
                             const std::atomic<bool>* kill = nullptr);
bool remove_dec_wav(const std::string& p);
std::string validate_candidate(const std::string& wav, const std::string& candidate,
                               const Env& env, const std::atomic<bool>* kill = nullptr);
std::string fmt_ext(const std::string& id, const std::vector<config::Format>& fmts);
bool deliver_sidecar(const std::string& sc_src, bool need,
                     const std::string& dir, const std::string& base_ne,
                     std::string* note);
const char* verify_name(Verify v);
std::vector<tags::TagType> native_types(const config::Format& f);

// ---------------------------------------------------------------------------
// FileSession: RAII-обёртка для tmp-файлов одного исходного файла.
// ---------------------------------------------------------------------------

class FileSession {
public:
    FileSession(const std::string& original_path, const std::string& tmp_base);
    ~FileSession();

    FileSession(const FileSession&) = delete;
    FileSession& operator=(const FileSession&) = delete;
    FileSession(FileSession&& o) noexcept;
    FileSession& operator=(FileSession&& o) noexcept;

    const std::string& dir() const { return dir_; }
    const std::string& original_path() const { return path_; }
    std::string ref_wav_path() const;
    std::string candidate_path(const std::string& fmt_id,
                               const std::string& variant_id,
                               const std::string& ext) const;
    std::string sidecar_path(const std::string& fmt_id) const;
    void cleanup();
    bool ok() const { return !dir_.empty(); }

private:
    std::string dir_;
    std::string path_;
};

// ---------------------------------------------------------------------------
// DiskBudget + ResourceManager: брокер ресурсов
// ---------------------------------------------------------------------------

struct DiskBudget {
    std::mutex m;
    uint64_t reserved = 0;
    uint64_t min_free = 1ull << 30;

    bool try_reserve(const std::string& tmp_path, uint64_t bytes);
    void release(uint64_t bytes);
};

struct ResourceRequest {
    uint64_t disk_bytes = 0;
    enum class Status { Granted, Deferred, Denied };
    Status status = Status::Deferred;
};

// Forward-declare FileJob для ResourceManager.
struct FileJob;

class ResourceManager {
public:
    ResourceManager() = default;
    ResourceManager(const std::string& tmp_path, int max_workers, uint64_t min_free = 1ull << 30);

    ResourceRequest request_disk(uint64_t bytes);
    void release_disk(uint64_t bytes);
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

// ---------------------------------------------------------------------------
// Планировщик: задачи, варианты, файлы
// ---------------------------------------------------------------------------

struct FmtPlan {
    tags::TagPlan plan;
    uint64_t sidecar_size = 0;
    std::string sidecar_path;
};

struct TaskDesc {
    size_t fmt_idx = 0;
    size_t variant_idx = 0;
};

enum class VariantOutcome { Ok, Failed, Cancelled };

struct FileJob {
    size_t idx = 0;
    std::string path;
    std::string base;
    std::string rel;
    std::string base_ne;
    std::string dir;
    std::string tok;

    JobMode mode = JobMode::Optimize;
    std::string target_dir;
    std::string restore_to;

    std::unique_ptr<FileSession> session;

    std::vector<TaskDesc> tasks;
    size_t released = 0;
    size_t completed = 0;
    bool prep_done = false;
    bool prep_running = false;
    bool done = false;
    bool cancelled = false;
    std::atomic<bool> kill_requested{false};
    std::atomic<bool> finalizing{false};

    bool prep_ok = false;
    uint64_t ref_size = 0;
    uint64_t wav_est = 0;
    uint64_t peak_file = 0;
    uint64_t prep_wall_ms = 0;    // wall-clock подготовки файла (probe+decode)
    uint64_t decode_wall_ms = 0;  // wall-clock декодирования эталонного WAV
    bool deferred = false;
    std::chrono::steady_clock::time_point defer_until{};
    int bits = 16;
    media::Probe probe;
    tags::TagSet ts;
    std::map<std::string, Env> envs;
    std::map<std::string, FmtPlan> fmt_plans;

    std::unique_ptr<std::mutex> m;
    bool any_passed = false;
    bool best_valid = false;
    Candidate best;
    size_t best_order = SIZE_MAX;
    std::vector<nlohmann::json> stat_records;
    std::vector<std::string> failures;
    std::vector<std::string> exclusions;
    std::vector<std::string> excluded_fmts;
    int variant_errors = 0;
    int tool_errors = 0;
    bool error_counted = false;
    report::FileSummary summary;
    bool early_ok = false;
    std::string out_path;
    bool error_reported = false;
};

// ---------------------------------------------------------------------------
// Runner: ядро планировщика (определения методов — в optimize_runner.cpp)
// ---------------------------------------------------------------------------

struct Runner {
    const Options* opts = nullptr;
    const std::vector<config::Format>* fmts = nullptr;
    report::Logger* logger = nullptr;
    std::string ffprobe;
    std::string ffmpeg;
    std::string tmp;
    int window = 1;

    ResourceManager rm;

    std::chrono::steady_clock::time_point res_last{};
    uint64_t ram_budget = 0;

    std::mutex qm;
    std::condition_variable cv;
    size_t next_prep = 0;
    int prep_active = 0;
    size_t total_done = 0;
    std::vector<std::unique_ptr<FileJob>> jobs;
    std::unordered_set<std::string> seen_paths_;
    std::atomic<int> failed{0};
    std::atomic<bool> abort{false};
    std::atomic<bool> shutdown_requested{false};
    std::atomic<bool> queue_paused{false};
    std::atomic<int> workers_alive{0};

    enum class WorkKind { None, Prep, Variant };
    struct Work {
        WorkKind kind = WorkKind::None;
        size_t idx = 0;
        FileJob* job = nullptr;
        size_t task = 0;
        uint64_t disk_reserved = 0;
    };

    void make_job(FileJob& j, size_t idx, const FileItem& it);
    std::vector<size_t> append_files(const std::vector<FileItem>& items,
                                     const AddOptions& ao = {});
    void pause_queue();
    void resume_queue();
    bool is_paused() const;
    size_t find_pos_locked(size_t idx) const;
    void remove_file(size_t idx);
    void discard_job_tmp(FileJob& j);
    bool reorder(const std::vector<size_t>& ids);
    bool all_done_locked() const;
    void count_error(FileJob& j);
    void count_error_locked(FileJob& j);
    bool variant_launchable_locked();
    bool deferred_retry_locked();
    bool find_next_prep_locked(size_t* out);
    bool prep_allowed_locked() const;
    bool take_work_locked(Work* w);
    void prep_file(FileJob& j);
    VariantOutcome run_variant(FileJob& j, size_t task_idx);
    void finalize_file(FileJob& j);
    void worker();
};

}  // namespace optimize
