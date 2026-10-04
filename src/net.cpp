#include "net.h"
#include "text.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
struct InternetHandleDeleter
{
    void operator()(HINTERNET handle) const { WinHttpCloseHandle(handle); }
};
using InternetHandle = std::unique_ptr<void, InternetHandleDeleter>;

std::string ErrorText(DWORD error)
{
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_FROM_HMODULE,
                   GetModuleHandleW(L"winhttp.dll"), error, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring message = text ? text : L"error " + std::to_wstring(error);
    LocalFree(text);
    while (!message.empty() && wcschr(L"\r\n .", message.back()))
        message.pop_back();
    return Utf8(message);
}

class Sha256Hasher
{
public:
    Sha256Hasher()
    {
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) ||
            !BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0)))
            throw std::runtime_error("SHA-256 is unavailable on this system.");
    }
    Sha256Hasher(const Sha256Hasher&) = delete;
    Sha256Hasher& operator=(const Sha256Hasher&) = delete;
    ~Sha256Hasher()
    {
        if (hash)
            BCryptDestroyHash(hash);
        if (algorithm)
            BCryptCloseAlgorithmProvider(algorithm, 0);
    }

    void Add(const char* data, size_t size)
    {
        if (!BCRYPT_SUCCESS(BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data)), static_cast<ULONG>(size), 0)))
            throw std::runtime_error("Could not compute a SHA-256 checksum.");
    }

    std::string Finish()
    {
        unsigned char digest[32]{};
        if (!BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0)))
            throw std::runtime_error("Could not compute a SHA-256 checksum.");
        std::string text;
        for (unsigned char byte : digest)
        {
            text += "0123456789abcdef"[byte >> 4];
            text += "0123456789abcdef"[byte & 15];
        }
        return text;
    }

private:
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
};

// Owns a request and closes it from another thread as soon as the user cancels, which makes a WinHTTP call that is
// connecting or waiting for the server fail at once instead of after its timeout.
class RequestWatch
{
public:
    RequestWatch(HINTERNET watched, const std::atomic<bool>& stop) : request(watched), cancel(stop), thread([this] { Watch(); }) {}
    RequestWatch(const RequestWatch&) = delete;
    RequestWatch& operator=(const RequestWatch&) = delete;
    ~RequestWatch()
    {
        {
            std::lock_guard lock(mutex);
            done = true;
        }
        wake.notify_one();
        thread.join();
        if (!closed)
            WinHttpCloseHandle(request);
    }

private:
    void Watch()
    {
        {
            std::unique_lock lock(mutex);
            while (!done && !cancel)
                wake.wait_for(lock, std::chrono::milliseconds(100));
            if (done)
                return;
        }
        // The destructor reads closed only after joining this thread, so closing needs no lock.
        WinHttpCloseHandle(request);
        closed = true;
    }

    HINTERNET request;
    const std::atomic<bool>& cancel;
    std::mutex mutex;
    std::condition_variable wake;
    bool done = false;
    bool closed = false;
    std::thread thread;
};

// Keeps the flags that say why the server's certificate was not accepted.
void CALLBACK OnSecureFailure(HINTERNET, DWORD_PTR context, DWORD status, LPVOID information, DWORD length)
{
    if (status == WINHTTP_CALLBACK_STATUS_SECURE_FAILURE && context && information && length >= sizeof(DWORD))
        *reinterpret_cast<DWORD*>(context) = *static_cast<const DWORD*>(information);
}

// Sends the request and waits for the response. Returns false with the reason in GetLastError.
bool Send(HINTERNET request, const std::wstring& headers, const std::string& body, DWORD& certificateFailure)
{
    certificateFailure = 0;
    const DWORD size = static_cast<DWORD>(body.size());
    return WinHttpSendRequest(request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), headers.empty() ? 0 : static_cast<DWORD>(-1L),
                              body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()), size, size,
                              reinterpret_cast<DWORD_PTR>(&certificateFailure)) &&
           WinHttpReceiveResponse(request, nullptr);
}

DWORD StatusCode(HINTERNET request)
{
    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                        WINHTTP_NO_HEADER_INDEX);
    return status;
}

