#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

// Called with the bytes received so far and the total, which is 0 when the server does not send one.
using DownloadProgress = std::function<void(uint64_t received, uint64_t total)>;

// Thrown when the user cancels.
struct Cancelled
{
};

// The most a download may be, so a broken or hostile server cannot fill the memory or the disk.
constexpr uint64_t kListLimit = 16ull << 20;     // download lists, presets, licenses and GitHub's JSON
constexpr uint64_t kPackageLimit = 256ull << 20; // effect packages and ReShade's installer
constexpr uint64_t kAddonLimit = 1ull << 30;     // add-on files, such as the depth estimation model

// Downloads an HTTPS url into memory, compressed when the server can, in which case maxSize applies to the
// decompressed data and progress gets no total. Throws std::runtime_error with a readable message, or Cancelled, which
// also interrupts a download that is still connecting or waiting for the server.
std::string Fetch(const std::wstring& url, const std::atomic<bool>& cancel, uint64_t maxSize = kListLimit, const DownloadProgress& progress = {});

struct HttpResponse
{
    unsigned status = 0;
    std::string body;
};

// Sends an HTTPS request with headers, each "Name: value" followed by "\r\n", and a body, and returns the answer
// whatever its status. Throws like Fetch when no answer arrives.
HttpResponse Request(const wchar_t* method, const std::wstring& url, const std::wstring& headers, const std::string& body,
                     const std::atomic<bool>& cancel, uint64_t maxSize = kListLimit);

// Downloads an HTTPS url to a file. A non-empty sha256 (lowercase hex) must match the downloaded data. Returns the
// SHA-256 of the data as it came from the server.
std::string Download(const std::wstring& url, const std::filesystem::path& path, const std::string& sha256, uint64_t maxSize,
                     const std::atomic<bool>& cancel, const DownloadProgress& progress = {});

// The SHA-256 (lowercase hex) of the rest of an open file, read through its handle.
std::string Sha256(void* file);

// Splits a plain HTTPS address on the default port into its host and path. False for anything else, including an
// address with a user name, password, query or fragment.
bool SplitHttpsUrl(const std::wstring& url, std::wstring& host, std::wstring& path);
