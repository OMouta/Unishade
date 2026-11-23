// Unishade Setup: installs Unishade, which then downloads its effects and presets, and later updates or uninstalls it.
// Drawn with Dear ImGui, as Unishade is.

#define IMGUI_DEFINE_MATH_OPERATORS
#include "install.h"
#include "resource.h"
#include "base/text.h"
#include "ui/kit.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cmath>
#include <functional>
#include <initializer_list>
#include <thread>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

// The launcher's font, which the build puts into the program.
extern const unsigned char kBricolageRegular[];
extern const unsigned kBricolageRegularSize;
extern const unsigned char kBricolageSemiBold[];
extern const unsigned kBricolageSemiBoldSize;
extern const unsigned char kBricolageExtraBold[];
extern const unsigned kBricolageExtraBoldSize;

namespace
{
using namespace kit;
using namespace kit::spectrum;

// Layout in pixels at 100% scaling.
constexpr float kWindowWidth = 900;
constexpr float kWindowHeight = 640;
constexpr float kSidebar = 236;
constexpr float kMargin = 12; // the black around the block
constexpr float kBlockRounding = 35.2f;
constexpr float kPad = 36;
constexpr float kRowRounding = 17.6f;
constexpr float kButton = 41.6f;
constexpr float kFooterPad = 24;
constexpr float kFooter = kFooterPad + kButton + 12; // the buttons at the bottom of the block
// Exit codes of a silent run. 1 is also used when the command line is invalid.
constexpr int kExitFailed = 1;
// Installed, but an effect package, a preset or the requested add-on was left out. The setup log says which.
constexpr int kExitIncomplete = 2;

enum class Page
{
    Welcome,
    Manage,
    Installing,
    Failed,
    Finished,
    Uninstall,
    Uninstalled,
};

// Runs work on its own thread. The window calls Collect every frame to learn when it has finished.
class Task
{
public:
    ~Task() { Wait(); }

    void Start(std::function<void()> work)
    {
        Wait();
        finished = false;
        error.clear();
        cancelled = false;
        thread = std::thread([this, work = std::move(work)] {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            try
            {
                work();
            }
            catch (const Cancelled&)
            {
                cancelled = true;
            }
            catch (const std::exception& e)
            {
                error = e.what();
            }
            CoUninitialize();
            finished = true;
        });
    }

    bool Running() const { return thread.joinable(); }

    // Returns true once, after the work has finished. error and cancelled are valid from then on.
    bool Collect()
    {
        if (!thread.joinable() || !finished)
            return false;
        thread.join();
        return true;
    }

    void Wait()
    {
        if (thread.joinable())
            thread.join();
    }

    std::string error;
    bool cancelled = false;

private:
    std::thread thread;
    std::atomic<bool> finished = true;
};

struct Arguments
{
    bool silent = false;
    bool uninstall = false;
    bool deleteUserFiles = false;
    bool portable = false;
    bool launch = false;
    std::wstring directory;
    std::wstring components;
    std::wstring log;
    std::string error;
};

struct App
{
    Page page = Page::Welcome;
    std::optional<Installation> installation;
    char directory[1024]{};
    std::string directoryError;

    Task installTask;
    std::unique_ptr<Progress> progress;
    std::vector<std::string> notes;
    bool closeRequested = false;

    bool hostRunning = false;
    bool launch = true;
    bool highPerformanceGpu = true;

