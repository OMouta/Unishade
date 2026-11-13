# Unishade for macOS and Linux

ReShade is Windows only, so this host runs ReShade effects itself. It follows the Windows design: it copies the game's window, runs effects on the copy and shows the result in a window over the game, with clicks passing through until the menu opens.

| | macOS | Linux |
| --- | --- | --- |
| Capture | ScreenCaptureKit | XComposite, with XDamage for when the game drew (X11 and XWayland) |
| Windows and processes | CoreGraphics window list, `NSRunningApplication`, `proc_pidpath` | `_NET_CLIENT_LIST`, XRes, `/proc` |
| Overlay window | A non-activating `NSPanel`, through a patch to GLFW | A GLFW window of the utility type, kept above |
| Shortcuts | Carbon hot keys | `XGrabKey` on the root window |
| Keys and buttons for effects | `CGEventSourceKeyState` | `XQueryKeymap`, `XQueryPointer` |
| Vulkan | MoltenVK, linked directly and copied into the app | The system's loader and driver |
| Frames on the graphics card | IOSurfaces as Vulkan images (`VK_EXT_metal_objects`) | DRI3 dma-bufs as Vulkan images (`VK_EXT_image_drm_format_modifier`) |

Frames stay on the graphics card where the system allows it, and are copied through memory otherwise: on Linux without DRI3 1.2 (no GPU, some NVIDIA setups on Xorg, remote X), without the Vulkan extensions above, or when the driver cannot import the X server's buffer layout. Copies go through shared memory (XShm), or without it where the X server cannot reach Unishade's memory, such as over the network. The log says which one runs. `--render ... --gpu-source` exercises the graphics-card path of the effect runtime without a window.

On Linux, frames are copied when XDamage says the game drew, at most about once per refresh of the monitor the game is on. Without XDamage they are copied once per refresh. While the overlay is hidden, because another window is in front, a few frames a second are copied. On macOS the stream itself is slowed to five frames a second then. Frames handed over as dma-bufs are not synchronised with the game's drawing, so one can now and then show a picture still being drawn.

Under a Wayland desktop only games that run through XWayland have a window Unishade can see. When a saved game runs without one, the launcher says so and how to switch the game to XWayland, such as `SDL_VIDEODRIVER=x11` or turning off Wayland in Wine, Proton or the game's launcher.

Effects compile with ReShade's own compiler (`effect_*.cpp` from ReShade 6.8.0, BSD-3-Clause) to SPIR-V, the same path ReShade takes in Vulkan games. `effects.cpp` runs what it produces: textures, render targets, stencil tests, storage, compute passes, blending, mipmaps and the uniforms ReShade sets itself, such as `timer` and `frametime`. Presets are ReShade's `.ini` files. The launcher and the menu use Dear ImGui with GLFW. Release builds turn off Dear ImGui's red outline around controls that share an ID, which users took for a border Unishade draws.

The effects the preset uses compile first. The rest of the library compiles after them on two threads at low priority, so a game keeps its cores, and the menu lists them as they arrive.

Compiled effects are kept in `cache` in the data folder, so effects start faster the next time: the SPIR-V of the effects the preset uses in `cache/preset-effects`, that of the rest in `cache/effects`, and Vulkan's pipeline cache in `cache/pipelines.bin`. An effect is compiled for the size of the game's picture, so every size has entries of its own. The preset's effects have 32 MB and the rest 224 MB, and beyond that the entries used longest ago are dropped, so the library compiled for another size cannot push out the effects in use.

## The overlay on macOS

### Which windows can be games

`ListWindows` reads every window, not only those on screen, since a game on its own full-screen Space is off the screen while another Space shows. A window off the screen is listed only when it covers a screen and its program has a Dock icon, because that is what such a game looks like and the hidden windows programs keep around are not. Most games sit on the normal layer. Wine raises some windows above it. A window that covers the screen goes one above the status level while it is active, a topmost window one above the floating or Dock level, and with the displays captured one above the shielding level. So the layers up to the pop-up menu level, and the shielding level and the one above it, count too. They also hold the Dock, the menu bar, status items, Control Center and other programs' overlays, which all belong to programs without a Dock icon, so a window above the normal layer counts only for a regular program. `window_rules::GameLayer` holds the rule, and `ForegroundWindow` applies it as well.

### The title bar

