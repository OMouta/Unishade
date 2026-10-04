#include "install.h"
#include "resource.h"
#include "../src/package_files.h"
#include "../src/preset_ini.h"
#include "../src/text.h"

#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
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

// Logs the host and ReShade write next to Unishade.exe, and the host's copy of Discord's game icons, deleted on every
// uninstall. ReShade numbers its log when another copy has it open.
constexpr const wchar_t* kLogs[] = { L"Unishade.log", L"Unishade.old.log", L"RobloxShadeHost.log", L"RobloxShadeHost.old.log",
                                     L"ReShade.log",  L"ReShade.log1",     L"ReShade.log2",        L"ReShade.log3",
                                     L"ReShade.log4", L"ReShade.log5",     L"ReShade.log6",        L"ReShade.log7",
                                     L"ReShade.log8", L"ReShade.log9",     L"ReShade.log10",       L"DiscordIcons.txt",
                                     L"DiscordIcons.txt.tmp" };
// The user's settings, saved games and presets, deleted only when the user asks. The folders go with everything in
// them, including effects the user added to reshade-shaders.
constexpr const wchar_t* kUserFiles[] = { L"ReShade.ini", L"ReShadePreset.ini", L"RobloxShadeHost.ini", L"games.ini", L"games.ini.tmp" };
constexpr const wchar_t* kUserFolders[] = { L"presets", L"reshade-shaders" };

// The files of each add-on, under the names the host loads them by. Where each one comes from and its SHA-256 are in
// the add-on's downloaded manifest, so they can change without a new Setup.
struct AddonInfo
{
    Addon addon;
    const char* name;
    const wchar_t* section;
    std::vector<const wchar_t*> files;
};
const AddonInfo kAddons[] = {
    { Addon::Depth, "Depth estimation", L"depth", { L"onnxruntime.dll", L"DirectML.dll", L"depth-anything-v2-small.onnx" } },
    { Addon::DLSS5, "DLSS5", L"dlss5", { L"nvngx_dlssnr.dll", L"renodx-dlss.addon64" } },
};

bool Selected(const InstallOptions& options, Addon addon)
{
    return addon == Addon::Depth ? options.depth : options.dlss5;
}

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

