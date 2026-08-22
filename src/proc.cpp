#include "proc.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/resource.h>
#endif

#include <atomic>
#include <thread>

#include "util.h"
#include "i18n.h"

namespace proc {

namespace {

std::atomic<bool> g_cancel{false};
std::atomic<bool> g_abort{false};

thread_local uint64_t t_child_cpu_ticks = 0;

#ifdef _WIN32
uint64_t ft_ticks(const FILETIME& ft) {
    return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}
#else
uint64_t timeval_ms(const struct timeval& tv) {
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}
#endif

std::string quote_arg(const std::string& a) {
    if (a.find_first_of(" \t\"") == std::string::npos) return a;
    std::string r = "\"";
    for (char c : a) {
        if (c == '"') r += "\\\"";
        else r += c;
    }
    r += "\"";
    return r;
}

}  // namespace

void cancel() { g_cancel.store(true, std::memory_order_relaxed); }
bool cancelled() { return g_cancel.load(std::memory_order_relaxed); }

void abort_all() { g_abort.store(true, std::memory_order_relaxed); }
bool aborted() { return g_abort.load(std::memory_order_relaxed); }

uint64_t child_cpu_ms() {
#ifdef _WIN32
    return t_child_cpu_ticks / 10000;
#else
    return t_child_cpu_ticks;
#endif
}

uint64_t thread_cpu_ms() {
#ifdef _WIN32
    FILETIME ft_creation{}, ft_exit{}, ft_kernel{}, ft_user{};
    if (GetThreadTimes(GetCurrentThread(), &ft_creation, &ft_exit, &ft_kernel, &ft_user))
        return (ft_ticks(ft_kernel) + ft_ticks(ft_user)) / 10000;
    return 0;
#else
    struct rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) == 0)
        return timeval_ms(ru.ru_utime) + timeval_ms(ru.ru_stime);
    return 0;
#endif
}