    Task uninstallTask;
    bool deleteUserFiles = false;
    bool folderLeft = false;
};
App app;

struct Ui
{
    HWND window = nullptr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swapchain;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11ShaderResourceView> logo;
    UINT resizeWidth = 0;
    UINT resizeHeight = 0;
};
Ui ui;

fs::path Directory()
{
    return fs::path(Wide(app.directory)).lexically_normal();
}

fs::path DefaultDirectory()
{
    PWSTR path = nullptr;
    SHGetKnownFolderPath(FOLDERID_UserProgramFiles, 0, nullptr, &path);
    fs::path directory = fs::path(path ? path : L"") / L"Unishade";
    CoTaskMemFree(path);
    return directory;
}

void StartInstall()
{
    InstallOptions options;
    options.directory = Directory();
    app.progress = std::make_unique<Progress>();
    app.notes.clear();
    app.installTask.Start([options] { Install(options, *app.progress); });
    app.page = Page::Installing;
}

void StartUninstall()
{
    app.uninstallTask.Start([directory = Directory(), deleteUserFiles = app.deleteUserFiles] { Uninstall(directory, deleteUserFiles); });
}

// Returns why the folder cannot be used, or an empty string.
std::string CheckDirectory(const fs::path& directory)
{
    if (!directory.is_absolute() || !directory.has_filename())
        return "Enter a full folder path, such as C:\\Games\\Unishade.";
    std::error_code ignored;
    const std::wstring lower = [&] {
        std::wstring text = directory.wstring();
        CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
        return text;
    }();
    if (fs::exists(directory / L"RobloxPlayerBeta.exe", ignored) || lower.find(L"\\roblox\\versions") != std::wstring::npos)
        return "That is Roblox's folder, which Roblox replaces when it updates. Pick another folder.";

    // Tries the nearest folder that exists, so nothing is created before installing.
    fs::path existing = directory;
    while (!fs::is_directory(existing, ignored) && existing.has_relative_path())
        existing = existing.parent_path();
    const fs::path probe = existing / (L"Unishade-" + std::to_wstring(GetCurrentProcessId()) + L".tmp");
    HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return "Setup cannot write to this folder. Pick another one, such as the suggested folder.";
    CloseHandle(file);
    return {};
}

std::optional<fs::path> BrowseFolder(const fs::path& current)
{
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return std::nullopt;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    std::error_code ignored;
    fs::path existing = current;
    while (!existing.empty() && !fs::is_directory(existing, ignored) && existing.has_relative_path())
        existing = existing.parent_path();
    ComPtr<IShellItem> start;
    if (!existing.empty() && SUCCEEDED(SHCreateItemFromParsingName(existing.c_str(), nullptr, IID_PPV_ARGS(&start))))
        dialog->SetFolder(start.Get());

    ComPtr<IShellItem> result;
    PWSTR path = nullptr;
    if (FAILED(dialog->Show(ui.window)) || FAILED(dialog->GetResult(&result)) || FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return std::nullopt;
    fs::path folder = path;
    CoTaskMemFree(path);
    // Keeps the files together instead of spreading them over the chosen folder.
    if (_wcsicmp(folder.filename().c_str(), L"Unishade") != 0 && _wcsicmp(folder.filename().c_str(), L"RobloxShadeHost") != 0)
        folder /= L"Unishade";
    return folder;
}

void OpenFile(const fs::path& path)
{
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Drawing helpers, on the launcher's kit: its colors, its type and its controls.

void Spacing(float height)
{
    ImGui::Dummy(ImVec2(0, S(height)));
}

void Title(const std::string& text)
{
    Text(text, kInk, kTextTitle, 0.0f, true);
    Spacing(4);
}

// A grey row with a title and a line under it, and an arrow at its right. Returns true when clicked.
bool ActionRow(const char* id, const char* title, const std::string& description)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x, pad = S(19.2f);
    const float right = min.x + width - pad - S(36);
    const float titled = Measure(title, kTextBody, true).y;
    const float described = Measure(description, kTextSmall, false, right - min.x - pad).y;
    const ImVec2 size(width, pad * 2 + titled + S(4) + described);
    const bool clicked = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    HandOnHover();
    draw->AddRectFilled(min, min + size, hovered ? kWellHover : kWell, S(kRowRounding));
    Line(draw, min.x + pad, right, min.y + pad + titled / 2, title, kInk, kTextBody, true);
    Paragraph(draw, ImVec2(min.x + pad, min.y + pad + titled + S(4)), right, description, kMist, kTextSmall);
    DrawIcon(draw, Icon::Right, ImVec2(min.x + width - pad - S(8), min.y + size.y / 2), S(17.6f), hovered ? kInk : kMist);
    return clicked;
}

// A grey row with a switch at its right.
void SwitchRow(const char* label, bool& value)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x, height = S(52.8f);
    const float left = min.x + width - S(11.2f) - S(kSwitchWidth);
    draw->AddRectFilled(min, min + ImVec2(width, height), kWell, S(kRowRounding));
    Line(draw, min.x + S(19.2f), left - S(12), min.y + height / 2, label, kInk, kTextBody);
    ImGui::PushID(label);
    if (SwitchButton("switch", ImVec2(left, min.y + (height - S(kSwitchHeight)) / 2), value))
        value = !value;
    ImGui::PopID();
    ImGui::SetCursorScreenPos(min);
    ImGui::Dummy(ImVec2(width, height));
}

// The block's buttons, from its right corner leftwards.
struct Footer
{
    float right;
    float top;

