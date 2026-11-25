#include "net/update.h"
#include "app/log.h"
#include "base/text.h"
#include "net/net.h"
#include "net/releases.h"
#include "net/version.h"

#include <windows.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <winver.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
// Never destroyed, since the check's thread may still run while the host exits.
struct Shared
{
    std::mutex mutex;
    Update available;
    std::string tag;        // the update's release
    std::wstring publisher; // who signed this Unishade, and so has to have signed the update's Setup. Empty for a beta
    UpdateInstall install;
    std::atomic<bool> cancel = false;
};
Shared& shared = *new Shared;

namespace fs = std::filesystem;

constexpr char kReleases[] = "https://github.com/OMouta/Unishade/releases/";

fs::path Self()
{
    std::wstring path(32768, L'\0');
    path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
    return path;
}

// Where an update's Setup is downloaded to: a folder of its own in the user's temporary folder, which the next check
// for updates deletes. Empty when the temporary folder cannot be found.
fs::path UpdateFolder()
{
    std::error_code error;
    const fs::path temporary = fs::temp_directory_path(error);
    return error ? fs::path() : temporary / L"Unishade-Update";
}

// The subject of the certificate a file is signed with, in full, or empty unless Windows trusts the signature. With
// file, the open file, what is checked is what was opened. Without, Windows opens it. When the revocation servers
// cannot be reached, the check is repeated without them, as for Setup's downloads. A certificate known as revoked
// still fails.
std::wstring Signer(const fs::path& path, HANDLE file)
{
    for (const DWORD revocation : { DWORD{ WTD_REVOKE_WHOLECHAIN }, DWORD{ WTD_REVOKE_NONE } })
    {
        WINTRUST_FILE_INFO info{ sizeof(info) };
        info.pcwszFilePath = path.c_str();
        info.hFile = file;
        WINTRUST_DATA data{ sizeof(data) };
        data.dwUIChoice = WTD_UI_NONE;
        data.fdwRevocationChecks = revocation;
        data.dwUnionChoice = WTD_CHOICE_FILE;
        data.pFile = &info;
        data.dwStateAction = WTD_STATEACTION_VERIFY;
        data.dwProvFlags = revocation == WTD_REVOKE_NONE ? 0 : WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;
        GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
        const LONG result = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
        std::wstring subject;
        if (result == ERROR_SUCCESS)
        {
            CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(data.hWVTStateData);
            CRYPT_PROVIDER_SGNR* signer = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
            CRYPT_PROVIDER_CERT* certificate = signer ? WTHelperGetProvCertFromChain(signer, 0) : nullptr;
            if (certificate && certificate->pCert)
            {
                CERT_NAME_BLOB& name = certificate->pCert->pCertInfo->Subject;
                // The size counts the zero at the end.
                subject.resize(CertNameToStrW(X509_ASN_ENCODING, &name, CERT_X500_NAME_STR, nullptr, 0));
                subject.resize(CertNameToStrW(X509_ASN_ENCODING, &name, CERT_X500_NAME_STR, subject.data(), static_cast<DWORD>(subject.size())) - 1);
            }
        }
        data.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
        const bool unreachable = result == CERT_E_REVOCATION_FAILURE || result == CRYPT_E_REVOCATION_OFFLINE || result == CRYPT_E_NO_REVOCATION_CHECK;
        if (!unreachable)
            return subject;
    }
    return {};
}

// A file opened for reading that nothing can write to or delete until the handle is closed.
std::unique_ptr<void, decltype(&CloseHandle)> OpenUnchanging(const fs::path& path)
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    return { file == INVALID_HANDLE_VALUE ? nullptr : file, &CloseHandle };
}