// Lets a proxy that uses Windows authentication sign in with the user's Windows account. Windows only does this on
// its own for proxies on the local network. Returns false when the proxy wants something else, such as a password.
bool UseWindowsCredentialsForProxy(HINTERNET request)
{
    DWORD supported = 0;
    DWORD first = 0;
    DWORD target = 0;
    if (!WinHttpQueryAuthSchemes(request, &supported, &first, &target) || target != WINHTTP_AUTH_TARGET_PROXY)
        return false;
    const DWORD scheme = (supported & WINHTTP_AUTH_SCHEME_NEGOTIATE) ? WINHTTP_AUTH_SCHEME_NEGOTIATE
                         : (supported & WINHTTP_AUTH_SCHEME_NTLM)    ? WINHTTP_AUTH_SCHEME_NTLM
                                                                     : 0;
    DWORD policy = WINHTTP_AUTOLOGON_SECURITY_LEVEL_LOW;
    return scheme && WinHttpSetOption(request, WINHTTP_OPTION_AUTOLOGON_POLICY, &policy, sizeof(policy)) &&
           WinHttpSetCredentials(request, WINHTTP_AUTH_TARGET_PROXY, scheme, nullptr, nullptr, nullptr);
}

// With decompress, WinHTTP asks the server to compress the data and hands it over decompressed. It then drops the
// Content-Length header, so maxSize applies to the decompressed data and the total is unknown. Without status, an
// answer other than 200 fails. With it, any answer is read and its status goes there.
void Transfer(const wchar_t* method, const std::wstring& url, const std::wstring& headers, const std::string& body,
              const std::function<void(const char*, size_t)>& sink, const std::atomic<bool>& cancel, uint64_t maxSize,
              const DownloadProgress& progress, bool decompress, DWORD* answered = nullptr)
{
    const std::string action = wcscmp(method, L"GET") == 0 ? "download " : "send to ";
    const auto fail = [&](const std::string& reason) { throw std::runtime_error("Could not " + action + Utf8(url) + ": " + reason + "."); };
    if (cancel)
        throw Cancelled{};

    // A null component with a non-zero length makes WinHttpCrackUrl point into url.
    URL_COMPONENTS parts{ sizeof(parts) };
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = 1;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
        fail("not an HTTPS address");
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    const std::wstring path = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) + std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength);

    InternetHandle session(WinHttpOpen(Wide(UNISHADE_USER_AGENT).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                       WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session)
        fail(ErrorText(GetLastError()));
    WinHttpSetTimeouts(session.get(), 30000, 30000, 30000, 30000);
    InternetHandle connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
    if (!connection)
        fail(ErrorText(GetLastError()));

    // Certificates are checked for revocation. Networks that block the revocation servers would make every download
    // fail, so when the check cannot be completed, the request is repeated without it, as browsers do. That is also
    // the case when Windows does not say why it refused the certificate. A certificate Windows reports as revoked
    // still fails, and the repeated request still checks everything else about the certificate.
    for (bool checkRevocation = true;; checkRevocation = false)
    {
        const HINTERNET handle = WinHttpOpenRequest(connection.get(), method, path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                    WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!handle)
            fail(ErrorText(GetLastError()));
        RequestWatch watch(handle, cancel);
        if (decompress)
        {
            DWORD formats = WINHTTP_DECOMPRESSION_FLAG_ALL;
            WinHttpSetOption(handle, WINHTTP_OPTION_DECOMPRESSION, &formats, sizeof(formats));
        }
        if (checkRevocation)
        {
            DWORD feature = WINHTTP_ENABLE_SSL_REVOCATION;
            WinHttpSetOption(handle, WINHTTP_OPTION_ENABLE_FEATURE, &feature, sizeof(feature));
        }
        WinHttpSetStatusCallback(handle, OnSecureFailure, WINHTTP_CALLBACK_FLAG_SECURE_FAILURE, 0);

        DWORD certificateFailure = 0;
        bool sent = Send(handle, headers, body, certificateFailure);
        for (int attempt = 0; sent && attempt < 2 && StatusCode(handle) == HTTP_STATUS_PROXY_AUTH_REQ && UseWindowsCredentialsForProxy(handle);
             ++attempt)
            sent = Send(handle, headers, body, certificateFailure);
        if (!sent)
        {
            const DWORD error = GetLastError();
            if (cancel)
                throw Cancelled{};
            const bool secure = error == ERROR_WINHTTP_SECURE_FAILURE || error == ERROR_WINHTTP_SECURE_CHANNEL_ERROR;
            if (checkRevocation && secure && (certificateFailure == 0 || certificateFailure == WINHTTP_CALLBACK_STATUS_FLAG_CERT_REV_FAILED))
                continue;
            if (secure && (certificateFailure & WINHTTP_CALLBACK_STATUS_FLAG_CERT_REVOKED))
                fail("the server's certificate was revoked");
            fail(ErrorText(error));
        }
        if (cancel)
            throw Cancelled{};

        const DWORD status = StatusCode(handle);
        if (status == HTTP_STATUS_PROXY_AUTH_REQ)
            fail("the proxy server asks for a sign-in that Windows cannot provide");
        if (answered)
            *answered = status;
        else if (status != HTTP_STATUS_OK)
            fail("the server answered " + std::to_string(status));
        uint64_t total = 0;
        DWORD size = sizeof(total);
        if (!WinHttpQueryHeaders(handle, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER64, WINHTTP_HEADER_NAME_BY_INDEX, &total, &size,
                                 WINHTTP_NO_HEADER_INDEX))
            total = 0;
        if (total > maxSize)
            fail("it is larger than expected");

        std::vector<char> buffer(1 << 16);
        uint64_t received = 0;
        for (;;)
        {
            if (cancel)
                throw Cancelled{};
            DWORD read = 0;
            if (!WinHttpReadData(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read))
            {
                const DWORD error = GetLastError();
                if (cancel)
                    throw Cancelled{};
                fail(ErrorText(error));
            }
            if (!read)
                break;
            received += read;
            if (received > maxSize)
                fail("it is larger than expected");
            sink(buffer.data(), read);
            if (progress)
                progress(received, total);
        }
        if (total && received != total)
            fail("the connection closed early");
        return;
    }
}
} // namespace

