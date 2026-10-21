#include "discord.h"
#include "config.h"
#include "discord_rpc.h"
#include "game_integration.h"
#include "log.h"
#include "menu.h"
#include "net.h"
#include "state.h"
#include "text.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace
{
using Clock = std::chrono::steady_clock;

// Unishade's application in Discord's developer portal. Discord shows its name above the game.
constexpr char kClientId[] = "1555600775462396117";
// Discord's list of the games it detects, with their icons. It is over 12 MB, and over 2 MB compressed, so only the
// icons are kept, beside the exe, and the list is downloaded again once they are kIconsAge old.
constexpr wchar_t kGameList[] = L"https://discord.com/api/v9/applications/detectable";
constexpr uint64_t kGameListLimit = 64ull << 20;
constexpr wchar_t kIconsFile[] = L"DiscordIcons.txt";
constexpr auto kIconsAge = std::chrono::hours(24 * 7);
// While Discord is not running, it is looked for again this often.
constexpr auto kRetry = std::chrono::seconds(15);
// An open connection is read this often, and more often until Discord accepts it, which it does within kHandshakeLimit.
constexpr auto kPoll = std::chrono::milliseconds(1000);
constexpr auto kHandshakePoll = std::chrono::milliseconds(50);
constexpr auto kHandshakeLimit = std::chrono::seconds(10);

// Never destroyed, since the thread runs until the host exits.
struct Shared
{
    std::mutex mutex;
    std::condition_variable wake;
    std::optional<discord::Activity> wanted;
    bool changed = false;
    bool started = false;
    DiscordStatus status;
    std::atomic<unsigned> version = 0;
};
Shared& shared = *new Shared;

// What the host asked for last, on the host's thread.
struct Host
{
    HWND target = nullptr;
    std::string executable;
    int64_t start = 0;
    std::optional<discord::Activity> wanted;
};
Host host;

// What the thread keeps: the connection to the Discord app and the games' icons.
struct Worker
{
    HANDLE pipe = INVALID_HANDLE_VALUE;
    std::string received;
    Clock::time_point opened;
    bool ready = false; // Discord accepted the handshake
    std::optional<discord::Activity> shown;
    unsigned nonce = 0;
    Clock::time_point retry;
    // Loaded the first time Discord shows a game. Empty when that failed.
    std::optional<std::vector<discord::GameIcon>> icons;

    bool Connected() const
    {
        return pipe != INVALID_HANDLE_VALUE;
    }

    // Discord clears the activity when the connection closes.
    void Close()
    {
        if (pipe != INVALID_HANDLE_VALUE)
            CloseHandle(pipe);
        pipe = INVALID_HANDLE_VALUE;
        received.clear();
        ready = false;
        shown.reset();
    }
};

// Returns whether the status changed.
bool SetStatus(DiscordState state, std::wstring error = {})
{
    std::lock_guard lock(shared.mutex);
    if (shared.status.state == state && shared.status.error == error)
        return false;
    shared.status = { state, std::move(error) };
    ++shared.version;
    return true;
}

// The Discord app's end of the connection: discord-ipc-0, or up to 9 when several copies of Discord run, such as PTB
// beside the regular one.
HANDLE OpenPipe()
{
    for (int i = 0; i < 10; ++i)
    {
        const std::wstring name = L"\\\\.\\pipe\\discord-ipc-" + std::to_wstring(i);
        // Identification only, so whatever holds the pipe cannot act as the user.
        const HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                        SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (pipe != INVALID_HANDLE_VALUE)
            return pipe;
    }
    return INVALID_HANDLE_VALUE;
}

bool Send(HANDLE pipe, uint32_t opcode, std::string_view json)
{
    const std::string frame = discord::Frame(opcode, json);
    DWORD written = 0;
    return WriteFile(pipe, frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr) && written == frame.size();
}

// Reads what Discord sent, without waiting for more, and answers its pings. Returns false once the connection is gone,
// with why Discord closed it in error when it said. Throws std::runtime_error for a broken frame.
bool Receive(Worker& worker, std::string& error)
{
    for (;;)
    {
        DWORD available = 0;
        if (!PeekNamedPipe(worker.pipe, nullptr, 0, nullptr, &available, nullptr))
            return false;
        if (!available)
            return true;
        const size_t had = worker.received.size();
        worker.received.resize(had + available);
        DWORD read = 0;
        if (!ReadFile(worker.pipe, worker.received.data() + had, available, &read, nullptr))
            return false;
        worker.received.resize(had + read);

        discord::Message message;
        while (discord::TakeFrame(worker.received, message))
        {
            if (message.opcode == discord::kPing && !Send(worker.pipe, discord::kPong, message.json))
                return false;
            if (message.opcode == discord::kClose)
            {
                error = discord::FindString(message.json, "message");
                return false;
            }
            if (message.opcode != discord::kFrame)
                continue;
            const std::string event = discord::FindString(message.json, "evt");
            if (event == "READY")
                worker.ready = true;
            else if (event == "ERROR")
            {
                const std::wstring reason = Wide(discord::FindString(message.json, "message"));
                if (SetStatus(DiscordState::Refused, reason))
                    Log(LogLevel::Info, L"Discord refused the activity: %ls", reason.c_str());
            }
        }
    }
}

// Closes the connection, and looks for Discord again after kRetry.
void Lose(Worker& worker, const std::string& error)
{
    worker.Close();
    worker.retry = Clock::now() + kRetry;
    if (!error.empty())
    {
        if (SetStatus(DiscordState::Refused, Wide(error)))
            Log(LogLevel::Info, L"Discord closed the connection: %ls", Wide(error).c_str());
    }
    else if (SetStatus(DiscordState::Closed))
        Log(LogLevel::Info, L"Discord is not running.");
}

// Through a temporary file, so a failed write leaves the old icons.
void SaveIcons(const fs::path& path, const std::vector<discord::GameIcon>& icons)
{
    const fs::path temporary = path.wstring() + L".tmp";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file << discord::FormatGameIcons(icons);
    file.close();
    if (!file || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        Log(LogLevel::Info, L"Could not save the games' icons beside Unishade.exe.");
    }
}

// The games' icons kept beside the exe, until they are kIconsAge old. Then they come from Discord, or stay the old
// ones when Discord cannot be reached.
std::vector<discord::GameIcon> LoadIcons()
{
    const fs::path path = ExeDirectory() + kIconsFile;
    std::vector<discord::GameIcon> icons;
    std::error_code error;
    const fs::file_time_type written = fs::last_write_time(path, error);
    if (!error)
    {
        std::ifstream file(path, std::ios::binary);
        icons = discord::ReadGameIcons(std::string(std::istreambuf_iterator<char>(file), {}));
        if (!icons.empty() && fs::file_time_type::clock::now() - written < kIconsAge)
            return icons;
    }
    try
    {
        const std::atomic<bool> cancel = false;
        std::vector<discord::GameIcon> downloaded = discord::ParseGameIcons(Fetch(kGameList, cancel, kGameListLimit));
        if (downloaded.empty())
            throw std::runtime_error("the list could not be read");
        SaveIcons(path, downloaded);
        return downloaded;
    }
    catch (const std::exception& e)
    {
        Log(LogLevel::Info, L"Could not get the games' icons from Discord: %hs", e.what());
        return icons;
    }
}

// Brings Discord in line with wanted, which the host may change meanwhile.
void Step(Worker& worker, std::optional<discord::Activity>& wanted)
{
    if (!wanted)
    {
        worker.Close();
        worker.retry = {};
        SetStatus(DiscordState::Idle);
        return;
    }
    if (!worker.Connected())
    {
        if (Clock::now() < worker.retry)
            return;
        worker.pipe = OpenPipe();
        if (!worker.Connected() || !Send(worker.pipe, discord::kHandshake, discord::HandshakeJson(kClientId)))
        {
            Lose(worker, {});
            return;
        }
        worker.opened = Clock::now();
        SetStatus(DiscordState::Connecting);
    }
    std::string error;
    if (!Receive(worker, error))
    {
        Lose(worker, error);
        return;
    }
    if (!worker.ready)
    {
        if (Clock::now() - worker.opened > kHandshakeLimit)
        {
            Log(LogLevel::Info, L"Discord did not answer.");
            Lose(worker, {});
        }
        return;
    }
    if (wanted == worker.shown)
        return;

    if (!worker.icons)
    {
        worker.icons = LoadIcons();
        // A download takes a moment, in which the host may have asked for something else.
        {
            std::lock_guard lock(shared.mutex);
            wanted = shared.wanted;
        }
        Step(worker, wanted);
        return;
    }

    const std::string icon = discord::FindGameIcon(*worker.icons, wanted->executable);
    if (!Send(worker.pipe, discord::kFrame, discord::SetActivityJson(GetCurrentProcessId(), *wanted, icon, ++worker.nonce)))
    {
        Lose(worker, {});
        return;
    }
    if (!worker.shown || worker.shown->game != wanted->game)
        Log(LogLevel::Info, L"Showing %ls on Discord%ls.", Wide(wanted->game).c_str(), icon.empty() ? L", without an icon" : L"");
    worker.shown = wanted;
    SetStatus(DiscordState::Showing);
}

void Run()
{
    InitThreadLog();
    Worker worker;
    std::optional<discord::Activity> wanted;
    for (;;)
    {
        {
            std::unique_lock lock(shared.mutex);
            const auto changed = [] { return shared.changed; };
            if (worker.Connected())
                shared.wake.wait_for(lock, worker.ready ? kPoll : kHandshakePoll, changed);
            else if (wanted)
                shared.wake.wait_until(lock, worker.retry, changed);
            else
                shared.wake.wait(lock, changed);
            shared.changed = false;
            wanted = shared.wanted;
        }
        try
        {
            Step(worker, wanted);
        }
        catch (const std::exception& e)
        {
            Log(LogLevel::Info, L"Lost the connection to Discord: %hs", e.what());
            Lose(worker, {});
        }
    }
}
} // namespace

void UpdateDiscord()
{
    if (g.target != host.target)
    {
        host.target = g.target;
        host.start = std::time(nullptr);
        host.executable.clear();
        try
        {
            if (g.target)
                host.executable = Utf8(ProcessExecutable(g.activeGame->processId).wstring());
        }
        catch (const std::system_error&)
        {
            // Without the path, Discord shows Unishade's logo instead of the game's icon.
        }
    }
    std::optional<discord::Activity> wanted;
    if (g.target && DiscordPresenceEnabled())
        wanted = discord::Activity{ Utf8(g.activeGame->name), Utf8(ActivePresetName()), host.executable, host.start };
    if (wanted == host.wanted)
        return;
    host.wanted = wanted;

    std::lock_guard lock(shared.mutex);
    shared.wanted = std::move(wanted);
    shared.changed = true;
    if (!shared.started)
    {
        shared.started = true;
        std::thread(Run).detach();
    }
    shared.wake.notify_one();
}

DiscordStatus CurrentDiscordStatus()
{
    std::lock_guard lock(shared.mutex);
    return shared.status;
}

unsigned DiscordStatusVersion()
{
    return shared.version;
}