// What a file's version resource says under a name such as ProductVersion, in the one language Unishade's files have.
std::string VersionText(const fs::path& path, const wchar_t* name)
{
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    std::vector<char> info(size);
    const std::wstring key = std::wstring(L"\\StringFileInfo\\040904b0\\") + name;
    void* text = nullptr;
    UINT length = 0;
    if (!size || !GetFileVersionInfoW(path.c_str(), 0, size, info.data()) || !VerQueryValueW(info.data(), key.c_str(), &text, &length) || !text || !length)
        return {};
    return Utf8(static_cast<const wchar_t*>(text));
}

// In quotes, for a command line, where backslashes before a quote would take the quote for text. So the ones at the
// end are doubled.
std::wstring Quote(const fs::path& path)
{
    std::wstring text = path.wstring();
    const size_t last = text.find_last_not_of(L'\\');
    text.append(text.size() - (last == std::wstring::npos ? 0 : last + 1), L'\\');
    return L"\"" + text + L"\"";
}

void SetStage(UpdateStage stage, std::string error = {})
{
    std::lock_guard lock(shared.mutex);
    shared.install = { stage, 0, std::move(error) };
}

// Downloads the Setup of the update's release, checks it, and runs it. Setup has to be the file the release's
// SHA256SUMS.txt lists, and Unishade's Setup of that version. A version's Setup is signed, by the publisher of this
// Unishade when this one is signed too. A beta's is not signed, so for a beta the release on GitHub is all there is
// to go by, as when it is downloaded by hand.
void Install(const Update& update, const std::string& tag, const std::wstring& publisher)
{
    const fs::path folder = UpdateFolder();
    std::error_code error;
    fs::remove_all(folder, error);
    fs::create_directories(folder, error);
    if (folder.empty() || error)
        throw std::runtime_error("Could not create a folder for the download in the temporary folder.");
    const std::string name = SetupName(tag);
    const std::string files = kReleases + ("download/" + tag + "/");
    const std::string sha256 = ListedSha256(Fetch(Wide(files + "SHA256SUMS.txt"), shared.cancel), name);
    if (sha256.empty())
        throw std::runtime_error("The release does not list the SHA-256 of its Setup, so it was not downloaded.");
    const fs::path setup = folder / name;
    Log(LogLevel::Info, "Downloading %s%s", files.c_str(), name.c_str());
    Download(Wide(files + name), setup, sha256, kPackageLimit, shared.cancel, [](uint64_t received, uint64_t total) {
        std::lock_guard lock(shared.mutex);
        shared.install.fraction = total ? static_cast<float>(static_cast<double>(received) / static_cast<double>(total)) : 0;
    });

    // Open until Setup has started, so the file that runs is the one that was checked.
    auto file = OpenUnchanging(setup);
    if (!file)
        throw std::runtime_error("Could not open the downloaded Setup.");
    const std::optional<Version> version = ParseVersion(VersionText(setup, L"ProductVersion"));
    if (VersionText(setup, L"ProductName") != "Unishade" || !version || version != ParseVersion(update.version))
        throw std::runtime_error("The downloaded Setup is not Unishade " + update.version + ", so it was not run.");
    if (const std::wstring signer = Signer(setup, file.get()); publisher.empty() ? !version->beta && signer.empty() : signer != publisher)
        throw std::runtime_error(publisher.empty() ? "The downloaded Setup is not signed, as a version's is, so it was not run."
                                                   : "The downloaded Setup is not signed by Unishade's publisher, so it was not run.");
    if (shared.cancel)
        throw Cancelled{};

    // Setup closes this Unishade, puts the new one in its folder and starts it. Effects and presets stay as they are.
    // A folder without Setup's copy was installed with --portable, and stays so.
    const fs::path directory = Self().parent_path();
    std::wstring command = Quote(setup) + L" --silent --launch --components host --dir " + Quote(directory);
    if (!fs::exists(directory / L"Unishade-Setup.exe", error))
        command += L" --portable";
    SetStage(UpdateStage::Installing);
    Log(LogLevel::Info, "Starting %s", Utf8(command).c_str());
    STARTUPINFOW startup{ sizeof(startup) };
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(setup.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, folder.c_str(), &startup, &process))
        throw std::runtime_error("Could not start the downloaded Setup.");
    CloseHandle(process.hThread);
    file.reset();
    // Unishade is closed before its file is replaced. Still being here when Setup has ended means it did not get that
    // far.
    WaitForSingleObject(process.hProcess, INFINITE);
    CloseHandle(process.hProcess);
    throw std::runtime_error("Setup could not install the update. Unishade-Setup.log in " + folder.parent_path().string() + " says why.");
}