    bool Button(const char* label, PillStyle style, bool enabled = true)
    {
        right -= PillWidth(label, style);
        const bool clicked = PillButton(label, ImVec2(right, top), S(kButton), style, enabled);
        right -= S(8.8f);
        return clicked;
    }
};

bool Link(const char* label, ImU32 color, float size)
{
    PushSize(size);
    ImGui::PushStyleColor(ImGuiCol_TextLink, color);
    const bool clicked = ImGui::TextLink(label);
    HandOnHover();
    ImGui::PopStyleColor();
    ImGui::PopFont();
    return clicked;
}

void Spinner(float radius)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 center = ImGui::GetCursorScreenPos() + ImVec2(radius, radius);
    const float start = static_cast<float>(ImGui::GetTime()) * 5.0f;
    draw->PathArcTo(center, radius - S(2), start, start + 4.2f, 32);
    draw->PathStroke(kInk, S(3));
    ImGui::Dummy(ImVec2(radius * 2, radius * 2));
}

// The ring's colors, as far as the installation is.
void ProgressLine(float fraction)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max = min + ImVec2(ImGui::GetContentRegionAvail().x, S(8));
    draw->AddRectFilled(min, max, kWell, S(4));
    RainbowLine(draw, min, max, std::clamp(Animate(ImGui::GetID("progress"), fraction, 0.2f, Curve::Linear), 0.0f, 1.0f), 1);
    ImGui::Dummy(max - min);
}

// On the black of the window, beside the block: the logo, the steps and the links.
void Sidebar(std::initializer_list<const char*> steps, int current)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetWindowPos();
    const float height = ImGui::GetWindowSize().y;
    const float x = origin.x + S(30), right = origin.x + S(kSidebar) - S(12);
    float y = origin.y + S(30);
    if (ui.logo)
        draw->AddImage(ImTextureID(reinterpret_cast<uintptr_t>(ui.logo.Get())), ImVec2(x - S(8), y), ImVec2(x - S(8) + S(80), y + S(80)));
    y += S(104);
    Line(draw, x, right, y, "Unishade", kText, kTextName, true);
    y += S(26);
    Line(draw, x, right, y, "Setup " UNISHADE_VERSION, kDim, kTextSmall);
    y += S(52);

    int index = 0;
    for (const char* step : steps)
    {
        const ImVec2 dot(x + S(7), y);
        if (index + 1 < static_cast<int>(steps.size()))
            draw->AddLine(dot + ImVec2(0, S(10)), dot + ImVec2(0, S(26)), kBorderStrong, S(1.5f));
        if (index < current)
            DrawIcon(draw, Icon::Check, dot, S(14), kDim);
        else if (index == current)
            draw->AddCircleFilled(dot, S(5), kText);
        else
            draw->AddCircle(dot, S(5), kBorderStrong, 0, S(1.5f));
        Line(draw, x + S(24), right, y, step, index == current ? kText : kDim, kTextBody, index == current);
        y += S(36);
        ++index;
    }

    ImGui::SetCursorPos(ImVec2(S(30), height - S(74)));
    if (Link("Privacy policy", kDim, kTextSmall))
        ShellExecuteW(nullptr, L"open", L"https://unishade.me/privacy/", nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::SetCursorPos(ImVec2(S(30), height - S(48)));
    if (Link("Credits and licenses", kDim, kTextSmall))
        ImGui::OpenPopup("Credits");
}

void CreditsPopup()
{
    const ImVec2 window = ImGui::GetWindowSize();
    const float pad = S(28);
    ImGui::SetNextWindowPos(ImGui::GetWindowPos() + window / 2, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(S(640), S(480)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, S(24)));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, S(kBlockRounding));
    if (ImGui::BeginPopupModal("Credits", nullptr,
                               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar))
    {
        Text("Credits and licenses", kInk, kTextName, 0.0f, true);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(19.2f), S(16)));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(kRowRounding));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kWell);
        ImGui::BeginChild("text", ImVec2(0, ImGui::GetContentRegionAvail().y - S(kButton) - ImGui::GetStyle().ItemSpacing.y),
                          ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        Text(std::string(Resource(IDR_CREDITS)), kMist, kTextSmall);
        ImGui::EndChild();
        const ImVec2 close(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - pad - PillWidth("Close", PillStyle::Black), ImGui::GetCursorScreenPos().y);
        if (PillButton("Close", close, S(kButton), PillStyle::Black))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}

