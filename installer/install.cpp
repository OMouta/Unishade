#include "install.h"
#include "resource.h"
#include "setup/package_files.h"
#include "base/text.h"

#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <stdexcept>

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace
{
// Inno Setup's key from earlier versions of Setup, so an update replaces its entry in Windows' app list.
constexpr wchar_t kUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{77125AF5-DF0A-485A-A633-E64FBD50E90C}_is1";
// Windows' graphics settings, one value per exe named after its path, holding "Name=Value;" pairs.
constexpr wchar_t kGpuPreferencesKey[] = L"Software\\Microsoft\\DirectX\\UserGpuPreferences";
// The launcher's "Start with Windows" entry, and where Windows keeps whether the user turned it off.
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kStartupApprovedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
// Lists the files Setup installed, relative to the installation folder, for uninstalling.
constexpr wchar_t kManifest[] = L"RobloxShadeHost-Setup.files";
constexpr wchar_t kSetupExe[] = L"Unishade-Setup.exe";

// What the host writes next to Unishade.exe for itself, deleted on every uninstall: its logs and locks, its copy of
// Discord's game icons, and the logs of the ReShade host and ReShade from Unishade 0.6.3 and earlier. ReShade
// numbers its log when another copy has it open.
constexpr const wchar_t* kLogs[] = { L"Unishade.log",  L"Unishade.old.log", L"Unishade.previous.log", L"unishade.lock",
                                     L"setup.lock",    L"DiscordIcons.txt", L"DiscordIcons.txt.tmp",  L"RobloxShadeHost.log",
                                     L"RobloxShadeHost.old.log", L"ReShade.log", L"ReShade.log1",     L"ReShade.log2",
                                     L"ReShade.log3",  L"ReShade.log4",     L"ReShade.log5",          L"ReShade.log6",
                                     L"ReShade.log7",  L"ReShade.log8",     L"ReShade.log9",          L"ReShade.log10" };
// The host's compiled effects, also deleted on every uninstall.
constexpr const wchar_t* kCacheFolders[] = { L"cache" };
// The user's settings, saved games and presets, deleted only when the user asks, with the settings Unishade 0.6.3 and
// earlier kept in ReShade.ini, ReShadePreset.ini and RobloxShadeHost.ini. The folders go with everything in them,
// including effects the user added to reshade-shaders and add-ons the user installed.
constexpr const wchar_t* kUserFiles[] = { L"Unishade.ini",      L"Unishade.ini.tmp",    L"games.ini",           L"games.ini.tmp",
                                          L"ReShade.ini",       L"ReShadePreset.ini",   L"RobloxShadeHost.ini", L"RobloxShadeHost.ini.tmp" };
constexpr const wchar_t* kUserFolders[] = { L"presets", L"reshade-shaders", L"Addons" };
// What Setup installed for Unishade 0.6.3 and earlier that this version does not use, removed when it updates one:
// ReShade, which would load into Unishade.exe from beside it, ReShade's license, the list of effect packages, the
// launcher's DLL and the add-ons' files, which Unishade now keeps in its Addons folder.
constexpr const wchar_t* kLeftovers[] = { L"dxgi.dll",        L"ReShade-LICENSE.txt",          L"EffectPackages.ini",
                                          L"UnishadeUi.dll",  L"onnxruntime.dll",              L"DirectML.dll",
                                          L"depth-anything-v2-small.onnx", L"nvngx_dlssnr.dll", L"renodx-dlss.addon64" };

const struct
{
    const wchar_t* link;
    const wchar_t* target;
} kShortcuts[] = {
    { L"Unishade.lnk", L"Unishade.exe" },
    { L"Unishade Setup.lnk", kSetupExe },
};

std::mutex logMutex;
std::ofstream logFile;
fs::path logPath;
fs::path movedSetup;

std::string Format(const char* format, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return buffer;
}

std::string PathText(const fs::path& path)
{
    return Utf8(path.wstring());
}

std::string SystemError(DWORD error)
{
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
                   reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring message = text ? text : L"error " + std::to_wstring(error);
    LocalFree(text);
    while (!message.empty() && wcschr(L"\r\n .", message.back()))
        message.pop_back();
    return Utf8(message);
}

void WriteFile(const fs::path& path, std::string_view data)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(data.data(), data.size());
    file.close();
    if (!file)
        throw std::runtime_error("Could not write " + PathText(path) + ".");
}