#ifdef _WIN32
Result run(const std::vector<std::string>& args, int timeout_sec, const std::string& cwd,
           const OutputMonitor& monitor) {
    Result res;
    if (args.empty()) return res;

    std::string cmdline;
    for (size_t i = 0; i < args.size(); i++) {
        if (i) cmdline += " ";
        cmdline += quote_arg(args[i]);
    }
    std::wstring wcmdline = util::u2w(cmdline);

    HANDLE hRead = nullptr, hWrite = nullptr;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        return res;
    }
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    DWORD create_flags = CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS;

    HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
    if (hJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli,
                                     sizeof(jeli))) {
            CloseHandle(hJob);
            hJob = nullptr;
        }
    }

    PROCESS_INFORMATION pi{};
    std::wstring wcwd;
    LPCWSTR lpCwd = nullptr;
    if (!cwd.empty()) {
        wcwd = util::u2w(cwd);
        lpCwd = wcwd.c_str();
    }
    BOOL ok = CreateProcessW(nullptr, wcmdline.data(), nullptr, nullptr, TRUE,
                             create_flags, nullptr, lpCwd, &si, &pi);
    if (!ok) {
        DWORD err = GetLastError();
        res.error = i18n::fmt("CreateProcess: error code %lu", (unsigned long)err);
        CloseHandle(hWrite);
        CloseHandle(hRead);
        if (hJob) CloseHandle(hJob);
        return res;
    }
    if (hJob) AssignProcessToJobObject(hJob, pi.hProcess);

    res.started = true;
    CloseHandle(hWrite);

    std::string output;
    std::thread reader([&]() {
        char buf[8192];
        DWORD n = 0;
        size_t cap = 8u << 20;
        while (ReadFile(hRead, buf, sizeof(buf), &n, nullptr) && n > 0) {
            if (output.size() < cap) {
                size_t take = std::min<size_t>(n, cap - output.size());
                output.append(buf, take);
            }
        }
    });

    DWORD effective_timeout_ms = INFINITE;
    if (timeout_sec > 0 && monitor.hard_timeout_sec > 0)
        effective_timeout_ms = (DWORD)std::min(timeout_sec, monitor.hard_timeout_sec) * 1000;
    else if (timeout_sec > 0)
        effective_timeout_ms = (DWORD)timeout_sec * 1000;
    else if (monitor.hard_timeout_sec > 0)
        effective_timeout_ms = (DWORD)monitor.hard_timeout_sec * 1000;

    constexpr DWORD kPollMs = 200;
    DWORD waited = 0;
    bool timed_out = false;
    bool stalled = false;

    std::wstring wmonitor_path;
    if (!monitor.path.empty()) wmonitor_path = util::u2w(monitor.path);
    auto get_process_cpu_ms = [&](HANDLE hProc) -> uint64_t {
        FILETIME ft_creation{}, ft_exit{}, ft_kernel{}, ft_user{};
        if (GetProcessTimes(hProc, &ft_creation, &ft_exit, &ft_kernel, &ft_user))
            return (ft_ticks(ft_kernel) + ft_ticks(ft_user)) / 10000;
        return 0;
    };

    constexpr DWORD kMonitorPollMs = 5000;
    DWORD monitor_waited = 0;
    uint64_t prev_file_size = 0;
    uint64_t prev_cpu_ms = get_process_cpu_ms(pi.hProcess);
    DWORD stall_accum_ms = 0;

    for (;;) {
        DWORD wait;
        DWORD poll = std::min(kPollMs, effective_timeout_ms != INFINITE
                                           ? effective_timeout_ms - waited
                                           : kPollMs);
        wait = WaitForSingleObject(pi.hProcess, poll);
        waited += kPollMs;

        if (wait == WAIT_OBJECT_0) break;

        if (effective_timeout_ms != INFINITE && waited >= effective_timeout_ms) {
            timed_out = true;
            break;
        }
        if (cancelled()) { res.cancelled = true; break; }
        if (aborted())   { res.aborted = true;   break; }

        monitor_waited += kPollMs;
        if (monitor_waited >= kMonitorPollMs && !wmonitor_path.empty()) {
            monitor_waited = 0;

            uint64_t cur_file_size = 0;
            DWORD attrs = GetFileAttributesW(wmonitor_path.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                WIN32_FILE_ATTRIBUTE_DATA fad{};
                if (GetFileAttributesExW(wmonitor_path.c_str(),
                                        GetFileExInfoStandard, &fad)) {
                    cur_file_size =
                        ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
                }
            }

            uint64_t cur_cpu_ms = get_process_cpu_ms(pi.hProcess);
            bool cpu_active = (cur_cpu_ms > prev_cpu_ms);
            prev_cpu_ms = cur_cpu_ms;

            if (cur_file_size > prev_file_size) {
                prev_file_size = cur_file_size;
                stall_accum_ms = 0;
            } else if (cur_file_size == 0 && !cpu_active) {
                stall_accum_ms += kMonitorPollMs;
            } else if (cur_file_size > 0 && !cpu_active) {
                stall_accum_ms += kMonitorPollMs;
            } else {
                stall_accum_ms = 0;
            }

            if (stall_accum_ms >= (DWORD)monitor.stall_timeout_sec * 1000) {
                stalled = true;
                break;
            }
        }
    }
    if (timed_out || stalled || res.cancelled || res.aborted) {
        if (timed_out) res.timed_out = true;
        if (stalled)   res.stalled   = true;
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 10000);
    }

    DWORD code = 0;
    if (GetExitCodeProcess(pi.hProcess, &code)) res.exit_code = (int)code;

    FILETIME ft_creation{}, ft_exit{}, ft_kernel{}, ft_user{};
    if (GetProcessTimes(pi.hProcess, &ft_creation, &ft_exit, &ft_kernel, &ft_user)) {
        uint64_t ticks = ft_ticks(ft_kernel) + ft_ticks(ft_user);
        res.cpu_ms = ticks / 10000;
        t_child_cpu_ticks += ticks;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    reader.join();
    CloseHandle(hRead);
    if (hJob) CloseHandle(hJob);
    res.output = util::normalize_output(output);
    return res;
}

#else  // POSIX

