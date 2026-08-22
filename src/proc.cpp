#include "proc.h"

#include <windows.h>

#include <atomic>
#include <thread>

#include "util.h"
#include "i18n.h"

namespace proc {

namespace {

std::atomic<bool> g_cancel{false};
std::atomic<bool> g_abort{false};

// Процессорное время дочерних процессов текущего потока (100нс-тики). Каждый
// run() накапливает CPU своих детей сюда — это позволяет атрибутировать затраты
// на задачу (см. proc::child_cpu_ms()).
thread_local uint64_t t_child_cpu_ticks = 0;

uint64_t ft_ticks(const FILETIME& ft) {
    return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

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

uint64_t child_cpu_ms() { return t_child_cpu_ticks / 10000; }

uint64_t thread_cpu_ms() {
    FILETIME ft_creation{}, ft_exit{}, ft_kernel{}, ft_user{};
    if (GetThreadTimes(GetCurrentThread(), &ft_creation, &ft_exit, &ft_kernel, &ft_user))
        return (ft_ticks(ft_kernel) + ft_ticks(ft_user)) / 10000;
    return 0;
}

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

    // Дочерний процесс запускаем обычным спавном (bInheritHandles=TRUE без
    // HANDLE_LIST): на реальном Windows ограниченное наследование через
    // PROC_THREAD_ATTRIBUTE_HANDLE_LIST оказалось нестабильным — CreateProcess
    // падал с ERROR_INVALID_PARAMETER, а NULL-указатель stdin ломал вывод
    // (кодек стартовал, но не писал в stdout). Утечка файловых дескрипторов,
    // из-за которой кодеки держали транзитные .llao-tmp и срывали замену,
    // закрыта на уровне открытия файлов: util::copy_file/read_file/write_file
    // открывают НЕнаследуемые дескрипторы с FILE_SHARE_DELETE, поэтому кодеки
    // не могут унаследовать наши файловые хендлы, какими бы они ни были.
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    // Кодеки — главные потребители CPU; низкий приоритет класса процесса
    // (BELOW_NORMAL) оставляет ресурсы потокам ввода/отрисовки интерфейса,
    // иначе статусбар заметно тормозит под нагрузкой.
    DWORD create_flags = CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS;

    // Job Object с JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: если наш процесс умирает
    // аварийно (kill извне, авария), ОС закроет все хендлы job и дочерний процесс
    // будет завершён принудительно — иначе кодеры остались бы работать после нас.
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

    // Чтение вывода в отдельном потоке, чтобы не блокироваться на заполненном pipe.
    std::string output;
    std::thread reader([&]() {
        char buf[8192];
        DWORD n = 0;
        size_t cap = 8u << 20;  // не храним больше 8 МБ
        while (ReadFile(hRead, buf, sizeof(buf), &n, nullptr) && n > 0) {
            if (output.size() < cap) {
                size_t take = std::min<size_t>(n, cap - output.size());
                output.append(buf, take);
            }
        }
    });

    // Ожидание с опросом: пока процесс работает, каждые ~200 мс проверяем
    // глобальный флаг отмены (Ctrl+C) и мониторинг прогресса. При отмене,
    // истечении таймаута или stall (файл не растёт + CPU ≈ 0) процесс
    // завершается принудительно.
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

    // Stall detection: проверяем каждые ~5 сек. Если файл не ростёт и
    // CPU процесса ≈ 0 stall_timeout_sec подряд → kill.
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
    DWORD stall_accum_ms = 0;  // суммарное время «stall» (файл не растёт + CPU ≈ 0)

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

        // Stall detection каждые ~5 сек.
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
                // Файл растёт — сбрасываем stall-счётчик.
                prev_file_size = cur_file_size;
                stall_accum_ms = 0;
            } else if (cur_file_size == 0 && !cpu_active) {
                // Файл не появился и CPU ≈ 0 → потенциальный stall.
                stall_accum_ms += kMonitorPollMs;
            } else if (cur_file_size > 0 && !cpu_active) {
                // Файл есть, не растёт, CPU ≈ 0 → stall.
                stall_accum_ms += kMonitorPollMs;
            } else {
                // CPU активен (или файл растёт) — терпим.
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

    // Процессорное время (kernel+user) — объективная мера работы, не зависящая
    // от планировщика/приоритета окна. Замер после завершения процесса.
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

}  // namespace proc
