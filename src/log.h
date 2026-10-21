#pragma once

#include <string>
#include <string_view>
#include <vector>

enum class LogLevel
{
    Info,
    Ok,
    Warning,
    Error,
};

struct Notice
{
    LogLevel level;
    std::wstring text;
};

// Opens Unishade.log beside the exe and writes a header with the version and system. The previous
// run's log is kept as Unishade.old.log.
void InitLog();

// MSVC's terminate handler is per thread. Call at the start of host worker threads.
void InitThreadLog();

// Writes a timestamped line to the log file. Warnings and errors are also shown in the launcher and the
// menu. printf-style; use %ls for wide strings and %hs for narrow ones. Safe from any thread. A line that
// repeats on the same thread right away is counted. At 16 MB, the file rolls over to Unishade.previous.log.
void Log(LogLevel level, const wchar_t* format, ...);

// Like Log, but shown in the launcher and the menu at any level. For what the user should see at a glance,
// such as the installation check.
void Report(LogLevel level, const wchar_t* format, ...);

// What Report and warnings and errors have shown so far, oldest first. A repeated message is shown once.
std::vector<Notice> Notices();

// Forgets the notices shown so far, when the user dismisses them. The log file keeps them.
void ClearNotices();

// Changes whenever a notice is added or the notices are cleared.
unsigned NoticeVersion();

// How much of a word in a notice is a web address, which the launcher and the menu show as a link: all of it but
// punctuation after the address, such as a full stop, or nothing when the word is not one.
size_t WebAddressLength(std::string_view word);
size_t WebAddressLength(std::wstring_view word);

const std::wstring& LogPath();

// Writes what the log still holds back, such as how often the last line repeated. Called before the host exits.
void FlushLog();

// Writes the current call stack as module paths and offsets, including in builds without debug symbols.
void LogStackTrace();

// Copies new ReShade warnings and errors, including shader compiler diagnostics, into Unishade.log.
// Called on the host thread.
void LogReShadeDiagnostics();
