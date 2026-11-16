#include "log.h"
#include "config.h"

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#define PATH_MAX 32768
#else
#include <sys/utsname.h>
#include <fcntl.h>
#include <unistd.h>
#include <execinfo.h>
#ifdef __APPLE__
#include <sys/ucontext.h>
#include <mach-o/dyld.h>
#else
#include <ucontext.h>
#endif
#endif

#include <algorithm>
#include <cstdarg>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <exception>
#include <limits.h>
#include <mutex>
#include <thread>

namespace
{
constexpr size_t kMaxNotices = 40;
constexpr size_t kMaxLogSize = 16 * 1024 * 1024;

std::mutex mutex;
FILE* file = nullptr;
std::vector<Notice> notices;
std::filesystem::path path;
char crashPath[PATH_MAX]{};
size_t logSize = 0;
const auto startedAt = std::chrono::steady_clock::now();

#ifdef _WIN32
// The exception, then each address of the stack in its module, so an offline debugger can place them without symbols
// installed here.
HANDLE OpenEmergencyLog()
{
    return CreateFileA(crashPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}

void EmergencyWrite(HANDLE out, const char* format, ...)
{
    char text[2048];
    va_list args;
    va_start(args, format);
    const int length = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (length <= 0)
        return;
    DWORD written = 0;
    for (const HANDLE target : { out, GetStdHandle(STD_ERROR_HANDLE) })
        if (target && target != INVALID_HANDLE_VALUE)
            WriteFile(target, text, static_cast<DWORD>(std::min(static_cast<size_t>(length), sizeof(text) - 1)), &written, nullptr);
}

void WriteStack(HANDLE out, CONTEXT context)
{
    // A damaged stack must not keep what was written already from reaching the disk.
    __try
    {
#if defined(_M_X64)
        for (unsigned i = 0; i < 64 && context.Rip; ++i)
        {
            void* base = nullptr;
            RtlPcToFileHeader(reinterpret_cast<void*>(context.Rip), &base);
            char module[MAX_PATH] = "unknown";
            if (base)
                GetModuleFileNameA(static_cast<HMODULE>(base), module, sizeof(module));
            EmergencyWrite(out, "  #%02u %s+0x%llX\n", i, module, static_cast<unsigned long long>(context.Rip - reinterpret_cast<uintptr_t>(base)));
            const DWORD64 previousStack = context.Rsp;
            DWORD64 functionBase = 0;
            if (const auto entry = RtlLookupFunctionEntry(context.Rip, &functionBase, nullptr))
            {
                void* data = nullptr;
                DWORD64 frame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, functionBase, context.Rip, entry, &context, &data, &frame, nullptr);
            }
            else
            {
                context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
                context.Rsp += sizeof(DWORD64);
            }
            if (context.Rsp <= previousStack)
                break;
        }
#endif
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        EmergencyWrite(out, "The rest of the stack is unreadable.\n");
    }
}

LONG WINAPI OnCrash(EXCEPTION_POINTERS* exception)
{
    const HANDLE out = OpenEmergencyLog();
    EmergencyWrite(out, "\nFATAL: exception=0x%08lX address=%p pid=%lu\n", exception->ExceptionRecord->ExceptionCode,
                   exception->ExceptionRecord->ExceptionAddress, GetCurrentProcessId());
    WriteStack(out, *exception->ContextRecord);
    if (out != INVALID_HANDLE_VALUE)
    {
        FlushFileBuffers(out);
        CloseHandle(out);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void OnTerminate()
{
    const HANDLE out = OpenEmergencyLog();
    EmergencyWrite(out, "\nFATAL: std::terminate\n");
    try
    {
        if (const auto error = std::current_exception())
            std::rethrow_exception(error);
    }
    catch (const std::exception& e)
    {
        EmergencyWrite(out, "%s\n", e.what());
    }
    catch (...)
    {
        EmergencyWrite(out, "Unhandled exception of unknown type.\n");
    }
    CONTEXT context{};
    RtlCaptureContext(&context);
    WriteStack(out, context);
    if (out != INVALID_HANDLE_VALUE)
    {
        FlushFileBuffers(out);
        CloseHandle(out);
    }
    std::abort();
}

std::string SystemName()
{
    // GetVersionEx reports Windows 8 to programs without a compatibility manifest.
    RTL_OSVERSIONINFOW version{ sizeof(version) };
    using RtlGetVersion = LONG(WINAPI*)(RTL_OSVERSIONINFOW*);
    if (const auto get = reinterpret_cast<RtlGetVersion>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))
        get(&version);
    return "Windows " + std::to_string(version.dwMajorVersion) + "." + std::to_string(version.dwMinorVersion) + "." +
           std::to_string(version.dwBuildNumber);
}
#else
int OpenEmergencyLog()
{
    return open(crashPath, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
}

void EmergencyWrite(int out, const char* text, size_t length)
{
    while (out >= 0 && length)
    {
        const ssize_t written = write(out, text, length);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            break;
        text += written;
        length -= static_cast<size_t>(written);
    }
}

void OnCrash(int signal, siginfo_t* info, void* state)
{
    // Signal handlers cannot use stdio, allocate memory or lock the normal logger.
    const int out = OpenEmergencyLog();
    char text[512];
    char* end = text;
    const auto word = [&](const char* value) { while (*value) *end++ = *value++; };
    const auto number = [&](uintptr_t value) {
        char digits[2 * sizeof(value)];
        unsigned count = 0;
        do { digits[count++] = "0123456789ABCDEF"[value % 16]; value /= 16; } while (value);
        while (count) *end++ = digits[--count];
    };
    word("\nFATAL: signal=0x"); number(static_cast<uintptr_t>(signal));
    word(" code=0x"); number(static_cast<uintptr_t>(info->si_code));
    word(" address=0x"); number(reinterpret_cast<uintptr_t>(info->si_addr));
    word(" pid=0x"); number(static_cast<uintptr_t>(getpid()));
    const auto* context = static_cast<const ucontext_t*>(state);
#if defined(__APPLE__) && defined(__aarch64__)
    word(" pc=0x"); number(context->uc_mcontext->__ss.__pc);
#elif defined(__APPLE__) && defined(__x86_64__)
    word(" pc=0x"); number(context->uc_mcontext->__ss.__rip);
#elif defined(__linux__) && defined(__x86_64__)
    word(" pc=0x"); number(static_cast<uintptr_t>(context->uc_mcontext.gregs[REG_RIP]));
#elif defined(__linux__) && defined(__aarch64__)
    word(" pc=0x"); number(context->uc_mcontext.pc);
#else
    (void)context;
#endif
    word("\n");
    EmergencyWrite(out, text, static_cast<size_t>(end - text));
    EmergencyWrite(STDERR_FILENO, text, static_cast<size_t>(end - text));
#ifdef __linux__
    // Module mappings let an offline debugger locate the fault even without symbols installed here.
    const int maps = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (maps >= 0)
    {
        char buffer[2048];
        ssize_t count;
        while ((count = read(maps, buffer, sizeof(buffer))) > 0)
            EmergencyWrite(out, buffer, static_cast<size_t>(count));
        close(maps);
    }
#endif
    if (out >= 0)
    {
        fsync(out);
        close(out);
    }
    // SA_RESETHAND restores the default handler. Preserve the usual signal exit status and core dump.
    raise(signal);
}

void OnTerminate()
{
    const int out = OpenEmergencyLog();
    constexpr char message[] = "\nFATAL: std::terminate\n";
    EmergencyWrite(out, message, sizeof(message) - 1);
    try
    {
        if (const auto error = std::current_exception())
            std::rethrow_exception(error);
    }
    catch (const std::exception& e)
    {
        EmergencyWrite(out, e.what(), strlen(e.what()));
        EmergencyWrite(out, "\n", 1);
    }
    catch (...)
    {
        constexpr char unknown[] = "Unhandled exception of unknown type.\n";
        EmergencyWrite(out, unknown, sizeof(unknown) - 1);
    }
    void* frames[64];
    const int count = backtrace(frames, 64);
    if (out >= 0)
    {
        backtrace_symbols_fd(frames, count, out);
        fsync(out);
        close(out);
    }
    std::abort();
}

std::string SystemName()
{
    utsname system{};
    uname(&system);
    return std::string(system.sysname) + " " + system.release + " (" + system.machine + ")";
}
#endif

const char* LevelName(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Ok:
        return "OK";
    case LogLevel::Warning:
        return "WARNING";
    case LogLevel::Error:
        return "ERROR";
    default:
        return "INFO";
    }
}

void Write(LogLevel level, bool report, const char* format, va_list args)
{
    va_list countArgs;
    va_copy(countArgs, args);
    const int length = vsnprintf(nullptr, 0, format, countArgs);
    va_end(countArgs);
    if (length < 0)
        return;
    constexpr size_t kMaxMessage = 65536;
    std::string text(std::min(static_cast<size_t>(length), kMaxMessage) + 1, '\0');
    vsnprintf(text.data(), text.size(), format, args);
    text.resize(strlen(text.c_str()));
    if (static_cast<size_t>(length) > kMaxMessage)
        text += " [message truncated at 65536 bytes]";

    char stamp[32];
    const auto now = std::chrono::system_clock::now();
    const time_t seconds = std::chrono::system_clock::to_time_t(now);
    tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    const auto uptime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt).count();
    const size_t thread = std::hash<std::thread::id>{}(std::this_thread::get_id());

    std::lock_guard lock(mutex);
    if (file && logSize + text.size() + 128 > kMaxLogSize)
    {
        fflush(file);
        std::error_code error;
        std::filesystem::rename(path, DataDirectory() / "Unishade.previous.log", error);
        if (!error)
        {
            fclose(file);
            file = fopen(path.string().c_str(), "a");
            logSize = 0;
            if (file)
                logSize = static_cast<size_t>(fprintf(file, "Unishade " UNISHADE_VERSION " log continued. Earlier entries are in Unishade.previous.log.\n"));
        }
        else
            fprintf(stderr, "Could not rotate Unishade.log: %s\n", error.message().c_str());
    }
    for (FILE* out : { file, stderr })
        if (out)
        {
            const int written = fprintf(out, "%s.%03lld %-7s [tid=%zu +%lldms] %s\n", stamp, static_cast<long long>(millis), LevelName(level),
                                        thread, static_cast<long long>(uptime), text.c_str());
            if (out == file && written > 0)
                logSize += static_cast<size_t>(written);
            if (fflush(out) != 0 || written < 0)
                fprintf(stderr, "Could not write Unishade.log: %s\n", strerror(errno));
        }
    if (!report && level != LogLevel::Warning && level != LogLevel::Error)
        return;
    // Retries, such as a capture that keeps failing, would otherwise fill the list with one message.
    if (std::any_of(notices.begin(), notices.end(), [&](const Notice& notice) { return notice.text == text; }))
        return;
    if (notices.size() == kMaxNotices)
        notices.erase(notices.begin());
    notices.push_back({ level, text });
}

// Crashes are written to crashPath, when there is one, and to stderr.
void InstallCrashHandlers()
{
#ifdef _WIN32
    SetUnhandledExceptionFilter(OnCrash);
#else
    struct sigaction action{};
    action.sa_sigaction = OnCrash;
    action.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&action.sa_mask);
    for (int signal : { SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE })
        sigaction(signal, &action, nullptr);
#endif
    std::set_terminate(OnTerminate);
}
} // namespace

