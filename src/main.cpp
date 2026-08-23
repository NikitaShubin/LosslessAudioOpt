#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

#ifdef _WIN32
#include <csignal>
#else
#include <signal.h>
#endif

#include "config.h"
#include "i18n.h"
#include "optimize.h"
#include "out.h"
#include "proc.h"
#include "stats.h"
#include "tool.h"
#include "util.h"

namespace {

#ifdef _WIN32
volatile LONG g_ctrl_count = 0;
#else
volatile sig_atomic_t g_ctrl_count = 0;
#endif

std::string g_atexit_tmp_dir;

void atexit_cleanup() {
    if (!g_atexit_tmp_dir.empty()) {
        optimize::clear_session_tmp_dir(g_atexit_tmp_dir);
    } else {
        optimize::clear_session_tmp_dir(std::string());
    }
}

#ifdef _WIN32
BOOL WINAPI ctrl_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        if (InterlockedIncrement(&g_ctrl_count) == 1) {
            proc::cancel();
        } else {
            ExitProcess(130);
        }
        return TRUE;
    }
    return FALSE;
}
#else
void sigint_handler(int) {
    if (g_ctrl_count == 0) {
        g_ctrl_count = 1;
        proc::cancel();
    } else {
        _exit(130);
    }
}
#endif

#ifdef _WIN32
std::vector<std::string> wide_argv() {
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wargv) return {};
    std::vector<std::string> out;
    for (int i = 0; i < argc; i++) out.push_back(util::w2u(std::wstring(wargv[i])));
    LocalFree(wargv);
    return out;
}
#endif

void usage() {
    out::print("LLAO — Lossless Audio Optimizer\n\n");
    out::print("Commands:\n");
#ifdef _WIN32
    const char* prog = "llao.exe";
#else
    const char* prog = "llao";
#endif
    out::print("  %s check-formats                       validate the formats/*.json schema\n", prog);
    out::print("  %s variants [fmt_id ...]               compression variants from formats/*.json\n", prog);
    out::print("  %s tools [fmt_id ...] [--no-download]  status/download of utilities into bin/<id>/\n", prog);
    out::print("  %s help <fmt_id> [--no-download] [-- <arguments>]  run the utility (--help)\n", prog);
    out::print("  %s stats                               show accumulated statistics\n", prog);
    out::print("  %s optimize <file|folder> [...]        main enumeration\n", prog);
    out::print("             [--jobs=N|M.F] [--formats=a,b] [--report=<file|folder>]\n");
    out::print("             [--no-download] [--dry-run] [--allow-lossy] [--debug] [--no-stats]\n");
    out::print("             [--no-status] [--verify=all|winner|none] [--ignore-errors] [--tmp=<path>]\n");
    out::print("  %s restore <file|folder> [...]         decode + re-encode to the target format\n", prog);
    out::print("             [--jobs=N|M.F] [--to=flac] [--variant=<id>] [--no-download]\n");
    out::print("             [--allow-lossy]\n");
    out::print("  --jobs=N exact thread count; --jobs=M.F multiplier of the CPU core count (default 2.0)\n");
    out::print("  optimize --debug writes the runs/*.jsonl log; --no-stats disables stats.json\n");
    out::print("  optimize --verify: all = check every candidate (default); winner = check only the\n");
    out::print("    best by size; none = no verification at all. Any file error aborts the run unless\n");
    out::print("    --ignore-errors is given (then such files are skipped and the run continues).\n");
}

int cmd_check_formats() {
    try {
        auto fmts = config::load_all();
        out::print("Configs are valid: %zu formats\n", fmts.size());
        for (const auto& f : fmts) {
            out::print("  %-16s %-26s [%s]\n", f.id.c_str(), f.name.c_str(),
                   i18n::str(f.enabled ? "enabled" : "disabled").c_str());
        }
        return 0;
    } catch (const std::exception& exc) {
        out::error("ERROR: %s\n", exc.what());
        return 1;
    }
}