// Pages. Each draws its content. DrawUi draws their buttons.

void WelcomePage()
{
    Title("Install Unishade");
    Text("Universal post-processing without injection.", kInk, kTextBody);
    Text("Your frame rate will be lower while it runs.", kMist, kTextSmall);
    Spacing(18);
    Text("Install folder", kInk, kTextHeading, 0.0f, true);
    const ImVec2 field = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x, browse = PillWidth("Browse", PillStyle::Well);
    PushSize(kTextBody);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(16), (S(kButton) - ImGui::GetFontSize()) / 2));
    ImGui::SetNextItemWidth(width - browse - S(8.8f));
    if (ImGui::InputText("##directory", app.directory, sizeof(app.directory), ImGuiInputTextFlags_ElideLeft))
        app.directoryError.clear();
    ImGui::PopStyleVar();
    ImGui::PopFont();
    if (PillButton("Browse", ImVec2(field.x + width - browse, field.y), S(kButton), PillStyle::Well))
        if (auto folder = BrowseFolder(Directory()))
        {
            strncpy_s(app.directory, Utf8(folder->wstring()).c_str(), _TRUNCATE);
            app.directoryError.clear();
        }
    if (!app.directoryError.empty())
        Text(app.directoryError, kDanger, kTextSmall);
}

void ManagePage()
{
    Title("Unishade is installed");
    std::string version = app.installation && !app.installation->version.empty() ? "Version " + app.installation->version : "Unishade";
    Text(version + " is installed in " + app.directory + ".", kMist, kTextSmall);
    if (app.installation && app.installation->version != UNISHADE_VERSION)
        Text("This setup installs version " UNISHADE_VERSION ".", kMist, kTextSmall);
    Spacing(14);
    if (ActionRow("update", "Update", "Install Unishade " UNISHADE_VERSION " and get the newest effects. Your settings and presets stay."))
        StartInstall();
    if (ActionRow("uninstall", "Uninstall", "Remove Unishade and the effects from this PC."))
        app.page = Page::Uninstall;
}

