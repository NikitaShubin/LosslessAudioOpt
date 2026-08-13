#include "proc.h"

#include <windows.h>

#include <thread>

#include "util.h"
#include "i18n.h"

namespace proc {

namespace {

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

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::wstring wcwd;
    LPCWSTR lpCwd = nullptr;
    if (!cwd.empty()) {
        wcwd = util::u2w(cwd);
        lpCwd = wcwd.c_str();
    }
    BOOL ok = CreateProcessW(nullptr, wcmdline.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, lpCwd, &si, &pi);
    CloseHandle(hWrite);
    if (!ok) {
        DWORD err = GetLastError();
        res.error = i18n::fmt("CreateProcess: error code %lu", (unsigned long)err);
        CloseHandle(hRead);
        return res;
    }

    res.started = true;

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

    DWORD timeout_ms = timeout_sec > 0 ? (DWORD)timeout_sec * 1000 : INFINITE;
    DWORD wait = WaitForSingleObject(pi.hProcess, timeout_ms);
    if (wait == WAIT_TIMEOUT) {
        res.timed_out = true;
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 10000);
    }

    DWORD code = 0;
    if (GetExitCodeProcess(pi.hProcess, &code)) res.exit_code = (int)code;

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hRead);
    reader.join();
    res.output = std::move(output);
    return res;
}

}  // namespace proc
