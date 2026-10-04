#include "log.h"
#include "config.h"
#include "text.h"
#include "web_address.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace
{
constexpr size_t kMaxNotices = 40;
// The log stops growing here, so something that keeps logging cannot fill the disk.
constexpr unsigned long long kMaxLogSize = 16ull * 1024 * 1024;

// Never destroyed, since other threads, such as the update check, may still log while the host exits.
struct Shared
{
    std::mutex mutex;
    std::vector<Notice> notices;
    unsigned long long size = 0;
    bool full = false;
    // The last line written, without its time, and how often it came again right after.
    LogLevel lastLevel = LogLevel::Info;
    std::string lastText;
    unsigned repeats = 0;
    std::string repeatStamp;
};
Shared& shared = *new Shared;
HANDLE file = INVALID_HANDLE_VALUE;
std::wstring path;
std::atomic<unsigned> noticeVersion = 0;

// Called with the mutex held.
void WriteToFile(std::string text)
{
    if (file == INVALID_HANDLE_VALUE || shared.full)
        return;
    if (shared.size + text.size() > kMaxLogSize)
    {
        shared.full = true;
        text = "Unishade.log reached 16 MB, so nothing more is written to it until Unishade restarts.\r\n";
    }
    DWORD written = 0;
    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
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
    wchar_t buffer[2048];
    _vsnwprintf_s(buffer, _TRUNCATE, format, args);

    static constexpr const char* kNames[] = { "", "ok", "warning", "error" };
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char stamp[64];
    snprintf(stamp, sizeof(stamp), "%04u-%02u-%02u %02u:%02u:%02u.%03u  %-7s  ", time.wYear, time.wMonth, time.wDay, time.wHour,
             time.wMinute, time.wSecond, time.wMilliseconds, kNames[static_cast<int>(level)]);
    std::string text = Utf8(buffer);

    std::lock_guard lock(shared.mutex);
    // A line that keeps coming, such as from a retry, is written once and then counted.
    if (level == shared.lastLevel && text == shared.lastText)
    {
        ++shared.repeats;
        shared.repeatStamp = stamp;
    }
    else
    {
        WriteRepeats();
        WriteToFile(stamp + text + "\r\n");
        shared.lastLevel = level;
        shared.lastText = std::move(text);
    }
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
} // namespace

void InitLog()
{
    path = ExeDirectory() + L"Unishade.log";
    MoveFileExW(path.c_str(), (ExeDirectory() + L"Unishade.old.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    const DWORD error = GetLastError();

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
}

void FlushLog()
{
    std::lock_guard lock(shared.mutex);
    WriteRepeats();
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