Result run(const std::vector<std::string>& args, int timeout_sec, const std::string& cwd,
           const OutputMonitor& monitor) {
    Result res;
    if (args.empty()) return res;

    // На Linux: если первый аргумент — .exe файл, оборачиваем через wine.
    std::vector<std::string> final_args = args;
    bool need_wine = false;
    if (!final_args.empty() && util::ends_with(util::to_lower(final_args[0]), ".exe")) {
        need_wine = true;
    }
    if (need_wine) {
        std::string wine = util::find_in_path("wine");
        if (wine.empty()) {
            res.error = "wine not found in PATH (needed to run " + final_args[0] + ")";
            return res;
        }
        std::vector<std::string> wrapped;
        wrapped.push_back(wine);
        // Конвертируем Unix-путы в формат Z:\... для wine:
        // некоторые Windows-утилиты (wavpack) интерпретируют '/' как префикс опций.
        for (size_t i = 0; i < final_args.size(); i++) {
            const std::string& a = final_args[i];
            if (a.size() > 1 && a[0] == '/' && a[1] != '/') {
                wrapped.push_back("Z:" + a);
            } else {
                wrapped.push_back(a);
            }
        }
        final_args = std::move(wrapped);
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) return res;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return res;
    }

    if (pid == 0) {
        // Дочерний процесс.
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        if (!cwd.empty()) {
            if (chdir(cwd.c_str()) != 0) _exit(127);
        }

        // Преобразуем аргументы в C-массив.
        std::vector<const char*> cargs;
        for (const auto& a : final_args) cargs.push_back(a.c_str());
        cargs.push_back(nullptr);

        execvp(cargs[0], const_cast<char* const*>(cargs.data()));
        _exit(127);
    }

    // Родительский процесс.
    close(pipefd[1]);
    res.started = true;

    std::string output;
    std::thread reader([&]() {
        char buf[8192];
        ssize_t n = 0;
        size_t cap = 8u << 20;
        while ((n = ::read(pipefd[0], buf, sizeof(buf))) > 0) {
            if (output.size() < cap) {
                size_t take = std::min<size_t>((size_t)n, cap - output.size());
                output.append(buf, take);
            }
        }
    });

    auto effective_timeout = timeout_sec;
    if (effective_timeout <= 0 && monitor.hard_timeout_sec > 0)
        effective_timeout = monitor.hard_timeout_sec;
    else if (timeout_sec > 0 && monitor.hard_timeout_sec > 0)
        effective_timeout = std::min(timeout_sec, monitor.hard_timeout_sec);

    constexpr int kPollMs = 200;
    int waited_ms = 0;
    bool timed_out = false;
    bool stalled = false;

    auto get_child_cpu_ms = [&](pid_t p) -> uint64_t {
        struct rusage ru{};
        if (getrusage(RUSAGE_CHILDREN, &ru) == 0)
            return timeval_ms(ru.ru_utime) + timeval_ms(ru.ru_stime);
        return 0;
    };

    int monitor_waited_ms = 0;
    uint64_t prev_file_size = 0;
    uint64_t prev_cpu_ms = get_child_cpu_ms(pid);
    int stall_accum_ms = 0;

    for (;;) {
        int status = 0;
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            if (WIFEXITED(status)) res.exit_code = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) res.exit_code = 128 + WTERMSIG(status);
            break;
        }
        if (w < 0) break;

        std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
        waited_ms += kPollMs;

        if (effective_timeout > 0 && waited_ms >= effective_timeout * 1000) {
            timed_out = true;
            break;
        }
        if (cancelled()) { res.cancelled = true; break; }
        if (aborted()) { res.aborted = true; break; }

        monitor_waited_ms += kPollMs;
        if (monitor_waited_ms >= 5000 && !monitor.path.empty()) {
            monitor_waited_ms = 0;

            uint64_t cur_file_size = 0;
            struct stat st{};
            if (stat(monitor.path.c_str(), &st) == 0 && S_ISREG(st.st_mode))
                cur_file_size = (uint64_t)st.st_size;

            uint64_t cur_cpu_ms = get_child_cpu_ms(pid);
            bool cpu_active = (cur_cpu_ms > prev_cpu_ms);
            prev_cpu_ms = cur_cpu_ms;

            if (cur_file_size > prev_file_size) {
                prev_file_size = cur_file_size;
                stall_accum_ms = 0;
            } else if (cur_file_size == 0 && !cpu_active) {
                stall_accum_ms += 5000;
            } else if (cur_file_size > 0 && !cpu_active) {
                stall_accum_ms += 5000;
            } else {
                stall_accum_ms = 0;
            }

            if (stall_accum_ms >= monitor.stall_timeout_sec * 1000) {
                stalled = true;
                break;
            }
        }
    }

    if (timed_out || stalled || res.cancelled || res.aborted) {
        if (timed_out) res.timed_out = true;
        if (stalled)   res.stalled   = true;
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 10000);
    }

    reader.join();
    close(pipefd[0]);

    // CPU-time дочерних процессов (.wall-clock: реальный замер после завершения).
    {
        struct rusage ru{};
        if (getrusage(RUSAGE_CHILDREN, &ru) == 0) {
            uint64_t ms = timeval_ms(ru.ru_utime) + timeval_ms(ru.ru_stime);
            res.cpu_ms = ms;
            t_child_cpu_ticks = ms;
        }
    }

    res.output = util::normalize_output(output);
    return res;
}
#endif

}  // namespace proc