ScreenCaptureKit copies a single window whole, title bar included. macOS gives no way to read another program's content area without the accessibility permission, so a rule stands in. A window on the normal layer that does not cover its screen is taken to have the standard title bar. Before macOS 26 that is 28 points. macOS 26 draws a 32 point title bar only for programs built with SDK 26 or newer and gives older ones the earlier look, so `macho.h` reads the SDK from the game executable's Mach-O build version. Asking AppKit would answer for Unishade's own SDK. Wine games are drawn by Wine, so Wine's executable decides, and an executable that cannot be read counts as the earlier look. `WindowBounds` and the frames leave out the same rows, decided on the main thread, which owns `NSScreen`, and handed to the capture queue through `titleInset`. `TakeFrame` decides again on every pass, so the crop follows a window into and out of full screen while the overlay is hidden. A borderless game in a window loses its top strip by this rule, and a window with a taller title bar, such as one with a toolbar, keeps the extra rows. `SkipTitleBar` under `[HOST]` in `Unishade.ini`, the menu's **Skip the title bar**, turns the rule off. `CoversScreen` lets the top start as low as the safe area, since macOS puts full-screen windows below the camera housing. The allowance stops one point short of the menu bar's height, because a zoomed window with a title bar starts below the menu bar. With the menu bar hidden automatically, a zoomed window cannot be told from one in full screen. `ContentCrop` rounds the window's edges inward, so the crop never takes pixels from outside it.

### A panel that does not activate

The menu needs the keyboard, but making Unishade the active program switches away from a game's full-screen Space. Only a panel created with `NSWindowStyleMaskNonactivatingPanel` can take the keyboard without activating its program, and the style has to be set when the window is made. GLFW makes its windows itself, so `posix.cmake` patches GLFW's `cocoa_window.m` in a copy under `patched/`. The patch adds `GLFWPanel`, an `NSPanel` that can become key, and `glfwCocoaPanelHint`, which makes the next window GLFW creates such a panel. `PrepareOverlayWindow` sets the hint. Configuring stops if the patched lines change, so a GLFW update cannot drop the patch unnoticed. If the overlay still is not a panel, the log says so and `FocusOverlay` activates Unishade as before.

### Focus

`FocusOverlay` orders the panel in front and makes it key, while the game stays the active program. A game that hides the cursor, as first person games do, lets go of the mouse only once it is no longer the active program. So when `CGCursorIsVisible` says the cursor is hidden, `FocusOverlay` also activates Unishade. The panel is key first, so Unishade has no other window for macOS to switch to. `CGCursorIsVisible` answers for the whole session, so a cursor hidden by another program counts too.

`ReturnFocus` follows the same two paths. When Unishade is the active program, or the game is not in front, it activates the game, which takes back the keyboard and the mouse. The panel is not ordered out then, because AppKit would make the launcher key and could switch to the launcher's Space. When the game stayed the active program, the key panel stays key until another window becomes key, and the game has no reason to ask. So `ReturnFocus` orders the panel out and straight back in, which gives the keyboard back to the game's window, and the overlay is missing for a frame at most.

### Level and corners

`MatchOverlay` runs on every pass while the overlay shows. It puts the overlay at the status level, or one above the game's layer when that is higher, since Wine raises and lowers its window as the game gains and loses focus. The captured picture is clear where a titled window's corners round, and shows black there, so the overlay rounds the corners of its own layer. It rounds the bottom ones when it leaves out the title bar and all four when it covers the whole window. It leaves them square when the window covers its screen or sits above the normal layer. macOS does not tell another program's corner radius, so the radius is measured from a standard window and rounded up, because a sliver without effects shows less than a black notch. It is 18 points for a game built with SDK 26 or newer on macOS 26, and 12 points otherwise, by the same SDK check as the title bar.

### What shows through the overlay

The overlay window is not opaque and has a clear background, so macOS does not take the game for covered and let it draw less often, which would slow capture down. Its pixels are still opaque, because the swapchain asks for opaque composite alpha, which makes MoltenVK's layer opaque, and `MatchOverlay` keeps it so. `canHide` is off, so hiding Unishade, or every program but the game, leaves the overlay over the game.

### The game's own windows

A dialog, launcher or file picker of the game would sit under the overlay's old picture of the game. `GameWindowCovered` looks for a window of the game's program above the game's window that overlaps it, on the normal layer or above and below the overlay's level, and the overlay hides while one does. Windows under 64 points, overlaps under 32 points, windows below the normal layer and windows the size and place of the game's are skipped, since they are more likely parts of the game. A game's see-through windows, as Wine and browsers make for overlays, report full alpha anyway, so a window also needs a title, as dialogs and file pickers have, or a quarter of the game's area. When unsure, the overlay stays. The host asks four times a second, since it lists windows.

### Errors by code

ScreenCaptureKit's descriptions of its errors are vague, so `FailWithError` logs the code and acts on it. The user refusing the permission makes `HasCapturePermission` false from then on, since `CGPreflightScreenCaptureAccess` only answers with what macOS said when Unishade started. The user stopping the recording from the menu bar turns effects off, because starting again at once would undo what they did. A closed window says so. `StopStream` ignores the result of a stop Unishade asked for, so every other error that reaches `FailWithError`, even one about stopping, means the stream is dead. Such a stream, as when the capture service's connection breaks, is started again after two seconds, and the log repeats an error only when it changes.

