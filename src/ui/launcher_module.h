#pragma once

// UnishadeUi.dll draws the launcher's window on Windows. It has a Dear ImGui of its own: in the host, ImGui's functions
// are ReShade's, which only draw inside ReShade's overlay. It draws on the CPU, since ReShade puts its effects on every
// Direct3D window in the host. The host owns the window and hands it messages, the model and the pictures.
//
// The host and the DLL each have their own C runtime, so nothing allocated by one is freed by the other: the DLL only
// reads the model, and the host fills the DLL's buffers.

#include "launcher_ui.h"

#include <windows.h>

#include <cstdint>
#include <string_view>

#ifdef UNISHADE_UI_EXPORTS
#define UNISHADE_UI_API extern "C" __declspec(dllexport)
#else
#define UNISHADE_UI_API extern "C" __declspec(dllimport)
#endif

// The launcher's pictures, which the host knows how to find.
class LauncherPictures
{
public:
    // Fills rgba with the picture, size by size pixels of red, green, blue and alpha. Returns false when there is none.
    virtual bool Pixels(std::string_view key, int size, uint8_t* rgba) = 0;

protected:
    ~LauncherPictures() = default;
};

class LauncherUi
{
public:
    // Loads the fonts and starts Dear ImGui's input for the window. Returns false when it cannot.
    virtual bool Init(HWND window) = 0;
    virtual void Shutdown() = 0;
    // Gives one of the window's messages to Dear ImGui. Returns true when it took the message, such as WM_SETCURSOR.
    virtual bool Message(HWND window, UINT message, WPARAM wParam, LPARAM lParam) = 0;
    // Draws the launcher into pixels, width by height, top row first, as 0x00RRGGBB. scale is the window's scaling.
    // Returns how many milliseconds until it wants to draw again, or 0 when only input or another model changes it.
    virtual unsigned Draw(const launcher::Model& model, launcher::Host& host, LauncherPictures& pictures, float scale, uint32_t* pixels, int width,
                          int height) = 0;

protected:
    ~LauncherUi() = default;
};

UNISHADE_UI_API LauncherUi* UnishadeLauncherUi();