fs::path ModulePath()
{
    wchar_t path[32768]{};
    GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    return path;
}

fs::path KnownFolder(REFKNOWNFOLDERID id)
{
    PWSTR path = nullptr;
    SHGetKnownFolderPath(id, 0, nullptr, &path);
    fs::path folder = path ? path : L"";
    CoTaskMemFree(path);
    return folder;
}

fs::path SystemFolder()
{
    wchar_t path[MAX_PATH]{};
    GetSystemDirectoryW(path, MAX_PATH);
    return path;
}

struct HandleCloser
{
    void operator()(HANDLE handle) const { CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, HandleCloser>;

// A name nobody can guess and create first.
std::wstring RandomName(const wchar_t* prefix)
{
    unsigned char bytes[16]{};
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        throw std::runtime_error("Windows could not generate a random name.");
    std::wstring name = prefix;
    for (unsigned char byte : bytes)
    {
        name += L"0123456789abcdef"[byte >> 4];
        name += L"0123456789abcdef"[byte & 15];
    }
    return name;
}

// A file list entry must name something inside the folder: relative, without a drive, root, ':' or "..".
bool ValidEntry(const fs::path& directory, const std::wstring& entry)
{
    if (entry.empty() || entry.find(L':') != std::wstring::npos)
        return false;
    const fs::path path(entry);
    if (path.has_root_path())
        return false;
    for (const fs::path& part : path)
        if (part == L"..")
            return false;
    const fs::path target = (directory / path).lexically_normal();
    return IsInside(target, directory) && !IsInside(directory, target);
}

// Entries that point elsewhere, whether written by a damaged Setup or by someone else, are dropped and logged.
std::set<std::wstring> ReadManifest(const fs::path& directory)
{
    std::set<std::wstring> files;
    std::ifstream file(directory / kManifest, std::ios::binary);
    for (std::string line; std::getline(file, line);)
    {
        line.erase(line.find_last_not_of("\r") + 1);
        if (line.empty())
            continue;
        if (ValidEntry(directory, Wide(line)))
            files.insert(Wide(line));
        else
            SetupLog("Ignored an entry of " + Utf8(kManifest) + " that is outside the installation folder: " + line);
    }
    return files;
}

void WriteManifest(const fs::path& directory, const std::set<std::wstring>& files)
{
    std::string text;
    std::error_code ignored;
    for (const auto& file : files)
        if (!ValidEntry(directory, file))
            SetupLog("Left an entry outside the installation folder out of " + Utf8(kManifest) + ": " + Utf8(file));
        else if (fs::exists(directory / file, ignored))
            text += Utf8(file) + "\r\n";
    WriteFile(directory / kManifest, text);
}

// A folder Setup installed to: the host, under its current or earlier name, with Setup's file list.
bool IsInstallation(const fs::path& directory)
{
    std::error_code ignored;
    return fs::is_regular_file(directory / kManifest, ignored) &&
           (fs::is_regular_file(directory / L"Unishade.exe", ignored) || fs::is_regular_file(directory / L"RobloxShadeHost.exe", ignored));
}

// Removes a file that Setup replaces or no longer installs.
void RemoveFile(const fs::path& path)
{
    std::error_code error;
    fs::remove(path, error);
    if (error)
        throw std::runtime_error("Could not delete " + PathText(path) + ": " + SystemError(error.value()) + ".");
}

// Deletes a file, or a folder with everything in it, in the installation folder. Returns false and logs why when
// it cannot, including when a link or junction on the way leads outside the folder.
bool DeleteInside(const fs::path& root, const fs::path& path, bool folder)
{
    std::error_code error;
    const fs::path parent = fs::weakly_canonical(path.parent_path(), error);
    if (error || !IsInside(parent, root))
    {
        SetupLog("Did not delete " + PathText(path) + " because it is not inside the installation folder.");
        return false;
    }
    // Neither removal follows links or junctions; they are deleted themselves.
    if (folder)
        fs::remove_all(path, error);
    else
        fs::remove(path, error);
    if (error)
    {
        SetupLog("Could not delete " + PathText(path) + ": " + SystemError(error.value()));
        return false;
    }
    return true;
}

std::wstring CurrentUserSid()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        throw std::runtime_error("Could not read the current user's account: " + SystemError(GetLastError()) + ".");
    const Handle owned(token);
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> user(size);
    wchar_t* text = nullptr;
    if (!size || !GetTokenInformation(token, TokenUser, user.data(), size, &size) ||
        !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &text))
        throw std::runtime_error("Could not read the current user's account: " + SystemError(GetLastError()) + ".");
    std::wstring sid = text;
    LocalFree(text);
    return sid;
}

