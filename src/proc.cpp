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

Result run(const std::vector<std::string>& args, int timeout_sec, const std::string& cwd) {
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
    // глобальный флаг отмены (Ctrl+C). При отмене или истечении таймаута
    // процесс завершается принудительно.
    DWORD timeout_ms = timeout_sec > 0 ? (DWORD)timeout_sec * 1000 : INFINITE;
    constexpr DWORD kPollMs = 200;
    DWORD waited = 0;
    bool timed_out = false;
    for (;;) {
        DWORD wait;
        if (timeout_ms == INFINITE) {
            wait = WaitForSingleObject(pi.hProcess, kPollMs);
        } else {
            DWORD remain = timeout_ms > waited ? timeout_ms - waited : 0;
            wait = WaitForSingleObject(pi.hProcess, std::min(kPollMs, remain));
            waited += kPollMs;
            if (wait != WAIT_OBJECT_0 && waited >= timeout_ms) {
                timed_out = true;
                break;
            }
        }
        if (wait == WAIT_OBJECT_0) break;
        if (cancelled()) {
            res.cancelled = true;
            break;
        }
        if (aborted()) {
            res.aborted = true;
            break;
        }
    }
    if (timed_out || res.cancelled || res.aborted) {
        if (timed_out) res.timed_out = true;
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
    res.output = std::move(output);
    return res;
}

}  // namespace proc
