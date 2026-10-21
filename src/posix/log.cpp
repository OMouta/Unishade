#include "log.h"
#include "config.h"

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
    localtime_r(&seconds, &local);
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
            file = fopen(path.c_str(), "a");
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
} // namespace

void InitLog()
{
    path = DataDirectory() / "Unishade.log";
    std::error_code ignored;
    std::filesystem::rename(path, DataDirectory() / "Unishade.old.log", ignored);
    file = fopen(path.c_str(), "a");
    const int openError = file ? 0 : errno;
    std::error_code sizeError;
    const auto size = std::filesystem::file_size(path, sizeError);
    logSize = sizeError ? 0 : static_cast<size_t>(size);
    snprintf(crashPath, sizeof(crashPath), "%s", path.c_str());
    struct sigaction action{};
    action.sa_sigaction = OnCrash;
    action.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&action.sa_mask);
    for (int signal : { SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE })
        sigaction(signal, &action, nullptr);
    std::set_terminate(OnTerminate);

    utsname system{};
    uname(&system);
    Log(LogLevel::Info, "Unishade %s on %s %s (%s)", UNISHADE_VERSION, system.sysname, system.release, system.machine);
    Log(LogLevel::Info, "Session started: pid=%ld, build=" __DATE__ " " __TIME__ ", data=%s.", static_cast<long>(getpid()), path.parent_path().c_str());
    if (!file)
        Log(LogLevel::Warning, "Could not open Unishade.log: %s.", strerror(openError));
    else if (ignored && ignored != std::errc::no_such_file_or_directory)
        Log(LogLevel::Warning, "Could not preserve the previous log: %s. Appending to it instead.", ignored.message().c_str());
#ifdef __APPLE__
    for (uint32_t i = 0; i < _dyld_image_count(); ++i)
        Log(LogLevel::Info, "Module: %s, base=%p.", _dyld_get_image_name(i), static_cast<const void*>(_dyld_get_image_header(i)));
#endif
}

void FlushLog()
{
    std::lock_guard lock(mutex);
    if (file)
    {
        fflush(file);
        fsync(fileno(file));
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