// A folder created outside the user's profile inherits its parent's permissions, and on C:\ those let every user of
// the PC modify the files in it, so another account could replace Unishade.exe or put a DLL it loads beside it. The
// first folder Setup creates there gets permissions of its own instead: full control for this user, SYSTEM and
// administrators, and read and run for other users. Folders that already exist keep theirs.
void CreateInstallFolder(const fs::path& directory)
{
    std::error_code ignored;
    fs::path first;
    for (fs::path folder = directory; folder.has_relative_path() && !fs::exists(folder, ignored); folder = folder.parent_path())
        first = folder;
    if (!first.empty() && !IsInside(directory, KnownFolder(FOLDERID_Profile)))
    {
        const std::wstring sddl = L"D:P(A;OICI;FA;;;" + CurrentUserSid() + L")(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)";
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
            throw std::runtime_error("Could not set up the permissions of " + PathText(first) + ": " + SystemError(GetLastError()) + ".");
        SECURITY_ATTRIBUTES attributes{ sizeof(attributes), descriptor, FALSE };
        const bool created = CreateDirectoryW(first.c_str(), &attributes) != FALSE;
        const DWORD error = GetLastError();
        LocalFree(descriptor);
        if (!created && error != ERROR_ALREADY_EXISTS)
            throw std::runtime_error("Could not create " + PathText(first) + ": " + SystemError(error) + ".");
        if (created)
            SetupLog("Created " + PathText(first) + ", which only this user and administrators can change.");
    }
    fs::create_directories(directory);
}

void CreateShortcut(const fs::path& link, const fs::path& target)
{
    ComPtr<IShellLinkW> shell;
    ComPtr<IPersistFile> file;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell))) || FAILED(shell->SetPath(target.c_str())) ||
        FAILED(shell->SetWorkingDirectory(target.parent_path().c_str())) || FAILED(shell.As(&file)) || FAILED(file->Save(link.c_str(), TRUE)))
        throw std::runtime_error("Could not create the Start menu shortcut " + PathText(link.stem()) + ".");
}

void Register(const fs::path& directory, const std::set<std::wstring>& files)
{
    uint64_t bytes = 0;
    std::error_code error;
    for (const auto& file : files)
    {
        const uint64_t size = fs::file_size(directory / file, error);
        bytes += error ? 0 : size;
    }

    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        throw std::runtime_error("Could not add Unishade to Windows' list of apps.");
    const auto text = [&](const wchar_t* name, const std::wstring& value) {
        RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    };
    const auto number = [&](const wchar_t* name, DWORD value) {
        RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    };
    const std::wstring setup = L"\"" + (directory / kSetupExe).wstring() + L"\"";
    text(L"DisplayName", L"Unishade");
    text(L"DisplayVersion", Wide(UNISHADE_VERSION));
    text(L"Publisher", L"Unishade contributors");
    text(L"DisplayIcon", (directory / L"Unishade.exe").wstring());
    text(L"InstallLocation", directory.wstring());
    text(L"UninstallString", setup + L" --uninstall");
    text(L"QuietUninstallString", setup + L" --uninstall --silent");
    text(L"ModifyPath", setup);
    text(L"URLInfoAbout", L"https://github.com/OMouta/Unishade");
    text(L"HelpLink", L"https://github.com/OMouta/Unishade/issues");
    number(L"EstimatedSize", static_cast<DWORD>(bytes / 1024));
    number(L"NoRepair", 1);
    RegCloseKey(key);
}

// The folder in Windows' app list entry, whether or not it still exists.
fs::path RegisteredDirectory()
{
    std::vector<wchar_t> location(32768);
    DWORD size = static_cast<DWORD>(location.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation", RRF_RT_REG_SZ, nullptr, location.data(), &size) != ERROR_SUCCESS)
        return {};
    fs::path directory = fs::path(location.data()).lexically_normal();
    return directory.has_filename() ? directory : directory.parent_path();
}

// Takes Unishade out of the apps Windows starts at sign-in, unless that entry starts another copy.
void RemoveFromStartup(const fs::path& directory)
{
    std::vector<wchar_t> command(32768);
    DWORD size = static_cast<DWORD>(command.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"Unishade", RRF_RT_REG_SZ, nullptr, command.data(), &size) != ERROR_SUCCESS || !command[0])
        return;
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(command.data(), &count);
    const fs::path exe = arguments && count > 0 ? fs::path(arguments[0]) : fs::path();
    LocalFree(arguments);
    const fs::path host = directory / L"Unishade.exe";
    if (exe.empty() || !IsInside(exe, host) || !IsInside(host, exe))
        return;
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, L"Unishade");
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kStartupApprovedKey, L"Unishade");
    SetupLog("Removed Unishade from the apps Windows starts at sign-in.");
}

