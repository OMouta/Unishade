# Unishade for macOS and Linux

ReShade is Windows only, so this host runs ReShade effects itself. It follows the Windows design: it copies the game's window, runs effects on the copy and shows the result in a window over the game, with clicks passing through until the menu opens.

| | macOS | Linux |
| --- | --- | --- |
| Capture | ScreenCaptureKit | XComposite, with XDamage for when the game drew (X11 and XWayland) |
| Windows and processes | CoreGraphics, `proc_pidpath` | `_NET_CLIENT_LIST`, XRes, `/proc` |
| Shortcuts | Carbon hot keys | `XGrabKey` on the root window |
| Keys and buttons for effects | `CGEventSourceKeyState` | `XQueryKeymap`, `XQueryPointer` |
| Vulkan | MoltenVK, linked directly and copied into the app | The system's loader and driver |
| Frames on the graphics card | IOSurfaces as Vulkan images (`VK_EXT_metal_objects`) | DRI3 dma-bufs as Vulkan images (`VK_EXT_image_drm_format_modifier`) |

Frames stay on the graphics card where the system allows it, and are copied through memory otherwise: on Linux without DRI3 1.2 (no GPU, some NVIDIA setups on Xorg, remote X), without the Vulkan extensions above, or when the driver cannot import the X server's buffer layout. Copies go through shared memory (XShm), or without it where the X server cannot reach Unishade's memory, such as over the network. The log says which one runs. `--render ... --gpu-source` exercises the graphics-card path of the effect runtime without a window.

On Linux, frames are copied when XDamage says the game drew, at most about once per refresh of the monitor the game is on. Without XDamage they are copied once per refresh. While the overlay is hidden, because another window is in front, a few frames a second are copied. Frames handed over as dma-bufs are not synchronised with the game's drawing, so one can now and then show a picture still being drawn.

Under a Wayland desktop only games that run through XWayland have a window Unishade can see. When a saved game runs without one, the launcher says so and how to switch the game to XWayland, such as `SDL_VIDEODRIVER=x11` or turning off Wayland in Wine, Proton or the game's launcher.

Effects compile with ReShade's own compiler (`effect_*.cpp` from ReShade 6.8.0, BSD-3-Clause) to SPIR-V, the same path ReShade takes in Vulkan games. `effects.cpp` runs what it produces: textures, render targets, stencil tests, storage, compute passes, blending, mipmaps and the uniforms ReShade sets itself, such as `timer` and `frametime`. Presets are ReShade's `.ini` files. The launcher and the menu use Dear ImGui with GLFW.

Compiled effects are kept in `cache` in the data folder, each effect's SPIR-V in `cache/effects` and Vulkan's pipeline cache in `cache/pipelines.bin`, so effects start faster the next time. Beyond 256 MB the entries used longest ago are dropped.

## Shortcuts, screenshots and input

Shortcuts are written as on Windows, such as `Ctrl+F9`, including the number pad keys `Numpad0` to `Numpad9`, `NumpadMultiply`, `NumpadAdd`, `NumpadSubtract`, `NumpadDecimal` and `NumpadDivide`. macOS has function keys up to F20.

Screenshots go to `Unishade` in the pictures folder: `~/Pictures` on macOS, and on Linux the one `XDG_PICTURES_DIR` names in the environment or in `~/.config/user-dirs.dirs`, which is not always `~/Pictures`. Files are named after the game, with anything that does not belong in a file name left out.

Effects that read the keyboard and mouse, through uniforms such as `key` and `mousebutton`, see Windows virtual-key codes, as on Windows. Unishade reads them for the whole system, so only while the game or the menu is in front. The mouse wheel only reaches effects while the menu is open, and X11 cannot tell the back and forward mouse buttons.

Technique shortcuts work as in ReShade: a preset's `Key` entries, or an effect's `toggle` annotations, turn a technique on or off. Like the keys effects read, they work while the game or the menu is in front, and not while typing in the menu.

## Building

Linux (Debian and Ubuntu package names):

```sh
sudo apt install cmake g++ libvulkan-dev libx11-dev libxcomposite-dev libxext-dev libxres-dev libxdamage-dev libxcb-dri3-dev \
    libx11-xcb-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

macOS:

```sh
brew install cmake molten-vk vulkan-headers
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel
```

The Linux build is `build/unishade`. The macOS build is `build/Unishade.app`, with MoltenVK inside and an ad-hoc signature. XRes, XDamage and DRI3 are loaded when the system has them, so only their headers are needed to build; without a header the build leaves that part out.

## Checking effects without a game

`unishade --render in.png preset.ini out.png` applies a preset to an image with the overlay's own code and no window. It exits with 1 when a technique the preset turns on is missing or fails to start. CI renders the repository's presets this way on Mesa's software Vulkan driver.

Set `UNISHADE_VALIDATION=1` to run with the Vulkan validation layers.

## Files

| File | Does |
| --- | --- |
| `main.cpp` | Command line, single instance, starts `App` |
| `app.cpp` | Finds the game, moves the overlay, shortcuts, presets, screenshots |
| `ui.cpp` | Menu, and what the shared launcher in `src/ui` shows |
| `effects.cpp` | ReShade effect runtime on Vulkan |
| `gpu.cpp` | Vulkan device, images and window swapchains |
| `platform_x11.cpp`, `platform_macos.mm` | Everything in `platform.h` |
| `setup.cpp` | `--install-effects` and the launcher's download button. Downloads over https only, except the sources given with `--effects-url` and `--presets-url`, which tests can point at local files |
| `config.cpp`, `games.cpp`, `hotkeys.cpp` | `Unishade.ini`, `games.ini` and shortcuts |

Shared with the Windows host, in `src/`: `preset_ini.h` (ReShade presets), `game_list.h` (`games.ini`), `hotkey_text.h` (how shortcuts are written), `names.h` (the rules for game folders and typed names), `package_files.h` (unpacking effect packages), `menu_layout.h` (the menu's size), `ini_text.h` and `theme.h`.
