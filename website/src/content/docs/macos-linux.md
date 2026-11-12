---
title: macOS and Linux
description: Run Unishade on a Mac or on Linux, where ReShade itself does not run.
order: 6
---

Unishade for macOS and Linux is experimental. It's new, so expect bugs.

ReShade only runs on Windows, so Unishade for macOS and Linux runs ReShade's effects itself. It uses ReShade's own effect compiler with Vulkan, through MoltenVK on a Mac. The same effect packages and presets work, and presets move between Windows, macOS and Linux unchanged.

Like on Windows, Unishade copies the game's window and draws it again with effects in a window of its own on top. It doesn't touch the game.

## macOS

You need a Mac with Apple Silicon (M1 or newer) and macOS 13 or newer. Macs with an Intel processor aren't supported.

1. Download **Unishade-macOS.zip** from the [download page](/download/#macos-and-linux), unzip it and move **Unishade** to Applications.
2. The build isn't notarized yet, so macOS stops it the first time:
   - On macOS 15 or newer, open Unishade and click **Done** when macOS says it can't verify it. Then open **System Settings > Privacy & Security**, scroll down to Security and click **Open Anyway** next to Unishade. Confirm with **Open Anyway** and your password.
   - On macOS 13 and 14, right-click Unishade and choose **Open**, then click **Open**.
3. Click **Allow screen recording** in the launcher and allow Unishade under **System Settings > Privacy & Security > Screen & System Audio Recording**, called **Screen Recording** on macOS 13 and 14. Then click **Restart Unishade** in the launcher.

The shortcuts differ, since Mac keyboards have no Home key:

| Shortcut | Does |
| --- | --- |
| Cmd+Shift+U | Opens and closes the menu |
| Ctrl+Cmd+O | Turns the overlay on and off |
| Cmd+Shift+C (hold) | Shows the game without effects |
| Cmd+Shift+P | Screenshot |
| Cmd+Shift+B | Before and after screenshot |
| Cmd+Shift+Right / Left | Next or previous preset |

Roblox is found automatically.

### Full screen and windows

- Unishade covers games in full screen, also on their own Space. While the game shows the cursor, the menu opens without making Unishade the active app, so macOS stays on the game's Space. When the game hides the cursor, as first person games do, Unishade becomes the active app while the menu is open, so the game lets go of the mouse.
- The launcher's window picker also lists a game that is in full screen on another Space.
- Wine, CrossOver, Whisky and Game Porting Toolkit games can raise their window above other windows in full screen. Unishade lists such a window and keeps its overlay just above it.
- In a window, effects leave the title bar alone and cover only the game's picture. The overlay's corners are rounded like the window's.
- A borderless game in a window has no title bar, so effects miss a strip at its top. Turn off **Skip the title bar** in the menu's **Settings > General**. It's saved as `SkipTitleBar=0` in `Unishade.ini` in the data folder.
- While another window of the game is in front of it, such as a dialog, a launcher or a file picker, the overlay hides.

### Screen recording permission

- Unishade copies the game's window through screen recording, so macOS shows its purple recording indicator in the menu bar while Unishade copies a game. That's expected.
- macOS only counts the permission once Unishade starts again. That's what **Restart Unishade** does. If it doesn't show, quit Unishade and open it again.
- The build has no stable signature yet, so a new version of Unishade can lose the permission when it replaces the old one. Select Unishade in the Screen Recording list, remove it with the minus button and add it again. Or run `tccutil reset ScreenCapture me.unishade.Unishade` in Terminal, then click **Allow screen recording** in the launcher again.
- macOS 15 and newer ask again from time to time whether Unishade may keep recording the screen. That's macOS, not Unishade. Allow it to keep effects running.
- If you stop the recording from the menu bar, effects turn off and the launcher says **Effects off**. Turn them back on with **Show effects** in the launcher, or with Ctrl+Cmd+O.

## Linux

Unishade works with games that draw through X11. That covers X11 desktops and, on Wayland desktops, every game that runs through XWayland, such as Wine and Proton games. Games that draw to Wayland directly can't be captured. It needs a 64-bit (x64) PC.

1. Install the Vulkan driver for your graphics card. Most distributions include it.
2. Download **Unishade-linux-x64.tar.gz** from the [download page](/download/#macos-and-linux) and unpack it. It holds a **Unishade** folder.
3. Open a terminal where you unpacked it and run `cd Unishade`, then `./unishade`.

Roblox is found automatically, whether it runs through Sober or through Wine. If Sober draws to Wayland directly on your desktop, start it without Wayland so it uses XWayland: `flatpak run --nosocket=wayland org.vinegarhq.Sober`.

The shortcuts are the same as on Windows: **Home** opens the menu.

## Effects

The first time, click **Download effects and presets** in the launcher. Unishade downloads every package from ReShade's official list and the Unishade presets, like Setup on Windows. From a terminal, `unishade --install-effects` does the same. On a Mac, run the executable inside the app, `Unishade.app/Contents/MacOS/Unishade`.

Everything is kept in one folder, which the launcher's **Data folder** link opens:

- macOS: `~/Library/Application Support/Unishade`
- Linux: `~/.local/share/unishade`

Screenshots go to `~/Pictures/Unishade`.

## What's different from Windows

- Everything the menu shows happens in Unishade's own menu. ReShade's menu isn't there.
- Preprocessor definitions, which some effects use for quality and feature options, are at the bottom of an effect's settings. A new value applies when you press **Enter**.
- Depth estimation and DLSS5 are Windows only. Effects that need the game's depth see an empty depth buffer, as they would on Windows without the depth add-on.
- A few effects use stencil, which is skipped. The **Status** tab lists effects that didn't compile.