// Calls back for each running host started from the folder, with the window that quits it when closed: the hidden
// window of Unishade's shortcuts, or the main window of Unishade 0.6.3 and earlier, which was a ReShade host.
template <typename Callback>
void ForEachHost(const fs::path& directory, Callback callback)
{
    for (const wchar_t* name : { L"UnishadeMessages", L"RobloxShadeHost" })
        for (HWND window = nullptr; (window = FindWindowExW(nullptr, window, name, nullptr));)
        {
            DWORD id = 0;
            GetWindowThreadProcessId(window, &id);
            const std::unique_ptr<void, decltype(&CloseHandle)> process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, id),
                                                                        &CloseHandle);
            if (!process)
                continue;
            wchar_t path[32768]{};
            DWORD size = static_cast<DWORD>(std::size(path));
            // A closed window cannot be the starting point of the next search, so start over.
            if (QueryFullProcessImageNameW(process.get(), 0, path, &size) && IsInside(path, directory) && callback(window, process.get()))
                window = nullptr;
        }
}

// Files cannot be replaced while the host runs from them, so ask it to quit. With unsaved changes to a preset it asks
// the user first, and Setup stops after waiting for it.
void CloseHost(const fs::path& directory)
{
    ForEachHost(directory, [](HWND window, HANDLE process) {
        SetupLog("Closing Unishade");
        PostMessageW(window, WM_CLOSE, 0, 0);
        if (WaitForSingleObject(process, 10000) != WAIT_OBJECT_0)
            throw std::runtime_error("Unishade is still running. Close it and try again.");
        return true;
    });
}

// "The effect package A was not installed." or "The effect packages A, B and C were not installed.", and what to do
// about it.
std::string SkippedNote(const std::vector<std::string>& names)
{
    std::string list;
    for (size_t index = 0; index < names.size(); ++index)
        list += (index == 0 ? "" : index + 1 == names.size() ? " and " : ", ") + names[index];
    return names.size() == 1 ? "The effect package " + list + " was not installed. Download again in Unishade's settings adds it."
                             : "The effect packages " + list + " were not installed. Download again in Unishade's settings adds them.";
}

// A download location from the command line as Unishade takes it, with a local file or folder as a file:/// address.
std::wstring SourceUrl(const std::wstring& location)
{
    if (location.rfind(L"https://", 0) == 0 || !fs::path(location).is_absolute())
        return location;
    return L"file:///" + fs::path(location).lexically_normal().generic_wstring();
}

// The files in the folder's reshade-shaders, relative to the folder.
std::set<std::wstring> EffectFiles(const fs::path& directory)
{
    std::set<std::wstring> files;
    std::error_code error;
    for (auto entry = fs::recursive_directory_iterator(directory / L"reshade-shaders", error); !error && entry != fs::recursive_directory_iterator();
         entry.increment(error))
        if (entry->is_regular_file(error))
            files.insert(entry->path().lexically_relative(directory).wstring());
    return files;
}