### Permission and restart

macOS shows its request for the permission only the first time, so `RequestCapturePermission` remembers in the user defaults that it asked and opens System Settings after that. A permission given later counts only in a new process, so the launcher offers **Restart Unishade**. `Relaunch` starts the app through `open -n`, so the new process is the app itself rather than a child of this one, which macOS checks the permission against. `--relaunch` makes it wait up to ten seconds for the old one to let go of the single-instance lock.

### Starting from the Finder

Nobody sees the terminal there, so a failed start and a second Unishade show an alert. The second one then brings the running Unishade to the front.

### App Nap

The overlay never has focus and the launcher is often covered, which App Nap takes for a program nobody uses. An `NSProcessInfo` activity, latency critical and allowing idle sleep, holds while capture runs. The capture queue runs at user-interactive priority and releases each frame's objects when the frame is done.

### Presenting

MoltenVK offers no mailbox mode, only FIFO and immediate. In FIFO the overlay would wait for the display on top of the game's own wait, so it presents in immediate mode, which does not tear on macOS, where every window is composited. The launcher keeps FIFO. The log says which mode each window got. While the menu is open, frames that no new picture of the game caused are held to the refresh rate of the overlay's monitor.

## Shortcuts, screenshots and input

Shortcuts are written as on Windows, such as `Ctrl+F9`, including the number pad keys `Numpad0` to `Numpad9`, `NumpadMultiply`, `NumpadAdd`, `NumpadSubtract`, `NumpadDecimal` and `NumpadDivide`. macOS has function keys up to F20.

Screenshots go to `Unishade` in the pictures folder: `~/Pictures` on macOS, and on Linux the one `XDG_PICTURES_DIR` names in the environment or in `~/.config/user-dirs.dirs`, which is not always `~/Pictures`. Files are named after the game, with anything that does not belong in a file name left out.

Effects that read the keyboard and mouse, through uniforms such as `key` and `mousebutton`, see Windows virtual-key codes, as on Windows. Unishade reads them for the whole system, so only while the game or the menu is in front, and only while something uses them: a technique with a shortcut, or such a uniform in an effect that is on. The mouse wheel only reaches effects while the menu is open, and X11 cannot tell the back and forward mouse buttons.

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

The Linux build is `build/unishade`. The macOS build is `build/Unishade.app`, with MoltenVK inside and an ad-hoc signature. An ad-hoc signature differs between builds, so macOS can drop the screen recording permission when a new build replaces the app.

The Mac app runs on macOS 13 and newer. The top-level `CMakeLists.txt` sets `CMAKE_OSX_DEPLOYMENT_TARGET` before `project()`, because `project()` reads it when it enables the first language, and a value set later has no effect. A value on the command line or in `MACOSX_DEPLOYMENT_TARGET` wins. CI reads the minimum version of the app and of `libMoltenVK.dylib` with `vtool` and fails above 13.0, since the runner's own macOS or a newer MoltenVK from Homebrew would raise it unnoticed. XRes, XDamage and DRI3 are loaded when the system has them, so only their headers are needed to build; without a header the build leaves that part out.

## Checking effects without a game

`unishade --render in.png preset.ini out.png` applies a preset to an image with the overlay's own code and no window. It exits with 1 when a technique the preset turns on is missing or fails to start. CI renders the repository's presets this way on Mesa's software Vulkan driver.

`--render` and `--install-effects` write their log to the terminal only and do not take the lock that keeps a second Unishade from starting, so the app can start while one runs. Ctrl-C, `SIGTERM` and `SIGHUP` quit the app the way closing the launcher does, and cancel a terminal install.

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
| `macho.h` | Reads the SDK a Mac program was built with from its executable, so the overlay knows whether macOS 26 draws the game with the new look. Plain C++, so tests can check it anywhere |
| `window_rules.h` | macOS rules for which layers can hold a game, whether a window covers its screen, and which pixels of a frame are the game's. Kept apart so `tests/posix_tests.cpp` can check them without a window |
| `setup.cpp` | `--install-effects` and the launcher's download button. Downloads over https only, except the sources given with `--effects-url` and `--presets-url`, which tests can point at local files |
| `config.cpp`, `games.cpp`, `hotkeys.cpp` | `Unishade.ini`, `games.ini` and shortcuts |

Shared with the Windows host, in `src/`: `preset_ini.h` (ReShade presets), `game_list.h` (`games.ini`), `hotkey_text.h` (how shortcuts are written), `names.h` (the rules for game folders and typed names), `package_files.h` (unpacking effect packages), `menu_layout.h` (the menu's size), `ini_text.h` and `theme.h`.