void Check()
{
    // The Setup the last update left behind.
    if (const fs::path folder = UpdateFolder(); !folder.empty() && InstallingUpdate().stage == UpdateStage::None)
    {
        std::error_code ignored;
        fs::remove_all(folder, ignored);
    }

    const std::atomic<bool> cancel = false;
    const std::string json = Fetch(L"https://api.github.com/repos/OMouta/Unishade/releases?per_page=30", cancel);
    const std::string tag = NewestRelease(json, ParseVersion(UNISHADE_VERSION).value_or(Version{}));
    if (tag.empty())
    {
        Log(LogLevel::Info, "Unishade is up to date.");
        return;
    }
    const std::string version = ParseVersion(tag.substr(1)).value_or(Version{}).Text();
    // Unishade installs the update itself when its own file is signed, as a released version's is: the Setup it
    // downloads has to be signed by the same publisher. A beta is not signed and installs updates all the same. Any
    // other build without a signature, such as one built from the source, sends the user to the release's page.
    const std::wstring publisher = Signer(Self(), nullptr);
#ifdef UNISHADE_BETA
    const bool installable = true;
#else
    const bool installable = !publisher.empty();
#endif
    Log(LogLevel::Info, "Unishade %s is available. %s", version.c_str(),
        installable ? "This build can install it itself." : "This build is not signed, so it cannot install it itself.");
    std::lock_guard lock(shared.mutex);
    shared.available = { version, kReleases + ("tag/" + tag), installable };
    shared.tag = tag;
    shared.publisher = publisher;
}
} // namespace

void CheckForUpdate()
{
    std::thread([] {
        try
        {
            Check();
        }
        catch (const std::exception& e)
        {
            Log(LogLevel::Info, "Could not check for updates: %s", e.what());
        }
    }).detach();
}

Update AvailableUpdate()
{
    std::lock_guard lock(shared.mutex);
    return shared.available;
}

void InstallUpdate()
{
    Update update;
    std::string tag;
    std::wstring publisher;
    {
        std::lock_guard lock(shared.mutex);
        if (!shared.available.installable || shared.install.stage == UpdateStage::Downloading || shared.install.stage == UpdateStage::Installing)
            return;
        update = shared.available;
        tag = shared.tag;
        publisher = shared.publisher;
        shared.install = { UpdateStage::Downloading };
        shared.cancel = false;
    }
    std::thread([update, tag, publisher] {
        try
        {
            Install(update, tag, publisher);
        }
        catch (const Cancelled&)
        {
            SetStage(UpdateStage::None);
        }
        catch (const std::exception& e)
        {
            Log(LogLevel::Warning, "Could not install Unishade %s: %s", update.version.c_str(), e.what());
            SetStage(UpdateStage::Failed, e.what());
        }
    }).detach();
}

void CancelUpdate()
{
    shared.cancel = true;
}

UpdateInstall InstallingUpdate()
{
    std::lock_guard lock(shared.mutex);
    return shared.install;
}

std::string NotFromPublisher(const fs::path& file, bool beta)
{
    const std::wstring publisher = Signer(Self(), nullptr);
    const std::wstring signer = Signer(file, nullptr);
    if (publisher.empty() ? beta || !signer.empty() : signer == publisher)
        return {};
    return publisher.empty() ? "is not signed, as the files of a version are" : "is not signed by Unishade's publisher";
}
