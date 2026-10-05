#include "log.h"
#include "config.h"
#include "text.h"
#include "web_address.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <csignal>
#include <exception>
#include <iterator>
#include <mutex>

namespace
{
constexpr size_t kMaxNotices = 40;
constexpr unsigned long long kMaxLogSize = 16ull * 1024 * 1024;

// Never destroyed, since other threads, such as the update check, may still log while the host exits.
struct Shared
{
    std::mutex mutex;
    std::vector<Notice> notices;
    unsigned long long size = 0;
    bool writeFailed = false;
    // The last line written, without its time, and how often it came again right after.
    LogLevel lastLevel = LogLevel::Info;
    DWORD lastThread = 0;
    std::string lastText;
    unsigned repeats = 0;
    std::string repeatStamp;
};
Shared& shared = *new Shared;
HANDLE file = INVALID_HANDLE_VALUE;
std::wstring path;
// Crash reporting does not allocate or take the logger's mutex.
wchar_t crashPath[32768]{};
std::atomic_flag crashing = ATOMIC_FLAG_INIT;
std::atomic<unsigned> noticeVersion = 0;
const ULONGLONG startedAt = GetTickCount64();

void WriteFailure(DWORD error)
{
    if (shared.writeFailed)
        return;
    shared.writeFailed = true;
    wchar_t message[256];
    swprintf_s(message, L"Could not write Unishade.log (Windows error %lu).", error);
    OutputDebugStringW(message);
    if (shared.notices.size() == kMaxNotices)
        shared.notices.erase(shared.notices.begin());
    shared.notices.push_back({ LogLevel::Warning, message });
    ++noticeVersion;
}

// Called with the mutex held.
void WriteToFile(std::string text)
{
    if (file == INVALID_HANDLE_VALUE || crashing.test())
        return;
    if (shared.size + text.size() > kMaxLogSize)
    {
        CloseHandle(file);
        file = INVALID_HANDLE_VALUE;
        if (!MoveFileExW(path.c_str(), (ExeDirectory() + L"Unishade.previous.log").c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            const DWORD error = GetLastError();
            file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            WriteFailure(error);
            return;
        }
        file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        shared.size = 0;
        text = "Unishade " UNISHADE_VERSION " log continued. Earlier entries are in Unishade.previous.log.\r\n" + text;
        if (file == INVALID_HANDLE_VALUE)
        {
            WriteFailure(GetLastError());
            return;
        }
    }
    DWORD written = 0;
    const BOOL succeeded = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    if (!succeeded || written != text.size())
        WriteFailure(succeeded ? ERROR_WRITE_FAULT : GetLastError());
    shared.size += written;
}

// Called with the mutex held.
void WriteRepeats()
{
    if (shared.repeats)
        WriteToFile(shared.repeatStamp + "(repeated " + std::to_string(shared.repeats) + " times)\r\n");
    shared.repeats = 0;
}

void Write(LogLevel level, bool notice, const wchar_t* format, va_list args)
{
    va_list countArgs;
    va_copy(countArgs, args);
    const int length = _vscwprintf(format, countArgs);
    va_end(countArgs);
    if (length < 0)
        return;
    constexpr size_t kMaxMessage = 65536;
    std::wstring buffer(std::min(static_cast<size_t>(length), kMaxMessage) + 1, L'\0');
    _vsnwprintf_s(buffer.data(), buffer.size(), _TRUNCATE, format, args);
    buffer.resize(wcslen(buffer.c_str()));
    if (static_cast<size_t>(length) > kMaxMessage)
        buffer += L" [message truncated at 65536 characters]";

    static constexpr const char* kNames[] = { "info", "ok", "warning", "error" };
    SYSTEMTIME time{};
    GetLocalTime(&time);
    const DWORD thread = GetCurrentThreadId();
    char stamp[96];
    snprintf(stamp, sizeof(stamp), "%04u-%02u-%02u %02u:%02u:%02u.%03u  %-7s  [tid=%lu +%llums]  ", time.wYear, time.wMonth, time.wDay, time.wHour,
             time.wMinute, time.wSecond, time.wMilliseconds, kNames[static_cast<int>(level)], thread, GetTickCount64() - startedAt);
    std::string text = Utf8(buffer);

    std::lock_guard lock(shared.mutex);
    // A line that keeps coming, such as from a retry, is written once and then counted.
    if (level == shared.lastLevel && thread == shared.lastThread && text == shared.lastText)
    {
        ++shared.repeats;
        shared.repeatStamp = stamp;
    }
    else
    {
        WriteRepeats();
        WriteToFile(stamp + text + "\r\n");
        shared.lastLevel = level;
        shared.lastThread = thread;
        shared.lastText = std::move(text);
    }
    if (level == LogLevel::Error && file != INVALID_HANDLE_VALUE)
        FlushFileBuffers(file);
    if (!notice && level != LogLevel::Warning && level != LogLevel::Error)
        return;
    auto& notices = shared.notices;
    // Retries, such as a capture that keeps failing, would otherwise fill the list with one message.
    if (std::any_of(notices.begin(), notices.end(), [&](const Notice& existing) { return existing.text == buffer; }))
        return;
    if (notices.size() == kMaxNotices)
        notices.erase(notices.begin());
    notices.push_back({ level, buffer });
    ++noticeVersion;
}

HANDLE OpenEmergencyLog()
{
    return CreateFileW(crashPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void EmergencyWrite(HANDLE out, const char* format, ...)
{
    char text[4096];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    DWORD written = 0;
    if (out != INVALID_HANDLE_VALUE)
        WriteFile(out, text, static_cast<DWORD>(strlen(text)), &written, nullptr);
    else
        OutputDebugStringA(text);
}

void StackAddress(HANDLE out, unsigned index, const void* address)
{
    void* base = nullptr;
    RtlPcToFileHeader(const_cast<void*>(address), &base);
    wchar_t module[1024]{};
    char name[3072]{};
    if (base && GetModuleFileNameW(static_cast<HMODULE>(base), module, static_cast<DWORD>(std::size(module))))
        WideCharToMultiByte(CP_UTF8, 0, module, -1, name, static_cast<int>(std::size(name)), nullptr, nullptr);
    EmergencyWrite(out, "  #%02u %p %s+0x%llX\r\n", index, address, name[0] ? name : "unknown",
                   static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(base)));
}

void CrashStack(HANDLE out, CONTEXT context)
{
    // A damaged stack must not prevent the exception details already written from reaching disk.
    __try
    {
#if defined(_M_X64)
        EmergencyWrite(out, "Registers: RIP=%llX RSP=%llX RBP=%llX RAX=%llX RBX=%llX RCX=%llX RDX=%llX\r\n",
                       context.Rip, context.Rsp, context.Rbp, context.Rax, context.Rbx, context.Rcx, context.Rdx);
        for (unsigned i = 0; i < 64 && context.Rip; ++i)
        {
            StackAddress(out, i, reinterpret_cast<void*>(context.Rip));
            const DWORD64 previousStack = context.Rsp;
            DWORD64 base = 0;
            if (const auto entry = RtlLookupFunctionEntry(context.Rip, &base, nullptr))
            {
                void* data = nullptr;
                DWORD64 frame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, context.Rip, entry, &context, &data, &frame, nullptr);
            }
            else
            {
                context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
                context.Rsp += sizeof(DWORD64);
            }
            if (context.Rsp <= previousStack)
                break;
        }
#else
        EmergencyWrite(out, "Stack unwinding is unavailable on this architecture.\r\n");
#endif
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        EmergencyWrite(out, "Stack unwinding stopped because the stack is unreadable.\r\n");
    }
}

LONG WINAPI OnCrash(EXCEPTION_POINTERS* exception)
{
    if (crashing.test_and_set())
        return EXCEPTION_CONTINUE_SEARCH;
    const HANDLE out = OpenEmergencyLog();
    const EXCEPTION_RECORD& record = *exception->ExceptionRecord;
    EmergencyWrite(out, "\r\nFATAL: unhandled exception 0x%08lX at %p, pid=%lu tid=%lu tick=%llu\r\n",
                   record.ExceptionCode, record.ExceptionAddress, GetCurrentProcessId(), GetCurrentThreadId(), GetTickCount64());
    for (DWORD i = 0; i < record.NumberParameters && i < EXCEPTION_MAXIMUM_PARAMETERS; ++i)
        EmergencyWrite(out, "  Exception parameter %lu: 0x%llX\r\n", i, static_cast<unsigned long long>(record.ExceptionInformation[i]));
    CrashStack(out, *exception->ContextRecord);
    if (out != INVALID_HANDLE_VALUE)
    {
        FlushFileBuffers(out);
        CloseHandle(out);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void OnAbort(int)
{
    if (crashing.test_and_set())
        return;
    const HANDLE out = OpenEmergencyLog();
    EmergencyWrite(out, "\r\nFATAL: abort called, pid=%lu tid=%lu tick=%llu\r\n", GetCurrentProcessId(), GetCurrentThreadId(), GetTickCount64());
    CONTEXT context{};
    RtlCaptureContext(&context);
    CrashStack(out, context);
    if (out != INVALID_HANDLE_VALUE)
    {
        FlushFileBuffers(out);
        CloseHandle(out);
    }
}

void OnTerminate()
{
    if (crashing.test_and_set())
        ExitProcess(1);
    const HANDLE out = OpenEmergencyLog();
    EmergencyWrite(out, "\r\nFATAL: std::terminate, pid=%lu tid=%lu tick=%llu\r\n", GetCurrentProcessId(), GetCurrentThreadId(), GetTickCount64());
    try
    {
        if (const auto error = std::current_exception())
            std::rethrow_exception(error);
    }
    catch (const std::exception& e)
    {
        EmergencyWrite(out, "Unhandled C++ exception: %s\r\n", e.what());
    }
    catch (...)
    {
        EmergencyWrite(out, "Unhandled exception of unknown type.\r\n");
    }
    CONTEXT context{};
    RtlCaptureContext(&context);
    CrashStack(out, context);
    if (out != INVALID_HANDLE_VALUE)
    {
        FlushFileBuffers(out);
        CloseHandle(out);
    }
    ExitProcess(1);
}
} // namespace

void InitLog()
{
    path = ExeDirectory() + L"Unishade.log";
    wcscpy_s(crashPath, path.c_str());
    const bool rotated = MoveFileExW(path.c_str(), (ExeDirectory() + L"Unishade.old.log").c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
    const DWORD rotationError = GetLastError();
    file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, rotated || rotationError == ERROR_FILE_NOT_FOUND ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    const DWORD error = GetLastError();
    LARGE_INTEGER existing{};
    if (file != INVALID_HANDLE_VALUE && GetFileSizeEx(file, &existing))
        shared.size = existing.QuadPart;
    SetUnhandledExceptionFilter(OnCrash);
    std::signal(SIGABRT, OnAbort);
    InitThreadLog();

    // GetVersionEx reports Windows 8 to programs without a compatibility manifest.
    RTL_OSVERSIONINFOW version{ sizeof(version) };
    using RtlGetVersion = LONG(WINAPI*)(RTL_OSVERSIONINFOW*);
    if (auto get = reinterpret_cast<RtlGetVersion>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))
        get(&version);
    char header[512];
    snprintf(header, sizeof(header), "Unishade %s on Windows %lu.%lu.%lu\r\n", UNISHADE_VERSION,
             version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber);
    {
        std::lock_guard lock(shared.mutex);
        WriteToFile(header + std::string("Folder: ") + Utf8(ExeDirectory()) + "\r\n\r\n");
    }

    if (file == INVALID_HANDLE_VALUE)
        Log(LogLevel::Warning, L"Could not create %ls (error %lu), so nothing is logged this time.", path.c_str(), error);
    else if (!rotated && rotationError != ERROR_FILE_NOT_FOUND)
        Log(LogLevel::Warning, L"Could not preserve the previous log (error %lu). Appending to it instead.", rotationError);
    Log(LogLevel::Info, L"Session started: pid=%lu, pointer_bits=%zu, processors=%lu, build=" __DATE__ " " __TIME__ ".", GetCurrentProcessId(), sizeof(void*) * 8,
        GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    MEMORYSTATUSEX memory{ sizeof(memory) };
    if (GlobalMemoryStatusEx(&memory))
        Log(LogLevel::Info, L"Memory: physical=%llu MB, available=%llu MB, load=%lu%%.", memory.ullTotalPhys / (1024 * 1024),
            memory.ullAvailPhys / (1024 * 1024), memory.dwMemoryLoad);
    TIME_ZONE_INFORMATION zone{};
    GetTimeZoneInformation(&zone);
    Log(LogLevel::Info, L"Local time zone: %ls, base UTC bias=%ld minutes.", zone.StandardName, zone.Bias);
    const HANDLE modules = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (modules != INVALID_HANDLE_VALUE)
    {
        MODULEENTRY32W module{ sizeof(module) };
        if (Module32FirstW(modules, &module))
            do
            {
                Log(LogLevel::Info, L"Module: %ls, base=%p, size=%lu.", module.szExePath, module.modBaseAddr, module.modBaseSize);
            } while (Module32NextW(modules, &module));
        CloseHandle(modules);
    }
}

void InitThreadLog()
{
    thread_local bool initialized = false;
    if (initialized)
        return;
    initialized = true;
    std::set_terminate(OnTerminate);
    ULONG guarantee = 32768;
    SetThreadStackGuarantee(&guarantee);
}

void FlushLog()
{
    std::lock_guard lock(shared.mutex);
    WriteRepeats();
    if (file != INVALID_HANDLE_VALUE && !FlushFileBuffers(file))
        WriteFailure(GetLastError());
}

void LogStackTrace()
{
    FlushLog();
    const HANDLE out = OpenEmergencyLog();
    EmergencyWrite(out, "Call stack: tid=%lu tick=%llu\r\n", GetCurrentThreadId(), GetTickCount64());
    void* addresses[64];
    const WORD count = CaptureStackBackTrace(1, static_cast<DWORD>(std::size(addresses)), addresses, nullptr);
    for (WORD i = 0; i < count; ++i)
        StackAddress(out, i, addresses[i]);
    if (out != INVALID_HANDLE_VALUE)
    {
        FlushFileBuffers(out);
        CloseHandle(out);
    }
}

void LogReShadeDiagnostics()
{
    static LONGLONG offset = 0;
    static bool diagnostic = false;
    const HANDLE source = CreateFileW((ExeDirectory() + L"ReShade.log").c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (source == INVALID_HANDLE_VALUE)
        return;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(source, &size))
    {
        CloseHandle(source);
        return;
    }
    if (size.QuadPart < offset)
    {
        offset = 0;
        diagnostic = false;
    }
    LARGE_INTEGER position{};
    position.QuadPart = offset;
    if (!SetFilePointerEx(source, position, nullptr, FILE_BEGIN))
    {
        CloseHandle(source);
        return;
    }
    // Bound each read, including when an add-on floods ReShade's log.
    std::string text(static_cast<size_t>(std::min(size.QuadPart - offset, 1024ll * 1024)), '\0');
    DWORD read = 0;
    if (ReadFile(source, text.data(), static_cast<DWORD>(text.size()), &read, nullptr))
    {
        text.resize(read);
        size_t consumed = 0;
        for (size_t end; (end = text.find('\n', consumed)) != std::string::npos; consumed = end + 1)
        {
            std::string_view line(text.data() + consumed, end - consumed);
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            // Compiler diagnostics can continue onto lines without ReShade's timestamp and level.
            if (line.find(" | ") != std::string_view::npos)
                diagnostic = line.find(" | ERROR | ") != std::string_view::npos || line.find(" | WARN  | ") != std::string_view::npos;
            if (diagnostic)
                Log(LogLevel::Info, L"ReShade diagnostic: %ls", Wide(line).c_str());
        }
        offset += static_cast<LONGLONG>(consumed);
        if (!consumed && text.size() == 1024 * 1024)
        {
            // A single oversized entry must not prevent every later diagnostic from being read.
            offset += static_cast<LONGLONG>(text.size());
            Log(LogLevel::Info, L"Skipped an oversized ReShade log entry at byte %lld.", offset);
        }
    }
    CloseHandle(source);
}

void Log(LogLevel level, const wchar_t* format, ...)
{
    va_list args;
    va_start(args, format);
    Write(level, false, format, args);
    va_end(args);
}

void Report(LogLevel level, const wchar_t* format, ...)
{
    va_list args;
    va_start(args, format);
    Write(level, true, format, args);
    va_end(args);
}

std::vector<Notice> Notices()
{
    std::lock_guard lock(shared.mutex);
    return shared.notices;
}

void ClearNotices()
{
    std::lock_guard lock(shared.mutex);
    if (shared.notices.empty())
        return;
    shared.notices.clear();
    ++noticeVersion;
}

unsigned NoticeVersion()
{
    return noticeVersion;
}

size_t WebAddressLength(std::string_view word)
{
    return WebAddressLengthOf(word);
}

size_t WebAddressLength(std::wstring_view word)
{
    return WebAddressLengthOf(word);
}

const std::wstring& LogPath()
{
    return path;
}