// Has the installed Unishade download the effects and presets into its folder, and follows the lines it writes, such
// as " 42%  Downloading effects - 3 of 40: SweetFX". The effect files it adds join Setup's file list, so uninstalling
// removes them as it removes the ones earlier versions of Setup installed. Returns false and leaves a note when a
// package was skipped, nothing could be installed or Setup was cancelled, which ends Unishade.
bool InstallEffects(const fs::path& directory, Progress& progress)
{
    constexpr float kBegin = 0.1f, kEnd = 1.0f;
    progress.Status("Downloading effects and presets");
    const std::set<std::wstring> before = EffectFiles(directory);
    // The sources go first, since Unishade starts installing when it reads --install-effects.
    std::wstring command = L"\"" + (directory / L"Unishade.exe").wstring() + L"\"";
    if (!sources.effects.empty())
        command += L" --effects-url \"" + SourceUrl(sources.effects) + L"\"";
    if (!sources.presets.empty())
        command += L" --presets-url \"" + SourceUrl(sources.presets) + L"\"";
    command += L" --install-effects";

    // Its output and errors go to one pipe, whose writing end only it holds.
    SECURITY_ATTRIBUTES inheritable{ sizeof(inheritable), nullptr, TRUE };
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &inheritable, 0))
        throw std::runtime_error("Could not start Unishade to download effects: " + SystemError(GetLastError()) + ".");
    const Handle output(read);
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{ sizeof(startup) };
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = write;
    startup.hStdError = write;
    PROCESS_INFORMATION process{};
    // A program that cannot start, as for a missing DLL, would wait behind an error box of Windows that a silent
    // install shows to nobody. With this error mode, which it inherits, it ends with an error code instead.
    const UINT errorMode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const bool started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup, &process);
    const DWORD startError = GetLastError();
    SetErrorMode(errorMode);
    CloseHandle(write);
    if (!started)
        throw std::runtime_error("Could not start Unishade to download effects: " + SystemError(startError) + ".");
    CloseHandle(process.hThread);
    const Handle running(process.hProcess);

    std::string pending, status, last;
    std::vector<std::string> skipped;
    const auto take = [&](std::string line) {
        line.erase(line.find_last_not_of("\r") + 1);
        if (line.empty())
            return;
        SetupLog("Unishade: " + line);
        int percent = 0;
        size_t at = 0;
        if (line.rfind("Skipped ", 0) == 0)
            skipped.push_back(line.substr(8, line.find(':') - 8));
        else if (sscanf_s(line.c_str(), "%d%%%zn", &percent, &at) == 1 && at)
        {
            const size_t start = line.find_first_not_of(' ', at);
            const std::string text = start == std::string::npos ? "" : line.substr(start);
            const size_t dash = text.find(" - ");
            if (text.substr(0, dash) != status)
            {
                status = text.substr(0, dash);
                progress.Status(status);
            }
            progress.Detail(dash == std::string::npos ? "" : text.substr(dash + 3));
            progress.Fraction(kBegin + (kEnd - kBegin) * std::clamp(percent, 0, 100) / 100.0f);
        }
        else
            last = line;
    };
    bool cancelled = false;
    for (bool exited = false; !exited;)
    {
        exited = WaitForSingleObject(running.get(), 100) != WAIT_TIMEOUT;
        DWORD available = 0;
        char buffer[4096];
        DWORD count = 0;
        while (PeekNamedPipe(read, nullptr, 0, nullptr, &available, nullptr) && available &&
               ReadFile(read, buffer, std::min<DWORD>(available, sizeof(buffer)), &count, nullptr) && count)
            pending.append(buffer, count);
        for (size_t end; (end = pending.find('\n')) != std::string::npos; pending.erase(0, end + 1))
            take(pending.substr(0, end));
        if (!exited && progress.cancel)
        {
            TerminateProcess(running.get(), 1);
            WaitForSingleObject(running.get(), 5000);
            cancelled = exited = true;
        }
    }
    take(pending);
    DWORD code = 1;
    GetExitCodeProcess(running.get(), &code);

    std::set<std::wstring> installed = ReadManifest(directory);
    for (const std::wstring& file : EffectFiles(directory))
        if (!before.count(file))
            installed.insert(file);
    WriteManifest(directory, installed);

    if (cancelled)
        progress.Note("Setup stopped before all effects and presets were downloaded. Download again in Unishade's settings gets them.");
    else if (code != 0)
        progress.Note("The effects and presets were not installed: " + (last.empty() ? Format("Unishade ended with exit code %lu.", code) : last) +
                      " Download again in Unishade's settings tries again.");
    else if (!skipped.empty())
        progress.Note(SkippedNote(skipped));
    progress.Fraction(kEnd);
    return !cancelled && code == 0 && skipped.empty();
}