void InitLog()
{
    path = DataDirectory() / "Unishade.log";
    std::error_code ignored;
    std::filesystem::rename(path, DataDirectory() / "Unishade.old.log", ignored);
    file = fopen(path.string().c_str(), "a");
    const int openError = file ? 0 : errno;
    std::error_code sizeError;
    const auto size = std::filesystem::file_size(path, sizeError);
    logSize = sizeError ? 0 : static_cast<size_t>(size);
    snprintf(crashPath, sizeof(crashPath), "%s", path.string().c_str());
    InstallCrashHandlers();

#ifdef _WIN32
    const long pid = static_cast<long>(GetCurrentProcessId());
#else
    const long pid = static_cast<long>(getpid());
#endif
    Log(LogLevel::Info, "Unishade %s on %s", UNISHADE_VERSION, SystemName().c_str());
    Log(LogLevel::Info, "Session started: pid=%ld, build=" __DATE__ " " __TIME__ ", data=%s.", pid, path.parent_path().string().c_str());
    if (!file)
        Log(LogLevel::Warning, "Could not open Unishade.log: %s.", strerror(openError));
    else if (ignored && ignored != std::errc::no_such_file_or_directory)
        Log(LogLevel::Warning, "Could not preserve the previous log: %s. Appending to it instead.", ignored.message().c_str());
#ifdef __APPLE__
    for (uint32_t i = 0; i < _dyld_image_count(); ++i)
        Log(LogLevel::Info, "Module: %s, base=%p.", _dyld_get_image_name(i), static_cast<const void*>(_dyld_get_image_header(i)));
#endif
}

void InitTerminalLog()
{
    InstallCrashHandlers();
    Log(LogLevel::Info, "Unishade %s on %s", UNISHADE_VERSION, SystemName().c_str());
}

void FlushLog()
{
    std::lock_guard lock(mutex);
    if (file)
    {
        fflush(file);
#ifdef _WIN32
        _commit(_fileno(file));
#else
        fsync(fileno(file));
#endif
    }
}

void Log(LogLevel level, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    Write(level, false, format, args);
    va_end(args);
}

void Report(LogLevel level, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    Write(level, true, format, args);
    va_end(args);
}

std::vector<Notice> Notices()
{
    std::lock_guard lock(mutex);
    return notices;
}

void ClearNotices()
{
    std::lock_guard lock(mutex);
    notices.clear();
}

const std::filesystem::path& LogPath()
{
    return path;
}