int cmd_tools(const std::vector<std::string>& args) {
    std::vector<std::string> ids;
    bool no_download = false;
    for (const auto& a : args) {
        if (a == "--no-download") no_download = true;
        else ids.push_back(a);
    }
    try {
        auto fmts = config::load_all();
        if (!ids.empty()) {
            std::vector<config::Format> filtered;
            for (const auto& f : fmts) {
                if (std::find(ids.begin(), ids.end(), f.id) != ids.end()) filtered.push_back(f);
            }
            std::vector<std::string> unknown;
            for (const auto& id : ids) {
                bool found = false;
                for (const auto& f : fmts)
                    if (f.id == id) found = true;
                if (!found) unknown.push_back(id);
            }
            for (const auto& u : unknown) out::error("ERROR: unknown format '%s'\n", u.c_str());
            if (unknown.empty() && filtered.empty()) {
                out::error("ERROR: no formats to check\n");
                return 1;
            }
            fmts = filtered;
        }
        for (const auto& f : fmts) {
            tool::Status st = tool::ensure(f, !no_download, "[" + f.id + "] ");
            if (st.path.empty()) {
                if (!st.message.empty())
                    out::print("  %-16s %-10s %s\n", f.id.c_str(), st.status.c_str(), st.message.c_str());
                else
                    out::print("  %-16s %-10s (not found)\n", f.id.c_str(), st.status.c_str());
            } else {
                std::string line = i18n::fmt("  %-16s %-10s %s", f.id.c_str(), st.status.c_str(), st.path.c_str());
                if (!st.message.empty()) line += i18n::fmt("  [%s]", st.message.c_str());
                out::text(stdout, line + "\n");
            }
        }
        return 0;
    } catch (const std::exception& exc) {
        out::error("ERROR: %s\n", exc.what());
        return 1;
    }
}

int cmd_help(const std::vector<std::string>& args) {
    if (args.empty()) {
        out::error("ERROR: help <fmt_id> [--no-download] [-- <arguments>]\n");
        return 2;
    }
    std::string fmt_id = args[0];
    bool no_download = false;
    std::vector<std::string> tool_args;
    bool after_dashdash = false;
    for (size_t i = 1; i < args.size(); i++) {
        const std::string& a = args[i];
        if (!after_dashdash && a == "--") {
            after_dashdash = true;
        } else if (!after_dashdash && a == "--no-download") {
            no_download = true;
        } else {
            tool_args.push_back(a);
        }
    }
    if (tool_args.empty()) tool_args = {"--help"};

    try {
        auto fmts = config::load_all();
        const config::Format& fmt = config::load_one(fmts, fmt_id);
        tool::Status st = tool::ensure(fmt, !no_download, "[" + fmt_id + "] ");
        if (st.path.empty()) {
            out::text(stderr, i18n::fmt("Tool for '%s' is unavailable", fmt_id.c_str()));
            if (!st.message.empty()) out::text(stderr, i18n::fmt(" (%s)", st.message.c_str()));
            out::text(stderr, ".\n");
            return 1;
        }
        out::print("[%s] %s (%s)\n", fmt_id.c_str(), st.path.c_str(), st.status.c_str());
        if (!st.message.empty()) out::print("WARNING: %s\n", st.message.c_str());

        std::vector<std::string> run_args = {st.path};
        run_args.insert(run_args.end(), tool_args.begin(), tool_args.end());
        proc::Result r = proc::run(run_args, 60);
        if (!r.started) {
            out::print("Launch error: %s\n", r.error.empty() ? i18n::str("unknown error").c_str() : r.error.c_str());
            out::print("\n[exit code: %d]\n", r.exit_code);
            return 1;
        }
        std::string out = util::trim(r.output);
        if (out.empty()) {
            out::print("%s\n", i18n::str("(no output)").c_str());
        } else {
            out::print("%s\n", out.c_str());
        }
        out::print("\n[exit code: %d%s]\n", r.exit_code, r.timed_out ? i18n::str(", timed out").c_str() : "");
        return 0;
    } catch (const std::exception& exc) {
        out::error("ERROR: %s\n", exc.what());
        return 1;
    }
}

int cmd_stats() {
    auto items = stats::load();
    stats::print_summary(items);
    return 0;
}

