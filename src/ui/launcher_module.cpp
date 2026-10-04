// UnishadeUi.dll. See launcher_module.h.

#include "launcher_module.h"
#include "kit.h"
#include "soft_renderer.h"

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <string_view>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

namespace
{
struct Picture
{
    ImTextureID texture = ImTextureID_Invalid;
    ImU32 tint = 0;
};

class Module final : public LauncherUi
{
public:
    bool Init(HWND window) override
    {
        context = ImGui::CreateContext();
        ImGui::SetCurrentContext(context);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        // Tab and the arrows move through the controls, as in the menu.
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        if (!ImGui_ImplWin32_Init(window))
            return false;
        soft::Init();

        LoadFonts();
        return true;
    }

    void Shutdown() override
    {
        if (!context)
            return;
        ImGui::SetCurrentContext(context);
        ForgetPictures();
        soft::Shutdown();
        ImGui_ImplWin32_Shutdown();
        kit::ForgetContext(context);
        ImGui::DestroyContext(context);
        context = nullptr;
    }

    bool Message(HWND window, UINT message, WPARAM wParam, LPARAM lParam) override
    {
        if (!context)
            return false;
        ImGui::SetCurrentContext(context);
        return ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam) != 0;
    }

    unsigned Draw(const launcher::Model& model, launcher::Host& host, LauncherPictures& source, float scale, uint32_t* pixels, int width,
                  int height) override
    {
        ImGui::SetCurrentContext(context);
        if (model.pictures != picturesVersion)
        {
            ForgetPictures();
            picturesVersion = model.pictures;
        }
        current = this;
        sourceNow = &source;
        ImGui_ImplWin32_NewFrame();
        ImGui::GetIO().DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
        ImGui::NewFrame();
        launcher::Draw(model, host, scale);
        ImGui::Render();
        soft::Render(ImGui::GetDrawData(), pixels, width, height);
        sourceNow = nullptr;

        // A control held down or a text box being typed in, and a tip waiting to show, need frames of their own.
        const ImGuiContext& g = *context;
        if (ImGui::IsAnyItemActive() && !ImGui::GetIO().WantTextInput)
            return 16;
        if (g.HoveredId && g.HoveredIdTimer < 1.0f)
            return 50;
        if (ImGui::GetIO().WantTextInput)
            return 250;
        return 0;
    }

    ImTextureID PictureTexture(const std::string& key, float drawn)
    {
        const int size = static_cast<int>(std::lround(drawn));
        const auto [entry, added] = pictures.try_emplace({ key, size });
        if (!added || size <= 0 || !sourceNow)
            return entry->second.texture;
        std::vector<uint8_t> rgba(static_cast<size_t>(size) * size * 4);
        if (!sourceNow->Pixels(key, size, rgba.data()))
            return ImTextureID_Invalid;
        entry->second.texture = soft::CreateTexture(rgba.data(), size, size);
        // The average of the pixels that show, weighted by how much they show.
        uint64_t sums[3] = {}, weight = 0;
        for (size_t i = 0; i < rgba.size(); i += 4)
        {
            for (int c = 0; c < 3; ++c)
                sums[c] += uint64_t(rgba[i + c]) * rgba[i + 3];
            weight += rgba[i + 3];
        }
        if (weight)
            entry->second.tint = IM_COL32(sums[0] / weight, sums[1] / weight, sums[2] / weight, 255);
        return entry->second.texture;
    }

    ImU32 PictureTint(const std::string& key) const
    {
        const auto found = pictures.lower_bound({ key, 0 });
        return found != pictures.end() && found->first.first == key ? found->second.tint : 0;
    }

    static inline Module* current = nullptr;

private:
    // Segoe UI, like the menu, with Windows' fonts for what it lacks: symbols, Chinese, Japanese, Korean, Thai, Indian
    // scripts and emoji, which show without color. Chinese, Japanese and Korean draw the same characters differently,
    // so the user's own language comes first.
    static void LoadFonts()
    {
        wchar_t windows[MAX_PATH]{};
        GetWindowsDirectoryW(windows, MAX_PATH);
        std::string folder(static_cast<size_t>(WideCharToMultiByte(CP_UTF8, 0, windows, -1, nullptr, 0, nullptr, nullptr)), '\0');
        WideCharToMultiByte(CP_UTF8, 0, windows, -1, folder.data(), static_cast<int>(folder.size()), nullptr, nullptr);
        folder.resize(folder.size() - 1);
        folder += "\\Fonts\\";

        struct Fallback
        {
            const char* regular;
            const char* bold;
        };
        std::vector<Fallback> cjk = { { "msyh.ttc", "msyhbd.ttc" }, { "YuGothM.ttc", "YuGothB.ttc" }, { "malgun.ttf", "malgunbd.ttf" }, { "msjh.ttc", "msjhbd.ttc" } };
        wchar_t language[LOCALE_NAME_MAX_LENGTH]{};
        LCIDToLocaleName(GetUserDefaultUILanguage(), language, LOCALE_NAME_MAX_LENGTH, 0);
        const std::wstring_view name = language;
        const size_t first = name.starts_with(L"ja") ? 1
                           : name.starts_with(L"ko") ? 2
                           : name.starts_with(L"zh-TW") || name.starts_with(L"zh-HK") || name.starts_with(L"zh-MO") || name.starts_with(L"zh-Hant") ? 3
                                                                                                                                                         : 0;
        std::rotate(cjk.begin(), cjk.begin() + static_cast<std::ptrdiff_t>(first), cjk.begin() + static_cast<std::ptrdiff_t>(first) + 1);

        std::vector<Fallback> all = { { "seguisym.ttf", "seguisym.ttf" } };
        all.insert(all.end(), cjk.begin(), cjk.end());
        // Windows 11 has Nirmala UI as a collection, Windows 10 as a single font.
        all.insert(all.end(), { { "LeelawUI.ttf", "LeelaUIb.ttf" }, { "Nirmala.ttc", "Nirmala.ttc" }, { "Nirmala.ttf", "NirmalaB.ttf" }, { "seguiemj.ttf", "seguiemj.ttf" } });
        std::vector<kit::FontFile> regular, bold;
        for (const Fallback& fallback : all)
        {
            regular.push_back({ folder + fallback.regular });
            bold.push_back({ folder + fallback.bold });
        }
        kit::LoadFonts({ folder + "segoeui.ttf" }, regular, { folder + "seguisb.ttf" }, bold);
    }

    void ForgetPictures()
    {
        for (auto& [key, picture] : pictures)
            if (picture.texture != ImTextureID_Invalid)
                soft::DestroyTexture(picture.texture);
        pictures.clear();
    }

    ImGuiContext* context = nullptr;
    LauncherPictures* sourceNow = nullptr;
    // By key and size in pixels. A picture without pixels keeps an empty entry until the model's pictures change.
    std::map<std::pair<std::string, int>, Picture> pictures;
    unsigned picturesVersion = 0;
};

Module module;
} // namespace

namespace kit
{
ImTextureID Picture(const std::string& key, float size)
{
    return Module::current ? Module::current->PictureTexture(key, size) : ImTextureID_Invalid;
}

ImU32 PictureTint(const std::string& key)
{
    return Module::current ? Module::current->PictureTint(key) : 0;
}
} // namespace kit

UNISHADE_UI_API LauncherUi* UnishadeLauncherUi()
{
    return &module;
}