void InstallingPage()
{
    const Progress::State state = app.progress->Read();
    Title(app.progress->cancel ? "Cancelling..." : "Installing");
    Spacing(4);
    Text(state.status.empty() ? "Starting..." : state.status, kInk, kTextBody, 0.0f, true);
    ProgressLine(state.fraction);
    Text(state.detail.empty() ? " " : state.detail, kMist, kTextSmall);
    Spacing(8);
    ImGui::BeginChild("history", ImVec2(0, 0));
    for (size_t i = 0; i + 1 < state.history.size(); ++i)
        Text(state.history[i], kMist, kTextSmall);
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

void FailedPage()
{
    const bool cancelled = app.installTask.cancelled;
    Title(cancelled ? "Installation cancelled" : "Setup could not finish");
    if (!cancelled)
        Text(app.installTask.error, kDanger, kTextBody);
    Spacing(6);
    Text("Go back to try again.", kMist, kTextBody);
    Spacing(6);
    if (Link("Open the setup log", kInk, kTextBody))
        OpenFile(SetupLogPath());
}

void FinishedPage()
{
    Title("Unishade is ready");
    Text("Start Unishade and open your game. Add games in the Unishade window.", kMist, kTextBody);
    for (const auto& note : app.notes)
    {
        Spacing(4);
        Text(note, kCaution, kTextSmall);
    }
    Spacing(14);
    SwitchRow("Run Unishade on the high-performance GPU", app.highPerformanceGpu);
    if (!app.hostRunning)
        SwitchRow("Start Unishade now", app.launch);
}

void UninstallPage()
{
    Title("Uninstall Unishade");
    Text("Removes Unishade and the effects from " + std::string(app.directory) + ", and its Start menu shortcuts.", kMist, kTextBody);
    Spacing(10);
    SwitchRow("Also delete my presets, settings, game list and add-ons", app.deleteUserFiles);
    Text("That deletes Unishade.ini and games.ini, the settings of Unishade 0.6.3 and earlier, and the presets, reshade-shaders and "
         "Addons folders with everything in them. Other files in the folder, such as screenshots, stay.",
         kMist, kTextSmall);
    if (app.uninstallTask.Running())
    {
        Spacing(10);
        Spinner(S(12));
    }
}

void UninstalledPage()
{
    if (app.uninstallTask.error.empty())
    {
        Title("Unishade was uninstalled");
        if (!app.deleteUserFiles)
            Text("Your presets and settings are still in " + std::string(app.directory) + ".", kMist, kTextBody);
        else if (app.folderLeft)
            Text("Files Setup did not create, such as screenshots, are still in " + std::string(app.directory) + ".", kMist, kTextBody);
    }
    else
    {
        Title("Uninstall could not finish");
        Text(app.uninstallTask.error, kDanger, kTextBody);
    }
}

void CollectTasks()
{
    if (app.installTask.Collect())
    {
        if (!app.installTask.error.empty() || app.installTask.cancelled)
            app.page = Page::Failed;
        else
        {
            app.notes = app.progress->Read().notes;
            app.installation = FindInstallation();
            app.hostRunning = HostRunning(Directory());
            // A different preference picked in Windows' settings stays unless the box is ticked.
            const int preference = GpuPreference(Directory());
            app.highPerformanceGpu = preference < 0 || preference == 2;
            app.page = Page::Finished;
        }
        if (app.closeRequested)
            DestroyWindow(ui.window);
    }
    if (app.uninstallTask.Collect())
    {
        std::error_code ignored;
        app.folderLeft = fs::exists(Directory(), ignored);
        app.page = Page::Uninstalled;
    }
}

void DrawUi()
{
    CollectTasks();

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::Begin("Setup", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();

    const Page page = app.page;
    if (page == Page::Uninstall || page == Page::Uninstalled)
        Sidebar({ "Uninstall", "Done" }, page == Page::Uninstall ? 0 : 1);
    else
    {
        int step = 0;
        switch (page)
        {
        case Page::Installing:
        case Page::Failed: step = 1; break;
        case Page::Finished: step = 2; break;
        default: break;
        }
        Sidebar({ "Welcome", "Install", "Done" }, step);
    }

    // The page in a block, as the launcher's are, with its buttons in the block's bottom right corner.
    const ImVec2 blockMin = origin + ImVec2(S(kSidebar), S(kMargin)), blockMax = origin + size - ImVec2(S(kMargin), S(kMargin));
    draw->AddRectFilled(blockMin, blockMax, kPaper, S(kBlockRounding));
    draw->AddRect(blockMin + ImVec2(0.5f, 0.5f), blockMax - ImVec2(0.5f, 0.5f), kEdge, S(kBlockRounding), 1.0f);
    ImGui::SetCursorScreenPos(blockMin);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(kPad), S(kPad)));
    ImGui::BeginChild("content", ImVec2(blockMax.x - blockMin.x, blockMax.y - blockMin.y - S(kFooter)), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    switch (page)
    {
    case Page::Welcome: WelcomePage(); break;
    case Page::Manage: ManagePage(); break;
    case Page::Installing: InstallingPage(); break;
    case Page::Failed: FailedPage(); break;
    case Page::Finished: FinishedPage(); break;
    case Page::Uninstall: UninstallPage(); break;
    case Page::Uninstalled: UninstalledPage(); break;
    }
    ImGui::EndChild();

    Footer footer{ blockMax.x - S(kFooterPad), blockMax.y - S(kFooterPad) - S(kButton) };
    switch (page)
    {
    case Page::Welcome:
        if (footer.Button("Install", PillStyle::Black))
        {
            app.directoryError = CheckDirectory(Directory());
            if (app.directoryError.empty())
                StartInstall();
        }
        break;
    case Page::Manage:
        if (footer.Button("Close", PillStyle::Well))
            DestroyWindow(ui.window);
        break;
    case Page::Installing:
        if (footer.Button("Cancel", PillStyle::Well, !app.progress->cancel))
            app.progress->cancel = true;
        break;
    case Page::Failed:
        if (footer.Button("Back", PillStyle::Black))
            app.page = app.installation ? Page::Manage : Page::Welcome;
        if (footer.Button("Close", PillStyle::Well))
            DestroyWindow(ui.window);
        break;
    case Page::Finished:
        if (footer.Button("Finish", PillStyle::Black))
        {
            // Before starting Unishade, which picks its GPU when it starts.
            if (app.highPerformanceGpu)
                SetHighPerformanceGpu(Directory());
            if (app.launch && !app.hostRunning)
                LaunchHost(Directory());
            DestroyWindow(ui.window);
        }
        break;
    case Page::Uninstall:
        if (footer.Button("Uninstall", PillStyle::Black, !app.uninstallTask.Running()))
            StartUninstall();
        if (footer.Button(app.installation ? "Back" : "Cancel", PillStyle::Well, !app.uninstallTask.Running()))
        {
            if (app.installation)
                app.page = Page::Manage;
            else
                DestroyWindow(ui.window);
        }
        break;
    case Page::Uninstalled:
        if (footer.Button("Close", PillStyle::Black))
            DestroyWindow(ui.window);
        break;
    }

    CreditsPopup();
    ImGui::End();
}

ComPtr<ID3D11ShaderResourceView> LoadLogo(UINT size)
{
    const std::string_view png = Resource(IDR_LOGO);
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> premultiplied;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> straight;
    std::vector<BYTE> pixels(size * size * 4);
    // Scaling with premultiplied alpha keeps the transparent edge from darkening.
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(png.data())), static_cast<DWORD>(png.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&premultiplied)) ||
        FAILED(premultiplied->Initialize(frame.Get(), GUID_WICPixelFormat32bppPRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) ||
        FAILED(factory->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(premultiplied.Get(), size, size, WICBitmapInterpolationModeHighQualityCubic)) ||
        FAILED(factory->CreateFormatConverter(&straight)) ||
        FAILED(straight->Initialize(scaler.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) ||
        FAILED(straight->CopyPixels(nullptr, size * 4, static_cast<UINT>(pixels.size()), pixels.data())))
        return nullptr;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = size;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data{ pixels.data(), size * 4, 0 };
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    if (FAILED(ui.device->CreateTexture2D(&desc, &data, &texture)) || FAILED(ui.device->CreateShaderResourceView(texture.Get(), nullptr, &view)))
        return nullptr;
    return view;
}

