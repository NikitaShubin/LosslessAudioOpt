#include "report.h"

#include <cstdio>
#include <ctime>
#include <filesystem>

#include "i18n.h"
#include "out.h"
#include "util.h"

namespace json = nlohmann;

namespace report {

namespace {

std::string strftime_str(const char* fmt) {
    std::time_t t = std::time(nullptr);
    std::tm tmv = {};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[64] = {0};
    std::strftime(buf, sizeof(buf), fmt, &tmv);
    return buf;
}

}  // namespace

std::string timestamp() { return strftime_str("%Y%m%d-%H%M%S"); }

Logger::Logger(const std::string& dir) {
    if (dir.empty()) return;
    if (!util::mkdirs(dir)) return;
    path_ = util::join_path(dir, "llao-" + strftime_str("%Y%m%d-%H%M%S") + ".jsonl");
    f_.open(std::filesystem::u8path(path_), std::ios::binary | std::ios::out | std::ios::trunc);
    ok_ = f_.is_open() && f_.good();
}

Logger::~Logger() {
    if (f_.is_open()) f_.close();
}

void Logger::event(const json::json& ev) {
    std::lock_guard<std::mutex> lk(m_);
    if (!ok_) return;
    json::json j = ev;
    if (!j.contains("ts")) j["ts"] = strftime_str("%Y-%m-%dT%H:%M:%S");
    util::sanitize_json(j);
    f_ << j.dump() << "\n";
    f_.flush();
}

void write_report(const std::string& path, const std::vector<FileSummary>& files) {
    std::string text;
    char buf[1024];

    size_t n_ok = 0, n_skip = 0, n_err = 0, n_replaced = 0;
    uint64_t t_orig = 0, t_best = 0;
    for (const auto& f : files) {
        if (f.status == "ok") n_ok++;
        else if (f.status == "skip") n_skip++;
        else n_err++;
        if (f.replaced) n_replaced++;
        t_orig += f.original;
        t_best += f.best;
    }

    text += i18n::str("LLAO — Lossless Audio Optimizer. Run report\n");
    text += i18n::str("Generated: ") + strftime_str("%Y-%m-%d %H:%M:%S") + "\n\n";
    snprintf(buf, sizeof(buf), "%s",
             i18n::fmt("Files: %zu (ok: %zu, skipped: %zu, errors: %zu), replaced: %zu\n",
                       files.size(), n_ok, n_skip, n_err, n_replaced).c_str());
    text += buf;

    if (n_ok > 0 && t_orig > 0) {
        double ratio = t_best > 0 ? 100.0 * (1.0 - (double)t_best / (double)t_orig) : 0.0;
        snprintf(buf, sizeof(buf), "%s",
                 i18n::fmt("Total source size: %.2f MB, result: %.2f MB (savings %.2f%%)\n\n",
                           t_orig / 1048576.0, t_best / 1048576.0, ratio).c_str());
        text += buf;
    }

    if (!files.empty()) {
        text += i18n::str("File | Status | Format | Variant | Was (MB) | Now (MB) | Savings\n");
        text += "-----|--------|--------|---------|-----------|------------|---------\n";
        for (const auto& f : files) {
            std::string name = util::base_name(f.path);
            std::string savings;
            if (f.status == "ok" && f.original > 0) {
                snprintf(buf, sizeof(buf), "%.2f%%", f.savings_pct);
                savings = buf;
            } else {
                savings = "-";
            }
            snprintf(buf, sizeof(buf), "%s | %s | %s | %s | %.2f | %.2f | %s",
                     name.c_str(), f.status.c_str(), f.best_format.c_str(),
                     f.best_variant.c_str(),
                     f.original / 1048576.0, f.best / 1048576.0, savings.c_str());
            text += buf;
            if (!f.detail.empty()) text += " (" + f.detail + ")";
            text += "\n";
        }
        text += "\n";
    }

    bool has_excl = false;
    for (const auto& f : files) {
        if (!f.exclusions.empty()) { has_excl = true; break; }
    }
    if (has_excl) {
        text += i18n::str("Exclusion reasons:\n");
        for (const auto& f : files) {
            if (f.exclusions.empty()) continue;
            text += "  " + util::base_name(f.path) + ":\n";
            for (const auto& e : f.exclusions) text += "    - " + e + "\n";
        }
    }

    if (!util::write_text(path, text)) {
        out::error("ERROR: could not write the report to %s\n", path.c_str());
    } else {
        out::print("Report saved: %s\n", path.c_str());
    }
}

}  // namespace report