void Commit(const fs::path& files, const InstallOptions& options, Progress& progress)
{
    const fs::path& directory = options.directory;
    progress.Status("Installing to " + PathText(directory));
    progress.Detail("");
    CloseHost(directory);
    std::set<std::wstring> installed;
    bool listRead = false;
    // When a step fails, what was copied so far stays in the file list, so uninstalling still removes it.
    const auto keepList = [&] {
        if (!listRead)
            return;
        try
        {
            WriteManifest(directory, installed);
        }
        catch (const std::exception& e)
        {
            SetupLog(e.what());
        }
    };
    try
    {
        CreateInstallFolder(directory);
        installed = ReadManifest(directory);
        listRead = true;
        for (const auto& entry : fs::directory_iterator(files))
        {
            const fs::path name = entry.path().filename();
            installed.insert(name.wstring());
            fs::copy_file(entry.path(), directory / name, fs::copy_options::overwrite_existing);
        }
        for (const wchar_t* leftover : kLeftovers)
        {
            RemoveFile(directory / leftover);
            installed.erase(leftover);
        }

        // Inno Setup's uninstaller from earlier versions of Setup.
        RemoveFile(directory / L"unins000.exe");
        RemoveFile(directory / L"unins000.dat");

        // Replace the old binaries in place; settings and the manifest keep their original names.
        for (const wchar_t* legacy : { L"RobloxShadeHost.exe", L"RobloxShadeHost-Setup.exe" })
        {
            RemoveFile(directory / legacy);
            installed.erase(legacy);
        }

        if (!options.portable)
        {
            const fs::path copy = directory / kSetupExe;
            installed.insert(kSetupExe);
            std::error_code different;
            if (!fs::equivalent(ModulePath(), copy, different))
                fs::copy_file(ModulePath(), copy, fs::copy_options::overwrite_existing);
            const fs::path startMenu = KnownFolder(FOLDERID_Programs);
            for (const auto& shortcut : kShortcuts)
                CreateShortcut(startMenu / shortcut.link, directory / shortcut.target);
            RemoveFile(startMenu / L"RobloxShadeHost.lnk");
            RemoveFile(startMenu / L"RobloxShadeHost Setup.lnk");
            Register(directory, installed);
        }
        WriteManifest(directory, installed);
    }
    catch (const fs::filesystem_error& e)
    {
        keepList();
        // A copy's error names the source first; the destination is the one that could not be written.
        const fs::path& path = e.path2().empty() ? e.path1() : e.path2();
        throw std::runtime_error("Could not write " + PathText(path) + ": " + SystemError(e.code().value()) + ".");
    }
    catch (...)
    {
        keepList();
        throw;
    }
    progress.Fraction(0.1f);
}
} // namespace

void Progress::Status(const std::string& text)
{
    SetupLog(text);
    std::lock_guard lock(mutex);
    state.status = text;
    state.detail.clear();
    state.history.push_back(text);
}

void Progress::Detail(const std::string& text)
{
    std::lock_guard lock(mutex);
    state.detail = text;
}

void Progress::Fraction(float value)
{
    std::lock_guard lock(mutex);
    state.fraction = value;
}

void Progress::Note(const std::string& text)
{
    SetupLog(text);
    std::lock_guard lock(mutex);
    state.notes.push_back(text);
}

Progress::State Progress::Read() const
{
    std::lock_guard lock(mutex);
    return state;
}

void OpenSetupLog(const fs::path& path)
{
    std::lock_guard lock(logMutex);
    logPath = path;
    logFile.open(path, std::ios::binary | std::ios::trunc);
}

void SetupLog(const std::string& line)
{
    SYSTEMTIME time{};
    GetLocalTime(&time);
    std::lock_guard lock(logMutex);
    logFile << Format("%04u-%02u-%02u %02u:%02u:%02u  ", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond) << line << "\r\n";
    logFile.flush();
}

const fs::path& SetupLogPath()
{
    return logPath;
}

std::string_view Resource(int id)
{
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL loaded = resource ? LoadResource(nullptr, resource) : nullptr;
    if (!loaded)
        throw std::runtime_error("Setup is damaged. Download it again.");
    return { static_cast<const char*>(LockResource(loaded)), SizeofResource(nullptr, resource) };
}

