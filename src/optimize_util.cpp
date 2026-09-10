#include "optimize_internal.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <set>
#include <thread>

#include "i18n.h"
#include "util.h"

namespace optimize {

namespace {
std::atomic<uint64_t> g_session_counter{0};
}  // namespace

std::string session_cookie() {
    return util::process_id() + "-" + std::to_string(g_session_counter.fetch_add(1));
}

void reset_session_counter() { g_session_counter.store(0); }

std::string norm_path(const std::string& p) {
    std::string s = p;
    for (auto& c : s)
        if (c == '\\') c = '/';
    return s;
}

int resolve_jobs(double jobs, bool jobs_float) {
    unsigned hw = std::thread::hardware_concurrency();
    if (hw < 1) hw = 1;
    double n;
    if (jobs <= 0) {
        n = hw * 2.0;
    } else if (jobs_float) {
        n = hw * jobs;
    } else {
        n = jobs;
    }
    int j = n <= 1.0 ? 1 : (int)(n + 0.5);
    return j;
}

std::set<std::string> supported_extensions(const std::vector<config::Format>& fmts) {
    std::set<std::string> s;
    for (const auto& f : fmts)
        if (!f.extension.empty()) s.insert(util::to_lower(f.extension));
    for (const auto& e : config::input_extensions()) s.insert(e);
    return s;
}

bool is_supported_file(const std::string& path, const std::set<std::string>& exts) {
    std::string base = util::to_lower(util::base_name(path));
    size_t dot = base.find_last_of('.');
    if (dot == std::string::npos) return false;
    return exts.count(base.substr(dot + 1)) != 0;
}

void collect_files(const std::string& p, std::vector<FileItem>& out, std::string* err,
                   const std::vector<config::Format>& fmts) {
    std::set<std::string> exts = supported_extensions(fmts);
    if (util::file_exists(p)) {
        if (is_supported_file(p, exts))
            out.push_back({p, util::base_name(p), util::abs_path(util::dir_name(p))});
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
                out.push_back({f, rel, util::abs_path(p)});
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

std::string base_tmp_dir(const std::string& custom) {
    return custom.empty() ? tmp_dir() : custom;
}

std::string session_tmp_dir(const std::string& custom) {
    std::string d = util::join_path(base_tmp_dir(custom), util::process_id());
    util::mkdirs(d);
    return d;
}

void clear_session_tmp_dir_impl(const std::string& custom) {
    std::error_code ec;
    fs::remove_all(fs::u8path(session_tmp_dir(custom)), ec);
}

void clear_tmp_base(const std::string& custom) {
    std::string d = base_tmp_dir(custom);
    std::error_code ec;
    fs::remove_all(fs::u8path(d), ec);
    util::mkdirs(d);
    reset_session_counter();
}

uint64_t estimated_wav_bytes(const media::Probe& probe, int bits) {
    int bps = bits > 0 ? bits : 16;
    uint64_t ch = probe.channels > 0 ? (uint64_t)probe.channels : 2;
    uint64_t sr = probe.sample_rate > 0 ? (uint64_t)probe.sample_rate : 44100;
    double dur = probe.duration > 0.0 ? probe.duration : 60.0;
    return 44 + ch * (bps / 8) * sr * (uint64_t)(dur + 1.0);
}

uint64_t file_peak_bytes(uint64_t wav, Verify v) {
    return wav * ((v == Verify::None) ? 3 : 4);
}

uint64_t variant_peak_bytes(uint64_t wav, Verify v) {
    return wav * ((v == Verify::None) ? 1 : 2);
}

std::string lower_ext(const std::string& path) {
    std::string b = util::base_name(path);
    size_t dot = b.find_last_of('.');
    if (dot == std::string::npos) return "";
    return util::to_lower(b.substr(dot + 1));
}

const config::Format* find_source_fmt(const media::Probe& probe, const std::string& path,
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

std::string fmt_ext(const std::string& id, const std::vector<config::Format>& fmts) {
    for (const auto& f : fmts)
        if (f.id == id) return f.extension;
    return id;
}

const char* verify_name(Verify v) {
    switch (v) {
        case Verify::All: return "all";
        case Verify::Winner: return "winner";
        default: return "none";
    }
}

std::vector<tags::TagType> native_types(const config::Format& f) {
    std::vector<tags::TagType> v;
    tags::TagType t = tags::tag_type_from_string(f.tag_system);
    if (t != tags::TagType::unknown) v.push_back(t);
    return v;
}

}  // namespace optimize
