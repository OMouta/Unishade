#pragma once

#include <filesystem>
#include <string>
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
    std::string text;
};

// Opens Unishade.log in the data folder and writes a header with the version and system. The previous run's log
// is kept as Unishade.old.log. Lines also go to stderr.
void InitLog();

// Writes a timestamped line to the log. Warnings and errors are also shown in the launcher and the menu.
// printf-style. Safe from any thread. At 16 MB the file rolls over to Unishade.previous.log.
void Log(LogLevel level, const char* format, ...) __attribute__((format(printf, 2, 3)));

// Like Log, but shown in the launcher and the menu at any level.
void Report(LogLevel level, const char* format, ...) __attribute__((format(printf, 2, 3)));

// What Report and warnings and errors have shown so far, oldest first: the last 40, each message once.
std::vector<Notice> Notices();
// Empties that list. The log keeps the messages.
void ClearNotices();

const std::filesystem::path& LogPath();
void FlushLog();