int cmd_optimize(const std::vector<std::string>& args) {
    optimize::Options opts;
    bool no_more_opts = false;
    for (const auto& a : args) {
        if (!no_more_opts && a == "--") {
            no_more_opts = true;
        } else if (!no_more_opts && a == "--dry-run") {
            opts.dry_run = true;
        } else if (!no_more_opts && a == "--allow-lossy") {
            opts.allow_lossy = true;
        } else if (!no_more_opts && a == "--no-download") {
            opts.no_download = true;
        } else if (!no_more_opts && a == "--debug") {
            opts.debug = true;
        } else if (!no_more_opts && a == "--no-stats") {
            opts.no_stats = true;
        } else if (!no_more_opts && a == "--no-status") {
            opts.no_status = true;
        } else if (!no_more_opts && a.rfind("--jobs=", 0) == 0) {
            std::string jv = a.substr(7);
            opts.jobs = std::stod(jv);
            opts.jobs_float = jv.find('.') != std::string::npos;
        } else if (!no_more_opts && a == "--jobs") {
            out::error("ERROR: use --jobs=N\n");
            return 2;
        } else if (!no_more_opts && a.rfind("--formats=", 0) == 0) {
            opts.formats = util::split(a.substr(10), ',');
        } else if (!no_more_opts && a == "--formats") {
            out::error("ERROR: use --formats=fmt1,fmt2\n");
            return 2;
        } else if (!no_more_opts && a.rfind("--report=", 0) == 0) {
            opts.report_path = a.substr(9);
        } else if (!no_more_opts && a == "--report") {
            out::error("ERROR: use --report=<file|folder>\n");
            return 2;
        } else if (!no_more_opts && a.rfind("--verify=", 0) == 0) {
            std::string v = a.substr(9);
            if (v == "all") opts.verify = optimize::Verify::All;
            else if (v == "winner") opts.verify = optimize::Verify::Winner;
            else if (v == "none") opts.verify = optimize::Verify::None;
            else {
                out::error("ERROR: --verify must be one of: all|winner|none\n");
                return 2;
            }
        } else if (!no_more_opts && a == "--verify") {
            out::error("ERROR: use --verify=all|winner|none\n");
            return 2;
        } else if (!no_more_opts && a == "--ignore-errors") {
            opts.ignore_errors = true;
        } else if (!no_more_opts && a.rfind("--tmp=", 0) == 0) {
            opts.tmp_dir = a.substr(6);
        } else if (!no_more_opts && a == "--tmp") {
            out::error("ERROR: use --tmp=<path>\n");
            return 2;
        } else {
            opts.inputs.push_back(a);
        }
    }
    if (opts.inputs.empty()) {
        out::error("ERROR: optimize <file|folder> [...] [--jobs=N|M.F] [--formats=a,b] "
                   "[--report=<file|folder>] [--no-download] [--dry-run]\n");
        return 2;
    }
    g_atexit_tmp_dir = opts.tmp_dir;
    return optimize::run(opts);
}

int cmd_restore(const std::vector<std::string>& args) {
    optimize::RestoreOptions opts;
    bool no_more_opts = false;
    for (const auto& a : args) {
        if (!no_more_opts && a == "--") {
            no_more_opts = true;
        } else if (!no_more_opts && a == "--no-download") {
            opts.no_download = true;
        } else if (!no_more_opts && a == "--allow-lossy") {
            opts.allow_lossy = true;
        } else if (!no_more_opts && a == "--no-status") {
            opts.no_status = true;
        } else if (!no_more_opts && a.rfind("--jobs=", 0) == 0) {
            std::string jv = a.substr(7);
            opts.jobs = std::stod(jv);
            opts.jobs_float = jv.find('.') != std::string::npos;
        } else if (!no_more_opts && a == "--jobs") {
            out::error("ERROR: use --jobs=N\n");
            return 2;
        } else if (!no_more_opts && a.rfind("--to=", 0) == 0) {
            opts.to = a.substr(5);
        } else if (!no_more_opts && a == "--to") {
            out::error("ERROR: use --to=<format>\n");
            return 2;
        } else if (!no_more_opts && a.rfind("--variant=", 0) == 0) {
            opts.variant = a.substr(10);
        } else if (!no_more_opts && a == "--variant") {
            out::error("ERROR: use --variant=<id>\n");
            return 2;
        } else {
            opts.inputs.push_back(a);
        }
    }
    if (opts.inputs.empty()) {
        out::error("ERROR: restore <file|folder> [...] [--jobs=N|M.F] [--to=flac] [--variant=<id>] "
                   "[--no-download] [--no-status]\n");
        return 2;
    }
    g_atexit_tmp_dir.clear();
    return optimize::restore_run(opts);
}

}  // namespace

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
#else
    struct sigaction sa{};
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
#endif

    try {
        std::vector<std::string> args;
#ifdef _WIN32
        args = wide_argv();
#else
        for (int i = 0; i < argc; i++) args.push_back(argv[i]);
#endif

        std::string lang_flag;
        std::vector<std::string> filtered;
        for (size_t i = 0; i < args.size(); i++) {
            const std::string& a = args[i];
            if (a == "--lang" && i + 1 < args.size()) {
                lang_flag = args[++i];
            } else if (a.rfind("--lang=", 0) == 0) {
                lang_flag = a.substr(7);
            } else {
                filtered.push_back(a);
            }
        }
        args = std::move(filtered);

        i18n::init(lang_flag, config::load_settings_lang());

        if (args.size() < 2) {
            usage();
            return 1;
        }
        std::string cmd = args[1];
        std::vector<std::string> rest(args.begin() + 2, args.end());

        atexit(atexit_cleanup);

        if (cmd == "check-formats") return cmd_check_formats();
        if (cmd == "variants") return optimize::list_variants(rest);
        if (cmd == "tools") return cmd_tools(rest);
        if (cmd == "help") return cmd_help(rest);
        if (cmd == "stats") return cmd_stats();
        if (cmd == "optimize") return cmd_optimize(rest);
        if (cmd == "restore") return cmd_restore(rest);

        out::error("ERROR: unknown command '%s'\n\n", cmd.c_str());
        usage();
        return 2;
    } catch (const std::exception& exc) {
        out::error("ERROR: %s\n", exc.what());
        return 1;
    }
}