bool Install(const InstallOptions& options, Progress& progress)
{
    SetupLog("Installing Unishade " UNISHADE_VERSION " to " + PathText(options.directory));
    // A new folder with a random name, so no other program can have prepared its contents.
    std::error_code error;
    const fs::path temp = fs::temp_directory_path(error);
    const fs::path work = temp / RandomName(L"Unishade-Setup-");
    if (error || !CreateDirectoryW(work.c_str(), nullptr))
    {
        const DWORD reason = error ? static_cast<DWORD>(error.value()) : GetLastError();
        throw std::runtime_error("Could not create a temporary folder in " + PathText(temp) + ": " + SystemError(reason) + ".");
    }
    struct Cleanup
    {
        fs::path path;
        ~Cleanup()
        {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    } cleanup{ work };
    const fs::path files = work / L"files";
    fs::create_directories(files, error);
    if (error)
        throw std::runtime_error("Could not prepare " + PathText(work) + ": " + SystemError(error.value()) + ".");

    WriteFile(files / L"Unishade.exe", Resource(IDR_HOST));
    WriteFile(files / L"LICENSE", Resource(IDR_LICENSE));
    WriteFile(files / L"CREDITS.txt", Resource(IDR_CREDITS));
    if (progress.cancel)
        throw Cancelled{};
    Commit(files, options, progress);
    const bool complete = !options.effects || InstallEffects(options.directory, progress);
    SetupLog(complete ? "Installation finished." : "Installation finished without the parts listed above.");
    return complete;
}

void Uninstall(const fs::path& directory, bool deleteUserFiles)
{
    SetupLog("Uninstalling from " + PathText(directory));
    // Only a folder Setup installed to, and only what Setup, the host and ReShade put there, is deleted.
    if (!IsInstallation(directory))
        throw std::runtime_error(PathText(directory) + " is not a Unishade installation, so Setup did not delete anything in it.");
    std::error_code error;
    const fs::path root = fs::weakly_canonical(directory, error);
    if (error)
        throw std::runtime_error("Could not read " + PathText(directory) + ": " + SystemError(error.value()) + ".");
    CloseHost(directory);

    // A running exe cannot be deleted, but it can be moved on the same drive.
    const fs::path self = ModulePath();
    if (IsInside(self, directory))
    {
        const std::wstring name = RandomName(L"Unishade-Setup-") + L".exe";
        for (const fs::path& target : { fs::temp_directory_path() / name, directory.parent_path() / name })
            if (MoveFileExW(self.c_str(), target.c_str(), 0))
            {
                movedSetup = target;
                break;
            }
    }

    // The shortcuts and the app list entry belong to the registered installation, not to a portable copy.
    std::error_code ignored;
    const fs::path registered = RegisteredDirectory();
    if (!registered.empty() && IsInside(registered, directory) && IsInside(directory, registered))
    {
        const fs::path startMenu = KnownFolder(FOLDERID_Programs);
        for (const auto& shortcut : kShortcuts)
            fs::remove(startMenu / shortcut.link, ignored);
        fs::remove(startMenu / L"RobloxShadeHost.lnk", ignored);
        fs::remove(startMenu / L"RobloxShadeHost Setup.lnk", ignored);
        RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
    }
    // Windows keeps graphics settings for exes that no longer exist.
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kGpuPreferencesKey, (directory / L"Unishade.exe").c_str());
    RemoveFromStartup(directory);

    // Files nobody listed here, such as screenshots, stay.
    std::set<std::wstring> remaining;
    for (const auto& file : ReadManifest(directory))
    {
        const fs::path path = directory / file;
        // Setup's own exe stays when it could not be moved out of the way.
        const bool runningSetup = movedSetup.empty() && IsInside(path, self) && IsInside(self, path);
        if (!DeleteInside(root, path, false) && !runningSetup)
            remaining.insert(file);
    }
    bool failed = !remaining.empty();
    for (const wchar_t* logName : kLogs)
        if (!DeleteInside(root, directory / logName, false))
            failed = true;
    if (deleteUserFiles)
    {
        for (const wchar_t* file : kUserFiles)
            if (!DeleteInside(root, directory / file, false))
                failed = true;
        for (const wchar_t* folder : kUserFolders)
            if (!DeleteInside(root, directory / folder, true))
                failed = true;
    }
    // The list keeps what could not be deleted, so uninstalling again can finish the job.
    if (remaining.empty())
    {
        if (!DeleteInside(root, directory / kManifest, false))
            failed = true;
    }
    else
        WriteManifest(directory, remaining);
    for (const wchar_t* folder : kCacheFolders)
        if (!DeleteInside(root, directory / folder, true))
            failed = true;

    // Deepest folders first, so parents are empty by the time they are tried. Folders with other files stay, and
    // links and junctions are left alone.
    std::vector<fs::path> folders;
    for (auto entry = fs::recursive_directory_iterator(directory, error); !error && entry != fs::recursive_directory_iterator();
         entry.increment(error))
        if (entry->symlink_status(ignored).type() == fs::file_type::directory)
            folders.push_back(entry->path());
    std::sort(folders.begin(), folders.end(), [](const fs::path& a, const fs::path& b) { return a.native().size() > b.native().size(); });
    if (fs::symlink_status(directory, ignored).type() == fs::file_type::directory)
        folders.push_back(directory);
    for (const auto& folder : folders)
        fs::remove(folder, ignored);
    if (failed)
        throw std::runtime_error("Some files in " + PathText(directory) + " could not be deleted. The setup log lists them. Close the programs "
                                 "that use them, then delete them yourself.");
    SetupLog("Uninstalled.");
}