void ApplyScale(float windowScale)
{
    kit::scale = windowScale;
    // The launcher's dark theme, which Unishade starts with.
    SetDark(true);
    ImGuiStyle style;
    spectrum::ApplyStyle(style);
    style.WindowRounding = 0;
    style.WindowBorderSize = 0;
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImGui::ColorConvertU32ToFloat4(IM_COL32(0, 0, 0, 150));
    ImGui::GetStyle() = style;

    if (ui.device)
        ui.logo = LoadLogo(static_cast<UINT>(std::lround(S(80))));
}

void CreateRenderTarget()
{
    ComPtr<ID3D11Texture2D> buffer;
    ui.swapchain->GetBuffer(0, IID_PPV_ARGS(&buffer));
    ui.device->CreateRenderTargetView(buffer.Get(), nullptr, &ui.target);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, message, wParam, lParam))
        return 1;

    switch (message)
    {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
        {
            ui.resizeWidth = LOWORD(lParam);
            ui.resizeHeight = HIWORD(lParam);
        }
        return 0;
    case WM_DPICHANGED:
    {
        ApplyScale(HIWORD(wParam) / 96.0f);
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SYSCOMMAND:
        // Alt alone would otherwise open the window menu.
        if ((wParam & 0xFFF0) == SC_KEYMENU)
            return 0;
        break;
    case WM_CLOSE:
        // Closing during installation cancels it first. The window closes once the download stops.
        if (app.installTask.Running())
        {
            app.progress->cancel = true;
            app.closeRequested = true;
            return 0;
        }
        if (app.uninstallTask.Running())
            return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool CreateDeviceAndSwapchain()
{
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    // WARP draws on the CPU, for machines without a usable GPU driver.
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &ui.device, nullptr, &ui.context)) &&
        FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &ui.device, nullptr, &ui.context)))
        return false;
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (FAILED(ui.device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateSwapChainForHwnd(ui.device.Get(), ui.window, &desc, nullptr, nullptr, &ui.swapchain)))
        return false;
    factory->MakeWindowAssociation(ui.window, DXGI_MWA_NO_ALT_ENTER);
    CreateRenderTarget();
    return true;
}