std::string SizeText(uint64_t bytes)
{
    return Format("%.1f MB", bytes / 1048576.0);
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

std::string ReadFile(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Could not read " + PathText(path) + ".");
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
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

bool IsSha256(const std::string& text)
{
    return text.size() == 64 && text.find_first_not_of("0123456789abcdef") == std::string::npos;
}

// A version such as 6.8.0: three numbers of one to nine digits, separated by dots.
bool IsVersion(const std::string& text)
{
    size_t start = 0;
    for (int part = 0; part < 3; ++part)
    {
        const size_t end = part < 2 ? text.find('.', start) : text.size();
        if (end == std::string::npos || end == start || end - start > 9 || text.find_first_not_of("0123456789", start) < end)
            return false;
        start = end + 1;
    }
    return true;
}

// The newest vX.Y.Z tag in ReShade's repository, without the v. GitHub lists the newest tags first, so the first page
// holds it.
std::string NewestReShadeTag(const std::atomic<bool>& cancel)
{
    const std::string json = Fetch(L"https://api.github.com/repos/crosire/reshade/tags?per_page=100", cancel, kListLimit);
    std::string newest;
    std::array<unsigned long, 3> newestNumbers{};
    for (size_t key = json.find("\"name\""); key != std::string::npos; key = json.find("\"name\"", key + 1))
    {
        const size_t start = json.find_first_not_of(" \t\r\n:", key + 6);
        const size_t end = start == std::string::npos || json[start] != '"' ? std::string::npos : json.find('"', start + 1);
        if (end == std::string::npos || json[start + 1] != 'v')
            continue;
        const std::string version = json.substr(start + 2, end - start - 2);
        std::array<unsigned long, 3> numbers{};
        if (IsVersion(version) && sscanf_s(version.c_str(), "%lu.%lu.%lu", &numbers[0], &numbers[1], &numbers[2]) == 3 && numbers > newestNumbers)
        {
            newest = version;
            newestNumbers = numbers;
        }
    }
    if (newest.empty())
        throw std::runtime_error("Could not find the newest ReShade on reshade.me or GitHub.");
    return newest;
}

std::wstring IniString(const fs::path& file, const std::wstring& section, const wchar_t* key, const wchar_t* fallback = L"")
{
    wchar_t value[4096]{};
    GetPrivateProfileStringW(section.c_str(), key, fallback, value, static_cast<DWORD>(std::size(value)), file.c_str());
    return value;
}

std::vector<std::wstring> IniSections(const fs::path& file)
{
    std::vector<wchar_t> buffer(1 << 16);
    const DWORD length = GetPrivateProfileSectionNamesW(buffer.data(), static_cast<DWORD>(buffer.size()), file.c_str());
    std::vector<std::wstring> sections;
    for (const wchar_t* name = buffer.data(); name < buffer.data() + length && *name; name += wcslen(name) + 1)
        sections.emplace_back(name);
    if (sections.empty())
        throw std::runtime_error("The download list " + PathText(file.filename()) + " is empty.");
    return sections;
}

std::string Lowercase(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return text;
}

// Reports download progress as bytes, and as the part between begin and end of the whole installation.
DownloadProgress ByteProgress(Progress& progress, float begin, float end, const std::string& label = "")
{
    return [&progress, begin, end, label](uint64_t received, uint64_t total) {
        progress.Detail(label + SizeText(received) + (total ? " of " + SizeText(total) : ""));
        if (total)
            progress.Fraction(begin + (end - begin) * static_cast<float>(received) / total);
    };
}

// Runs exe without a window and returns its exit code. exe stays locked by the handle it was verified through until the
// process has started. Cancelling ends the process.
DWORD RunHidden(const fs::path& exe, Handle verified, std::wstring command, const fs::path& directory, const std::atomic<bool>& cancel)
{
    STARTUPINFOW startup{ sizeof(startup) };
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup, &process))
        throw std::runtime_error("Could not start the ReShade installer: " + SystemError(GetLastError()) + ".");
    verified.reset();
    CloseHandle(process.hThread);
    const Handle running(process.hProcess);
    const ULONGLONG start = GetTickCount64();
    while (WaitForSingleObject(running.get(), 100) == WAIT_TIMEOUT)
    {
        const bool timedOut = GetTickCount64() - start > 5 * 60 * 1000;
        if (cancel || timedOut)
        {
            TerminateProcess(running.get(), 1);
            WaitForSingleObject(running.get(), 5000);
            if (cancel)
                throw Cancelled{};
            throw std::runtime_error("ReShade's installer did not finish within five minutes.");
        }
    }
    DWORD code = 1;
    GetExitCodeProcess(running.get(), &code);
    return code;
}

// ReShade's installer writes .\reshade-shaders\Shaders\**\** as the search paths. ReShade treats a
// trailing ** as "search recursively" and then looks for a folder literally named **, so no effect or
// texture is found. Drops the extra suffix and keeps everything else, including the byte order mark.
void FixReShadeIni(const fs::path& path, bool addPresetPath)
{
    const std::string text = ReadFile(path);
    const std::string newline = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::string result;
    bool changed = false;
    size_t start = 0;
    while (start < text.size())
    {
        size_t end = text.find('\n', start);
        end = end == std::string::npos ? text.size() : end + 1;
        std::string line = text.substr(start, end - start);
        start = end;

        std::string content = line.substr(0, line.find_last_not_of("\r\n") + 1);
        const std::string ending = line.substr(content.size());
        if ((content.rfind("EffectSearchPaths=", 0) == 0 || content.rfind("TextureSearchPaths=", 0) == 0) && content.size() > 6 &&
            content.compare(content.size() - 6, 6, "\\**\\**") == 0)
        {
            content.resize(content.size() - 3);
            changed = true;
        }
        result += content + ending;
        if (addPresetPath && content.find("[GENERAL]") != std::string::npos)
        {
            result += (ending.empty() ? newline : "") + "PresetPath=.\\presets\\ReShadePreset.ini" + newline;
            changed = true;
        }
    }
    if (changed)
        WriteFile(path, result);
}

// "The preset A was not installed." or "The presets A, B and C were not installed.", and what to do about it.
std::string SkippedNote(const char* one, const char* several, const std::vector<std::string>& names)
{
    std::string list;
    for (size_t index = 0; index < names.size(); ++index)
        list += (index == 0 ? "" : index + 1 == names.size() ? " and " : ", ") + names[index];
    return names.size() == 1 ? std::string("The ") + one + " " + list + " was not installed. Run Setup again later to add it."
                             : std::string("The ") + several + " " + list + " were not installed. Run Setup again later to add them.";
}