void DeleteMovedSetup()
{
    if (movedSetup.empty())
        return;
    // Gives Setup two seconds to exit, then deletes it. The paths reach cmd.exe through the environment, which it
    // expands once, and stand in quotes, so neither % signs nor other characters in them mean anything to it.
    const fs::path system32 = SystemFolder();
    const fs::path cmd = system32 / L"cmd.exe";
    SetEnvironmentVariableW(L"UNISHADE_PING", (system32 / L"PING.EXE").c_str());
    SetEnvironmentVariableW(L"UNISHADE_SETUP_COPY", movedSetup.c_str());
    std::wstring command = L"\"" + cmd.wstring() + L"\" /d /v:off /s /c \"\"%UNISHADE_PING%\" -n 3 127.0.0.1 >nul & del /f /q \"%UNISHADE_SETUP_COPY%\"\"";
    STARTUPINFOW startup{ sizeof(startup) };
    PROCESS_INFORMATION process{};
    if (CreateProcessW(cmd.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, system32.c_str(), &startup, &process))
    {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

std::optional<Installation> FindInstallation()
{
    const fs::path directory = RegisteredDirectory();
    std::error_code ignored;
    if (directory.empty() || (!fs::exists(directory / L"Unishade.exe", ignored) && !fs::exists(directory / L"RobloxShadeHost.exe", ignored)))
        return std::nullopt;
    wchar_t version[64]{};
    DWORD size = sizeof(version);
    RegGetValueW(HKEY_CURRENT_USER, kUninstallKey, L"DisplayVersion", RRF_RT_REG_SZ, nullptr, version, &size);
    return Installation{ directory, Utf8(version) };
}

bool HostRunning(const fs::path& directory)
{
    bool running = false;
    ForEachHost(directory, [&](HWND, HANDLE) {
        running = true;
        return false;
    });
    return running;
}

void LaunchHost(const fs::path& directory)
{
    ShellExecuteW(nullptr, L"open", (directory / L"Unishade.exe").c_str(), nullptr, directory.c_str(), SW_SHOWNORMAL);
}

namespace
{
std::vector<std::wstring> GpuSettings(const fs::path& exe)
{
    std::vector<std::wstring> settings;
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kGpuPreferencesKey, exe.c_str(), RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS)
        return settings;
    std::wstring text(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, kGpuPreferencesKey, exe.c_str(), RRF_RT_REG_SZ, nullptr, text.data(), &size) != ERROR_SUCCESS)
        return settings;
    text.resize(wcslen(text.c_str()));
    for (size_t start = 0; start < text.size();)
    {
        const size_t end = std::min(text.find(L';', start), text.size());
        if (end > start)
            settings.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return settings;
}
} // namespace

int GpuPreference(const fs::path& directory)
{
    for (const std::wstring& setting : GpuSettings(directory / L"Unishade.exe"))
        if (setting.rfind(L"GpuPreference=", 0) == 0)
            return _wtoi(setting.c_str() + wcslen(L"GpuPreference="));
    return -1;
}

void SetHighPerformanceGpu(const fs::path& directory)
{
    const fs::path exe = directory / L"Unishade.exe";
    std::wstring value;
    for (const std::wstring& setting : GpuSettings(exe))
        if (setting.rfind(L"GpuPreference=", 0) != 0)
            value += setting + L";";
    value += L"GpuPreference=2;";
    HKEY key = nullptr;
    const bool saved = RegCreateKeyExW(HKEY_CURRENT_USER, kGpuPreferencesKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS &&
                       RegSetValueExW(key, exe.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                      static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    if (key)
        RegCloseKey(key);
    SetupLog(saved ? "Set " + PathText(exe) + " to High performance in Windows' graphics settings."
                   : "Could not set " + PathText(exe) + " to High performance in Windows' graphics settings.");
}
