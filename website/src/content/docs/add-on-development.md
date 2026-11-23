---
title: Make an add-on
description: How to write a Unishade add-on, from its addon.ini to settings, controls and the game's picture.
order: 7
---

An add-on is a folder with a small text file and a native library. It can work on the game's picture before or after effects, draw over the game, change effects, react to what Unishade does, and put its own settings and controls in the menu.

The interface is one C header, [unishade_addon.h](links:addonHeader). Its comments are the reference. This page is the short tour. The fastest start is to copy the [example add-on](links:addonExample), which is about 150 lines of C and needs no graphics code.

## The folder

Put a folder in the Addons folder, which **Add-ons > Open folder** opens. It needs an `addon.ini`:

```ini
[Addon]
Id=someone.my-addon
Name=My add-on
Version=1.2.0
Author=Someone
Description=What it does, in a sentence.
Icon=icon.png
Api=1

[windows-x64]
Library=MyAddon.dll

[macos]
Library=MyAddon.dylib

[linux-x64]
Library=MyAddon.so
```

- `Id` names the add-on in presets and in Unishade's settings. Use lowercase letters, digits, dots, hyphens and underscores, and never change it.
- `Api` is the version of the header you built against.
- Each system the add-on runs on has a section with its library. `windows-x64`, `macos-arm64` and the like name one processor. `windows`, `macos` or `linux` alone is for a library that runs on all of them.
- `Icon` is a PNG in the folder. Without one, the Add-ons page shows the start of the name.

Unishade reads this file without running anything, so the Add-ons page can show your add-on's name, version and author before it asks the user whether to load it. It asks once and remembers the answer for the `Id`, so a rebuilt library loads without asking again.

## The library

The library exports one function, which says what the add-on does and what to call:

```c
#include "unishade_addon.h"

static void* Create(const UnishadeHost* host, char* error, size_t errorSize)
{
    host->log(host, UNISHADE_LOG_INFO, "Started.");
    return (void*)host;
}

static void Destroy(void* addon) {}

static const UnishadeAddon kAddon = {
    .size = sizeof(UnishadeAddon),
    .api = UNISHADE_API_VERSION,
    .create = Create,
    .destroy = Destroy,
};

UNISHADE_ADDON_EXPORT const UnishadeAddon* UnishadeGetAddon(void)
{
    return &kAddon;
}
```

Unishade calls every function on its main thread. Leave out the functions you don't need.

| Function | When Unishade calls it |
| --- | --- |
| `create`, `destroy` | The add-on starts and stops. |
| `update` | Once per pass of the main loop, with the settings' values. |
| `frame` | For every frame, if the add-on asked for the picture. |
| `event` | A game started or stopped, a preset loaded, the menu opened, a screenshot was saved. |
| `ui` | For every frame the add-on's own controls show. |
| `draw` | For every frame Unishade shows, to draw over the game. |

## Settings

List your settings and Unishade shows them in the menu and saves them. An add-on that writes the picture gets a tile next to the effects, and each preset keeps its settings. Any other add-on's settings are saved for the computer.

```c
enum { kStrength, kTint };

static const UnishadeSetting kSettings[] = {
    { .key = "Strength", .label = "Strength", .type = UNISHADE_SETTING_SLIDER, .defaultValue = { 0.5f }, .max = 1 },
    { .key = "Tint", .label = "Tint", .type = UNISHADE_SETTING_COLOR_RGB, .defaultValue = { 1, 1, 1 } },
};
```

The values arrive with every `update` and `frame`, four floats for each setting in the order you listed them. `settings[kStrength][0]` is the slider and `settings[kTint][0]` to `[2]` are the color.

## Controls of your own

Settings are fixed. For text that changes, progress, or a value the add-on keeps itself, add a `ui` function. Unishade calls it while the controls show, and draws what you ask for in its own style:

```c
static void Ui(void* addon, const UnishadeUi* ui)
{
    My* my = addon;
    ui->text(ui, my->status);
    ui->progress(ui, "Loading", my->loaded);
    if (ui->button(ui, "Start again"))
        Restart(my);
}
```

A control's function returns true once after the user used it. The controls show under the add-on's settings, in **More > Add-ons** of the menu over the game.

## Drawing over the game

A `draw` function draws lines, rectangles, circles and text on top of the game, for things like a crosshair or a frame rate. It needs no graphics code:

```c
static void Draw(void* addon, const UnishadeDraw* draw)
{
    const float x = draw->width / 2, y = draw->height / 2, arm = 10 * draw->scale;
    const uint32_t white = UNISHADE_COLOR(255, 255, 255, 255);
    draw->line(draw, x - arm, y, x + arm, y, white, 2 * draw->scale);
    draw->line(draw, x, y - arm, x, y + arm, white, 2 * draw->scale);
}
```

Positions are in pixels of the game's picture, from its top left corner. Multiply sizes by `scale` and they match Unishade's menu on any display. The drawing goes under the menu, hides while effects are off, and stays out of screenshots.

## Talking to Unishade

`UnishadeHost` has the functions an add-on can call:

- `getState` says whether a game is being played, its name and picture size, the frame rate, the preset, and whether effects and the menu are on.
- `showMessage`, `setEffectsOn` and `takeScreenshot` do what the user can do.
- `presetCount`, `getPreset` and `usePreset` list and switch presets.
- `findTechnique`, `setTechnique`, `findUniform`, `getValue` and `setValue` turn effects on and off and change their values, as the menu does.
- `keyDown` reads the keyboard and mouse buttons, for an add-on that sets `UNISHADE_READS_KEYS`.

## The picture

An add-on asks for frames with its flags. `UNISHADE_READS_PICTURE` and `UNISHADE_WRITES_PICTURE` are for the game's picture before effects, `UNISHADE_READS_RESULT` and `UNISHADE_WRITES_RESULT` for the result after them, and `UNISHADE_WRITES_DEPTH` and `UNISHADE_READS_DEPTH` for depth.

Frames stay on the graphics card. Unishade shares them as Direct3D 11 textures on Windows, IOSurfaces on macOS and dma-bufs on Linux, and the add-on opens them with a device of its own. This is the hard part of an add-on, and the rules differ on each system. The header's comment under **Frames** has them. Depth and DLSS5 in Unishade's source are working code for Windows and macOS.

## Later versions

Every struct that can get more members starts with its size, and new members only go at the end. An add-on built today keeps working in a later Unishade. If you use something newer than version 1 and want to run in an older Unishade, check with `UNISHADE_HAS` first.

An add-on runs as part of Unishade, with the same access. A crash in it is a crash in Unishade.