// A download list. Tests may give a local file instead of an address.
std::string FetchList(const std::wstring& location, const std::atomic<bool>& cancel)
{
    if (location.rfind(L"https://", 0) != 0 && fs::path(location).is_absolute())
        return ReadFile(location);
    return Fetch(location, cancel, kListLimit);
}

void InstallReShade(const fs::path& work, const fs::path& files, const ReShadeRelease& release, bool presets, Progress& progress)
{
    progress.Status("Downloading ReShade " + release.version);
    const fs::path setup = work / L"ReShade-Setup.exe";
    const std::string sha256 = Download(L"https://reshade.me/downloads/ReShade_Setup_" + Wide(release.version) + L"_Addon.exe", setup, "",
                                        kPackageLimit, progress.cancel, ByteProgress(progress, 0.0f, 0.05f));

    // Opened through a handle that keeps anyone from changing, renaming or deleting the file until ReShade's installer
    // has started from it. Checking it against what was downloaded covers the moment before it was opened.
    const HANDLE opened = CreateFileW(setup.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (opened == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Could not open the ReShade installer: " + SystemError(GetLastError()) + ".");
    Handle verified(opened);
    if (Sha256(verified.get()) != sha256)
        throw std::runtime_error("The ReShade installer changed after it was downloaded.");

    // ReShade's installer also leaves an empty preset and a log next to the exe, which must not replace
    // the user's files, so it runs in a folder of its own and only its DLL and settings are kept.
    progress.Status("Setting up ReShade");
    const fs::path folder = work / L"reshade";
    fs::create_directories(folder);
    fs::copy_file(files / L"Unishade.exe", folder / L"Unishade.exe");
    const DWORD code = RunHidden(setup, std::move(verified),
                                 L"\"" + setup.wstring() + L"\" --headless --api dxgi \"" + (folder / L"Unishade.exe").wstring() + L"\"", work,
                                 progress.cancel);
    if (code != 0 || !fs::exists(folder / L"dxgi.dll") || !fs::exists(folder / L"ReShade.ini"))
        throw std::runtime_error(Format("ReShade's installer failed (exit code %lu).", code));
    fs::copy_file(folder / L"dxgi.dll", files / L"dxgi.dll");
    fs::copy_file(folder / L"ReShade.ini", files / L"ReShade.ini");
    FixReShadeIni(files / L"ReShade.ini", presets);
    WriteFile(files / L"ReShade-LICENSE.txt", release.license);
    progress.Fraction(0.08f);
}

void InstallPackage(const fs::path& catalog, const std::wstring& section, const std::string& name, const fs::path& extracted,
                    const fs::path& files, const std::atomic<bool>& cancel)
{
    const std::wstring url = IniString(catalog, section, L"DownloadUrl");
    if (url.rfind(L"https://", 0) != 0)
        throw std::runtime_error("The effect package " + name + " has an invalid download address.");
    const fs::path shaderDestination = PackageDestination(files, Utf8(IniString(catalog, section, L"InstallPath")), "Shaders", name);

    fs::remove_all(extracted);
    ExtractZip(Fetch(url, cancel, kPackageLimit), extracted, name);
    const PackageFolders folders = FindPackageFolders(extracted, name);
    CopyPackageFiles(folders.shaders, shaderDestination, Utf8(IniString(catalog, section, L"DenyEffectFiles")));
    if (!folders.textures.empty())
        CopyPackageFiles(folders.textures, PackageDestination(files, Utf8(IniString(catalog, section, L"TextureInstallPath")), "Textures", name), "");
    fs::remove_all(extracted);
    SetupLog("Effect package installed: " + name);
}

// Every package on ReShade's list is installed, since presets use effects from packages the list does not select
// by default. A package that fails is left out with a note, except one the list marks as required: the standard
// effects with ReShade.fxh, which most other effects include. Returns false when a package was left out.
bool InstallEffects(const fs::path& work, const fs::path& files, Progress& progress)
{
    constexpr float kBegin = 0.08f, kEnd = 0.75f;
    progress.Status("Downloading effects");
    const fs::path catalog = files / L"EffectPackages.ini";
    WriteFile(catalog, FetchList(sources.effects, progress.cancel));
    const auto sections = IniSections(catalog);
    std::vector<std::string> skipped;
    for (size_t index = 0; index < sections.size(); ++index)
    {
        const std::wstring& section = sections[index];
        const std::string name = Utf8(IniString(catalog, section, L"PackageName", section.c_str()));
        progress.Detail(Format("%zu of %zu: ", index + 1, sections.size()) + name);
        const fs::path extracted = work / L"package";
        try
        {
            InstallPackage(catalog, section, name, extracted, files, progress.cancel);
        }
        catch (const std::exception& e)
        {
            std::error_code ignored;
            fs::remove_all(extracted, ignored);
            if (IniString(catalog, section, L"Required") == L"1")
                throw;
            SetupLog("Effect package skipped: " + name + ". " + e.what());
            skipped.push_back(name);
        }
        progress.Fraction(kBegin + (kEnd - kBegin) * (index + 1) / sections.size());
    }
    if (skipped.size() == sections.size())
        throw std::runtime_error("No effect package could be installed. Check your internet connection and try again.");
    if (!skipped.empty())
        progress.Note(SkippedNote("effect package", "effect packages", skipped));
    return skipped.empty();
}

void InstallPreset(const fs::path& catalog, const std::wstring& file, const fs::path& folder, const std::set<std::string>& effects,
                   const std::atomic<bool>& cancel)
{
    const std::string name = Utf8(file);
    if (fs::path(file).filename() != file || file.find(L':') != std::wstring::npos || _wcsicmp(fs::path(file).extension().c_str(), L".ini") != 0)
        throw std::runtime_error("The preset list contains an invalid filename.");
    const std::string hash = Lowercase(Utf8(IniString(catalog, file, L"sha256")));
    if (!IsSha256(hash))
        throw std::runtime_error("The preset list has an invalid checksum for " + name + ".");
    const fs::path preset = folder / file;
    Download(sources.presets + L"/" + file, preset, hash, kListLimit, cancel);
    try
    {
        // ReShade keeps Techniques before the first section, where the INI functions cannot read it.
        std::string techniques;
        PresetIni(ReadFile(preset)).Get("", "Techniques", techniques);
        if (PresetIni::Split(techniques).empty())
            throw std::runtime_error(name + " uses no effects.");
        for (const std::string& technique : PresetIni::Split(techniques))
        {
            const size_t at = technique.find('@');
            const std::string shader = at == std::string::npos ? "" : technique.substr(at + 1);
            if (shader.empty() || shader.find_first_of("\\/:") != std::string::npos || !effects.count(Lowercase(shader)))
                throw std::runtime_error(name + " needs " + (shader.empty() ? technique : shader) + ", which no installed effect package provides.");
        }
    }
    catch (...)
    {
        std::error_code ignored;
        fs::remove(preset, ignored);
        throw;
    }
}

// A preset that cannot be downloaded or verified, or needs an effect that was not installed, is left out with a
// note, and so are all of them when their list cannot be downloaded. Returns false when a preset was left out.
bool InstallPresets(const fs::path& work, const fs::path& files, Progress& progress)
{
    progress.Status("Downloading presets");
    const fs::path catalog = work / L"preset-downloads.ini";
    std::vector<std::wstring> presets;
    try
    {
        WriteFile(catalog, Fetch(sources.presets + L"/downloads.ini", progress.cancel));
        presets = IniSections(catalog);
    }
    catch (const std::exception& e)
    {
        SetupLog(std::string("Presets skipped: ") + e.what());
        progress.Note("The presets were not installed because their list could not be downloaded. Run Setup again later to add them.");
        return false;
    }

    std::set<std::string> effects;
    for (const auto& entry : fs::recursive_directory_iterator(files / L"reshade-shaders" / L"Shaders"))
        if (entry.is_regular_file())
            effects.insert(Lowercase(Utf8(entry.path().filename().wstring())));

    fs::create_directories(files / L"presets");
    std::vector<std::string> skipped;
    for (const std::wstring& file : presets)
    {
        try
        {
            InstallPreset(catalog, file, files / L"presets", effects, progress.cancel);
        }
        catch (const std::exception& e)
        {
            SetupLog("Preset skipped: " + Utf8(file) + ". " + e.what());
            skipped.push_back(Utf8(file));
        }
    }
    if (!skipped.empty())
        progress.Note(SkippedNote("preset", "presets", skipped));
    progress.Fraction(0.8f);
    return skipped.empty();
}

// The address to download an add-on file from, when the manifest's url points into this repository's GitHub releases
// or anywhere on Hugging Face. Addresses from before the repository was renamed move to the new name.
std::optional<std::wstring> AddonUrl(const std::wstring& url)
{
    std::wstring host, path;
    if (!SplitHttpsUrl(url, host, path))
        return std::nullopt;
    constexpr std::wstring_view legacy = L"/OMouta/RobloxShadeHost/releases/download/";
    constexpr std::wstring_view releases = L"/OMouta/Unishade/releases/download/";
    if (_wcsicmp(host.c_str(), L"github.com") == 0 && path.rfind(legacy, 0) == 0)
        path.replace(0, legacy.size(), releases);

    // Escapes and dot segments could lead the server somewhere other than the path reads.
    if (path.find_first_of(L"%\\") != std::wstring::npos)
        return std::nullopt;
    for (size_t start = 0; start < path.size();)
    {
        const size_t end = std::min(path.find(L'/', start), path.size());
        const std::wstring_view segment(path.data() + start, end - start);
        if (segment == L"." || segment == L"..")
            return std::nullopt;
        start = end + 1;
    }
    const bool release = _wcsicmp(host.c_str(), L"github.com") == 0 && path.size() > releases.size() && path.rfind(releases, 0) == 0;
    const bool huggingFace = _wcsicmp(host.c_str(), L"huggingface.co") == 0 && path.size() > 1;
    if (!release && !huggingFace)
        return std::nullopt;
    return L"https://" + host + path;
}

// Returns false and leaves a note when the add-on cannot be installed, so the rest still installs: when its manifest
// cannot be downloaded, turns it off or lacks a valid address or SHA-256 for a file, or when a download fails or does
// not match the manifest's SHA-256.
bool DownloadAddon(const fs::path& work, const fs::path& files, const AddonInfo& addon, Progress& progress)
{
    constexpr float kBegin = 0.8f, kEnd = 0.95f;
    progress.Status(std::string("Downloading ") + addon.name);
    try
    {
        const fs::path manifest = work / L"addon-downloads.ini";
        WriteFile(manifest, FetchList(addon.addon == Addon::Depth ? sources.depth : sources.dlss5, progress.cancel));
        if (IniString(manifest, addon.section, L"enabled", L"0") != L"1")
            throw std::runtime_error(std::string(addon.name) + " downloads are turned off for now.");
        // Every entry is checked before the first download, so an invalid manifest downloads nothing.
        const std::string invalid = std::string("The ") + addon.name + " download list ";
        std::vector<std::wstring> urls;
        std::vector<std::string> hashes;
        for (const wchar_t* file : addon.files)
        {
            const auto url = AddonUrl(IniString(manifest, file, L"url"));
            if (!url)
                throw std::runtime_error(invalid + "points " + Utf8(file) + " somewhere Setup does not download add-ons from.");
            const std::string hash = Lowercase(Utf8(IniString(manifest, file, L"sha256")));
            if (!IsSha256(hash))
                throw std::runtime_error(invalid + "has no valid checksum for " + Utf8(file) + ".");
            urls.push_back(*url);
            hashes.push_back(hash);
        }
        for (size_t index = 0; index < addon.files.size(); ++index)
        {
            const wchar_t* file = addon.files[index];
            const float begin = kBegin + (kEnd - kBegin) * index / addon.files.size();
            const float end = kBegin + (kEnd - kBegin) * (index + 1) / addon.files.size();
            Download(urls[index], files / file, hashes[index], kAddonLimit, progress.cancel, ByteProgress(progress, begin, end, Utf8(file) + ": "));
        }
        return true;
    }
    catch (const std::exception& e)
    {
        std::error_code ignored;
        for (const wchar_t* file : addon.files)
            fs::remove(files / file, ignored);
        SetupLog(std::string(addon.name) + " skipped: " + e.what());
        progress.Note(std::string(addon.name) + " was not installed because its download failed or could not be verified. Run Setup again "
                                                "later to add it.");
        return false;
    }
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
// the PC modify the files in it, so another account could replace dxgi.dll or Unishade.exe. The first folder Setup
// creates there gets permissions of its own instead: full control for this user, SYSTEM and administrators, and
// read and run for other users. Folders that already exist keep theirs.
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

// Calls back for each running host started from the folder.
template <typename Callback>
void ForEachHost(const fs::path& directory, Callback callback)
{
    for (HWND window = nullptr; (window = FindWindowExW(nullptr, window, L"RobloxShadeHost", nullptr));)
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

// Files cannot be replaced while the host runs from them, so ask it to quit.
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
        const bool hadReShadeIni = fs::exists(directory / L"ReShade.ini");
        installed = ReadManifest(directory);
        listRead = true;
        for (const auto& entry : fs::recursive_directory_iterator(files))
        {
            if (!entry.is_regular_file())
                continue;
            const fs::path relative = entry.path().lexically_relative(files);
            const fs::path target = directory / relative;
            // ReShade settings and presets belong to the user once installed.
            const bool userFile = _wcsicmp(relative.c_str(), L"ReShade.ini") == 0 || _wcsicmp(relative.begin()->c_str(), L"presets") == 0;
            if (userFile && fs::exists(target))
                continue;
            fs::create_directories(target.parent_path());
            if (!userFile)
                installed.insert(relative.wstring());
            fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing);
        }
        // An add-on that is not selected is removed. One that is selected but could not be downloaded keeps the
        // files it had.
        if (options.reshade)
            for (const auto& addon : kAddons)
                if (!Selected(options, addon.addon))
                    for (const wchar_t* file : addon.files)
                        RemoveFile(directory / file);
        // Repairs the search paths in a ReShade.ini written by an earlier version of Setup.
        if (options.reshade && hadReShadeIni)
            FixReShadeIni(directory / L"ReShade.ini", false);

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
    progress.Fraction(1.0f);
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

ReShadeRelease FetchReShadeRelease(const std::atomic<bool>& cancel)
{
    ReShadeRelease release;
    try
    {
        // The download button on reshade.me links ReShade_Setup_<version>_Addon.exe of the newest version.
        const std::string page = Fetch(L"https://reshade.me/", cancel, kListLimit);
        const size_t end = page.find("_Addon.exe");
        const size_t start = end == std::string::npos || end == 0 ? std::string::npos : page.rfind('_', end - 1);
        if (start == std::string::npos)
            throw std::runtime_error("Could not find ReShade's download on reshade.me.");
        release.version = page.substr(start + 1, end - start - 1);
        // The version becomes part of the download and license addresses.
        if (!IsVersion(release.version))
            throw std::runtime_error("reshade.me lists an unexpected ReShade version.");
    }
    catch (const std::runtime_error& error)
    {
        // reshade.me's pages break at times while its downloads keep working. Every release is also a tag on GitHub.
        SetupLog(std::string(error.what()) + " Looking for the newest ReShade on GitHub instead.");
        release.version = NewestReShadeTag(cancel);
    }
    release.license = Fetch(L"https://raw.githubusercontent.com/crosire/reshade/v" + Wide(release.version) + L"/LICENSE.md", cancel, kListLimit);
    if (release.license.find("Redistribution and use") == std::string::npos)
        throw std::runtime_error("Could not load the ReShade license.");
    SetupLog("Newest ReShade: " + release.version);
    return release;
}

bool Install(const InstallOptions& options, const ReShadeRelease& release, Progress& progress)
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
    WriteFile(files / L"UnishadeUi.dll", Resource(IDR_UI));
    WriteFile(files / L"LICENSE", Resource(IDR_LICENSE));
    WriteFile(files / L"CREDITS.txt", Resource(IDR_CREDITS));
    bool complete = true;
    if (options.reshade)
    {
        InstallReShade(work, files, release, options.presets, progress);
        if (!InstallEffects(work, files, progress))
            complete = false;
        if (options.presets && !InstallPresets(work, files, progress))
            complete = false;
        for (const auto& addon : kAddons)
            if (Selected(options, addon.addon) && !DownloadAddon(work, files, addon, progress))
                complete = false;
    }
    Commit(files, options, progress);
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

bool AddonInstalled(const fs::path& directory, Addon addon)
{
    std::error_code ignored;
    for (const auto& info : kAddons)
        if (info.addon == addon)
            for (const wchar_t* file : info.files)
                if (fs::exists(directory / file, ignored))
                    return true;
    return false;
}

std::wstring ReadShortcut(const fs::path& directory, const wchar_t* name, const wchar_t* fallback)
{
    wchar_t value[128]{};
    GetPrivateProfileStringW(L"Input", name, fallback, value, static_cast<DWORD>(std::size(value)), (directory / L"RobloxShadeHost.ini").c_str());
    return value;
}

bool WriteShortcut(const fs::path& directory, const wchar_t* name, const std::wstring& value)
{
    return WritePrivateProfileStringW(L"Input", name, value.c_str(), (directory / L"RobloxShadeHost.ini").c_str()) != FALSE;
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
