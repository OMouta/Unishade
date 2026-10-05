#include "../src/log.h"

#include <windows.h>
#include <crtdbg.h>

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;

namespace
{
std::wstring directory;

bool Check(bool condition, const char* message)
{
    if (!condition)
        std::printf("Failed: %s\n", message);
    return condition;
}

std::string Read(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(file), {} };
}

__declspec(noinline) void Overflow(unsigned remaining)
{
    volatile char stack[4096]{};
    if (remaining)
        Overflow(remaining - 1);
    (void)stack[0];
}

int Child(const std::wstring& mode)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    InitLog();
    if (mode == L"rotate" || mode == L"crash-after-rotate")
    {
        const std::wstring message(8000, L'x');
        for (unsigned i = 0; i < 2300; ++i)
            Log(LogLevel::Info, L"Entry %u %ls", i, message.c_str());
        Log(LogLevel::Info, L"Last entry after rollover.");
    }
    if (mode == L"crash" || mode == L"crash-after-rotate")
    {
        Log(LogLevel::Info, L"About to crash.");
        *reinterpret_cast<volatile int*>(static_cast<uintptr_t>(1)) = 1;
        return 1;
    }
    if (mode == L"terminate")
    {
        std::thread worker([] {
            InitThreadLog();
            throw std::runtime_error("worker failure for logging test");
        });
        worker.join();
        return 1;
    }
    if (mode == L"abort")
        std::abort();
    if (mode == L"overflow")
        Overflow(1000000);
    if (mode == L"normal")
    {
        Log(LogLevel::Info, L"Long message: %ls END", std::wstring(6000, L'x').c_str());
        Log(LogLevel::Info, L"Unicode: Ol\u00e1 \u65e5\u672c\u8a9e.");
        Log(LogLevel::Info, L"Repeated message.");
        Log(LogLevel::Info, L"Repeated message.");
        Log(LogLevel::Info, L"Repeated message.");
        FlushLog();
        std::thread a([] { Log(LogLevel::Info, L"Thread A marker."); });
        std::thread b([] { Log(LogLevel::Info, L"Thread B marker."); });
        a.join();
        b.join();
        ClearNotices();
        Log(LogLevel::Info, L"File-only diagnostic.");
        if (!Check(Notices().empty(), "diagnostics should not add UI notices"))
            return 1;
        Log(LogLevel::Error, L"Test error.");
        if (!Check(Notices().size() == 1, "errors should add a notice"))
            return 1;
        LogStackTrace();
        std::ofstream(fs::path(directory) / "ReShade.log") << "00:00:01:000 [1] | INFO  | Ordinary ReShade entry\n"
                                                           "00:00:01:000 [1] | ERROR | shader.fx failed\n"
                                                           "shader.fx(3): unexpected token\n"
                                                           "00:00:02:000 [1] | WARN  | Missing texture\n"
                                                           "00:00:03:000 [1] | INFO  | Done\n";
        LogReShadeDiagnostics();
        LogReShadeDiagnostics();
    }
    Log(LogLevel::Info, L"Session ended: exit_code=0.");
    FlushLog();
    return 0;
}

bool RunChild(const fs::path& folder, const wchar_t* mode, bool crash = false)
{
    fs::create_directories(folder);
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    std::wstring command = L"\"" + std::wstring(executable) + L"\" " + mode + L" \"" + folder.wstring() + L"\"";
    STARTUPINFOW startup{ sizeof(startup) };
    PROCESS_INFORMATION process{};
    if (!Check(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE,
               "start isolated log fixture"))
        return false;
    const DWORD waited = WaitForSingleObject(process.hProcess, 10000);
    if (waited != WAIT_OBJECT_0)
        TerminateProcess(process.hProcess, 1);
    DWORD exit = 0;
    GetExitCodeProcess(process.hProcess, &exit);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return Check(waited == WAIT_OBJECT_0, "log fixture must exit without deadlocking") &&
           Check(crash ? exit != 0 : exit == 0, "log fixture exit status");
}
} // namespace