std::string Fetch(const std::wstring& url, const std::atomic<bool>& cancel, uint64_t maxSize, const DownloadProgress& progress)
{
    std::string data;
    Transfer(L"GET", url, {}, {}, [&](const char* chunk, size_t size) { data.append(chunk, size); }, cancel, maxSize, progress, true);
    return data;
}

HttpResponse Request(const wchar_t* method, const std::wstring& url, const std::wstring& headers, const std::string& body,
                     const std::atomic<bool>& cancel, uint64_t maxSize)
{
    HttpResponse response;
    DWORD status = 0;
    Transfer(method, url, headers, body, [&](const char* chunk, size_t size) { response.body.append(chunk, size); }, cancel, maxSize, {}, true,
             &status);
    response.status = status;
    return response;
}

std::string Download(const std::wstring& url, const std::filesystem::path& path, const std::string& sha256, uint64_t maxSize,
                     const std::atomic<bool>& cancel, const DownloadProgress& progress)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
        throw std::runtime_error("Could not write " + Utf8(path.wstring()) + ".");
    std::string received;
    try
    {
        Sha256Hasher hasher;
        Transfer(L"GET", url, {}, {},
            [&](const char* chunk, size_t size) {
                file.write(chunk, size);
                hasher.Add(chunk, size);
            },
            cancel, maxSize, progress, false);
        file.close();
        if (!file)
            throw std::runtime_error("Could not write " + Utf8(path.wstring()) + ".");
        received = hasher.Finish();
        if (!sha256.empty() && received != sha256)
            throw std::runtime_error("The download from " + Utf8(url) + " does not match its checksum.");
    }
    catch (...)
    {
        file.close();
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        throw;
    }
    return received;
}

std::string Sha256(void* file)
{
    Sha256Hasher hasher;
    std::vector<char> buffer(1 << 16);
    for (;;)
    {
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
            throw std::runtime_error("Could not read a downloaded file: " + ErrorText(GetLastError()) + ".");
        if (!read)
            return hasher.Finish();
        hasher.Add(buffer.data(), read);
    }
}

bool SplitHttpsUrl(const std::wstring& url, std::wstring& host, std::wstring& path)
{
    // A null component with a non-zero length makes WinHttpCrackUrl point into url.
    URL_COMPONENTS parts{ sizeof(parts) };
    parts.dwHostNameLength = parts.dwUserNameLength = parts.dwPasswordLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = 1;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS || parts.nPort != INTERNET_DEFAULT_HTTPS_PORT ||
        parts.dwUserNameLength || parts.dwPasswordLength || parts.dwExtraInfoLength)
        return false;
    host.assign(parts.lpszHostName, parts.dwHostNameLength);
    path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    return true;
}