// Bricolage Grotesque, as in Unishade, then Segoe UI for what it lacks, such as a folder named in Cyrillic.
void AddFonts()
{
    wchar_t windows[MAX_PATH]{};
    GetWindowsDirectoryW(windows, MAX_PATH);
    const std::string fonts = Utf8(windows) + "\\Fonts\\";
    LoadFonts({ .data = { kBricolageRegular, kBricolageRegularSize } }, { { .path = fonts + "segoeui.ttf" } },
              { .data = { kBricolageSemiBold, kBricolageSemiBoldSize } }, { { .path = fonts + "seguisb.ttf" } },
              { .data = { kBricolageExtraBold, kBricolageExtraBoldSize } });
}


int RunWindow(const Arguments& arguments)
{
    app.installation = FindInstallation();
    const fs::path directory = !arguments.directory.empty() ? fs::absolute(arguments.directory)
                               : app.installation        ? app.installation->directory
                                                         : DefaultDirectory();
    strncpy_s(app.directory, Utf8(directory.wstring()).c_str(), _TRUNCATE);
    if (arguments.uninstall)
        app.page = Page::Uninstall;
    else
        app.page = app.installation ? Page::Manage : Page::Welcome;

    WNDCLASSEXW windowClass{ sizeof(windowClass) };
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hIcon = LoadIconW(windowClass.hInstance, MAKEINTRESOURCEW(1));
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = CreateSolidBrush(RGB((theme::kBackground >> 16) & 0xFF, (theme::kBackground >> 8) & 0xFF, theme::kBackground & 0xFF));
    windowClass.lpszClassName = L"UnishadeSetup";
    RegisterClassExW(&windowClass);
    constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    ui.window = CreateWindowExW(0, windowClass.lpszClassName, L"Unishade Setup", kStyle, CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr,
                                nullptr, windowClass.hInstance, nullptr);
    if (!ui.window)
        return kExitFailed;
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(ui.window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const COLORREF caption = RGB((theme::kSidebar >> 16) & 0xFF, (theme::kSidebar >> 8) & 0xFF, theme::kSidebar & 0xFF);
    DwmSetWindowAttribute(ui.window, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));

    // Sizes the window for its monitor's scaling and centers it.
    const UINT dpi = GetDpiForWindow(ui.window);
    RECT frame{ 0, 0, static_cast<LONG>(std::lround(kWindowWidth * dpi / 96.0f)), static_cast<LONG>(std::lround(kWindowHeight * dpi / 96.0f)) };
    AdjustWindowRectExForDpi(&frame, kStyle, FALSE, 0, dpi);
    MONITORINFO monitor{ sizeof(monitor) };
    GetMonitorInfoW(MonitorFromWindow(ui.window, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;
    SetWindowPos(ui.window, nullptr, (monitor.rcWork.left + monitor.rcWork.right - width) / 2, (monitor.rcWork.top + monitor.rcWork.bottom - height) / 2,
                 width, height, SWP_NOZORDER | SWP_NOACTIVATE);

    if (!CreateDeviceAndSwapchain())
    {
        MessageBoxW(ui.window, L"Setup could not start its window because DirectX 11 is unavailable.", L"Unishade Setup", MB_ICONERROR);
        return kExitFailed;
    }
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().LogFilename = nullptr;
    ImGui_ImplWin32_Init(ui.window);
    ImGui_ImplDX11_Init(ui.device.Get(), ui.context.Get());
    AddFonts();
    ApplyScale(dpi / 96.0f);
    ShowWindow(ui.window, SW_SHOWNORMAL);

    // Draws a few frames after each message, so hover and click states settle, then waits for input.
    int framesLeft = 3;
    for (bool running = true; running;)
    {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            running &= message.message != WM_QUIT;
            framesLeft = 3;
        }
        if (!running)
            break;
        if (IsIconic(ui.window))
        {
            WaitMessage();
            continue;
        }
        if (ui.resizeWidth)
        {
            ui.target.Reset();
            ui.swapchain->ResizeBuffers(0, ui.resizeWidth, ui.resizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            ui.resizeWidth = ui.resizeHeight = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawUi();
        ImGui::Render();
        const float clear[] = { 0, 0, 0, 1 };
        ID3D11RenderTargetView* targets[] = { ui.target.Get() };
        ui.context->OMSetRenderTargets(1, targets, nullptr);
        ui.context->ClearRenderTargetView(ui.target.Get(), clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        ui.swapchain->Present(1, 0);

        const bool animating = app.installTask.Running() || app.uninstallTask.Running() || Animating();
        if (!animating && --framesLeft <= 0)
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 500, QS_ALLINPUT);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    return 0;
}

int RunSilent(const Arguments& arguments)
{
    InstallOptions options;
    // An update Unishade started itself has Setup start it again. A failed installation leaves the folder as it was,
    // so then the version before starts, which says that the update did not go through.
    struct Launch
    {
        const fs::path& directory;
        bool wanted;
        ~Launch()
        {
            if (wanted && !directory.empty())
                LaunchHost(directory);
        }
    } launch{ options.directory, arguments.launch && !arguments.uninstall };
    try
    {
        const auto installation = FindInstallation();
        options.directory = !arguments.directory.empty() ? fs::absolute(arguments.directory).lexically_normal()
                            : installation               ? installation->directory
                                                         : DefaultDirectory();
        if (arguments.uninstall)
        {
            Uninstall(options.directory, arguments.deleteUserFiles);
            return 0;
        }

        options.portable = arguments.portable;
        options.effects = false;
        const std::wstring components = arguments.components.empty() ? L"effects" : arguments.components;
        for (size_t start = 0; start <= components.size();)
        {
            size_t end = components.find(L',', start);
            end = end == std::wstring::npos ? components.size() : end;
            std::wstring component = components.substr(start, end - start);
            start = end + 1;
            CharLowerBuffW(component.data(), static_cast<DWORD>(component.size()));
            if (component == L"effects")
                options.effects = true;
            else if (component != L"host" && !component.empty())
                throw std::runtime_error("Unknown component: " + Utf8(component) + ". Use host or effects.");
        }

        Progress progress;
        return Install(options, progress) ? 0 : kExitIncomplete;
    }
    catch (const Cancelled&)
    {
        SetupLog("Cancelled.");
    }
    catch (const std::exception& e)
    {
        SetupLog(std::string("Error: ") + e.what());
    }
    return kExitFailed;
}

Arguments ParseArguments()
{
    Arguments arguments;
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count);
    for (int i = 1; i < count; ++i)
    {
        const std::wstring argument = argv[i];
        const auto value = [&](std::wstring& target) {
            if (i + 1 < count)
                target = argv[++i];
            else
                arguments.error = Utf8(argument) + " needs a value.";
        };
        if (argument == L"--silent")
            arguments.silent = true;
        else if (argument == L"--uninstall")
            arguments.uninstall = true;
        else if (argument == L"--delete-user-files")
            arguments.deleteUserFiles = true;
        else if (argument == L"--portable")
            arguments.portable = true;
        else if (argument == L"--launch")
            arguments.launch = true;
        else if (argument == L"--dir")
            value(arguments.directory);
        else if (argument == L"--components")
            value(arguments.components);
        else if (argument == L"--log")
            value(arguments.log);
        else if (argument == L"--effects-url")
            value(sources.effects);
        else if (argument == L"--presets-url")
            value(sources.presets);
        else
            arguments.error = "Unknown option: " + Utf8(argument);
    }
    LocalFree(argv);
    return arguments;
}
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    // Setup's copy lives in the installation folder, where Unishade 0.6.3 and earlier had ReShade as dxgi.dll, and
    // Windows looks in the exe's folder first. From here on, DLLs loaded by name, including the delay-loaded imports,
    // come from System32 only. dxgi.dll and d3d11.dll are loaded right away, so later loads by name get these copies.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const Arguments arguments = ParseArguments();
    OpenSetupLog(!arguments.log.empty() ? fs::absolute(arguments.log) : fs::temp_directory_path() / L"Unishade-Setup.log");
    SetupLog("Unishade Setup " UNISHADE_VERSION);
    if (!arguments.error.empty())
    {
        SetupLog(arguments.error);
        if (!arguments.silent)
            MessageBoxW(nullptr, Wide(arguments.error).c_str(), L"Unishade Setup", MB_ICONERROR);
        return kExitFailed;
    }
    const int result = arguments.silent ? RunSilent(arguments) : RunWindow(arguments);
    DeleteMovedSetup();
    return result;
}