// Tests write into their own folder rather than beside the test executable.
std::wstring ExeDirectory()
{
    return directory;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc == 3)
    {
        directory = std::wstring(argv[2]) + L"\\";
        return Child(argv[1]);
    }
    wchar_t temporary[32768]{};
    GetTempPathW(static_cast<DWORD>(std::size(temporary)), temporary);
    const fs::path root = fs::path(temporary) / (L"Unishade log tests " + std::to_wstring(GetCurrentProcessId()));
    bool ok = true;
    const fs::path normal = root / "normal";
    fs::create_directories(normal);
    std::ofstream(normal / "Unishade.log") << "previous session marker\n";
    ok &= RunChild(normal, L"normal");
    const std::string log = Read(normal / "Unishade.log");
    ok &= Check(Read(normal / "Unishade.old.log").find("previous session marker") != std::string::npos, "preserve previous session");
    ok &= Check(log.find(std::string(6000, 'x') + " END") != std::string::npos, "preserve long diagnostics");
    ok &= Check(log.find("Ol\xC3\xA1 \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E") != std::string::npos, "write UTF-8");
    ok &= Check(log.find("(repeated 2 times)") != std::string::npos, "flush repeat counts");
    ok &= Check(log.find("[tid=") != std::string::npos && log.find("Thread A marker") != std::string::npos &&
                log.find("Thread B marker") != std::string::npos, "record worker thread diagnostics");
    ok &= Check(log.find("Call stack:") != std::string::npos && log.find("log_tests.exe+0x") != std::string::npos, "record module offsets");
    ok &= Check(log.find("shader.fx failed") != std::string::npos && log.find("unexpected token") != std::string::npos &&
                log.find("Missing texture") != std::string::npos && log.find("Ordinary ReShade entry") == std::string::npos,
                "copy ReShade warnings and multiline compiler errors");
    ok &= Check(log.find("shader.fx failed") == log.rfind("shader.fx failed"), "copy each ReShade diagnostic once");

    const fs::path blocked = root / "blocked rotation";
    fs::create_directories(blocked / "Unishade.old.log");
    std::ofstream(blocked / "Unishade.log") << "retained when rotation fails\n";
    ok &= RunChild(blocked, L"append");
    ok &= Check(Read(blocked / "Unishade.log").find("retained when rotation fails") != std::string::npos, "rotation failure must not erase the log");

    const fs::path rotated = root / "rotate";
    ok &= RunChild(rotated, L"rotate");
    ok &= Check(fs::exists(rotated / "Unishade.previous.log") && fs::file_size(rotated / "Unishade.previous.log") <= 16 * 1024 * 1024,
                "bound rollover file size");
    ok &= Check(Read(rotated / "Unishade.log").find("Last entry after rollover") != std::string::npos, "keep logging after rollover");

    for (const wchar_t* mode : { L"crash", L"crash-after-rotate", L"terminate", L"abort", L"overflow" })
    {
        const fs::path folder = root / mode;
        ok &= RunChild(folder, mode, true);
        const std::string fatal = Read(folder / "Unishade.log");
        ok &= Check(fatal.find("FATAL:") != std::string::npos && fatal.find("log_tests.exe+0x") != std::string::npos,
                    "crashes must leave fatal details and stack offsets");
        if (std::wstring(mode) == L"terminate")
            ok &= Check(fatal.find("worker failure for logging test") != std::string::npos, "record uncaught worker exception");
        if (std::wstring(mode) == L"crash")
            ok &= Check(fatal.find("0xC0000005") != std::string::npos && fatal.find("Exception parameter 1: 0x1") != std::string::npos,
                        "record access violation and fault address");
    }
    if (ok)
        fs::remove_all(root);
    else
        std::printf("Fixture logs kept in %ls\n", root.c_str());
    return ok ? 0 : 1;
}
