---
title: Troubleshooting
description: What the messages in the Unishade window mean, and where the log is.
order: 5
---

The Unishade window lists what it found at startup and any problem since. Most messages say what to do. Only the newest version is supported, so [update](/download/) first.

Most of this page is about Windows. Where macOS and Linux differ, it says so. See also [macOS and Linux](/docs/macos-linux/).

## ReShade was not found, is too old, or didn't load the add-on

Windows only. Run **Unishade Setup** from the Start menu and choose **Update or change add-ons**. It installs the ReShade build with full add-on support, which the menu needs.

If you installed without Setup, install ReShade again with full add-on support, pick `Unishade.exe` and choose **DirectX 10/11/12**. ReShade set up for DirectX 9 or OpenGL shows no effects.

## No effects found

- Windows: run Setup again to download them.
- macOS and Linux: click **Download effects and presets** in the launcher.

## A shortcut is in use by another program

Pick another key for it in the Unishade window's **Settings > Shortcuts**, or close the other program.

## The game isn't detected

- Run the game in windowed or borderless mode.
- Check that the game is in the Unishade window's list and switched on.
- If the game moved to another folder, remove it and [add it again](/docs/games/#add-a-game).
- Try **Pick a window** in the Unishade window.
- On Linux, check that the game draws through X11 or XWayland. Games that draw to Wayland directly can't be captured.

Some games block screen capture or overlays, and those may not work.

## Unishade is inside Roblox's folder

Windows only. Roblox replaces its folder when it updates, which deletes Unishade. Uninstall and install again into another folder.

## Lower frame rate

Effects cost frame rate. Turn off the heaviest ones or pick a lighter preset. On Windows, depth estimation costs some too.

On Windows, **Performance** on a game's page in the Unishade window limits the frame rate and lowers the effect resolution and depth detail for that game. **Settings > Performance** in the menu changes the same values for the game you're playing.

## The log

- Windows: **Open log** in the Unishade window opens `Unishade.log`. The previous run's log is `Unishade.old.log` in the same folder. Setup writes its own log to `%TEMP%\Unishade-Setup.log`.
- macOS and Linux: **Open log** in the launcher opens `Unishade.log` in the data folder, next to `Unishade.old.log`.

## Get help

Ask on [Discord](links:discord) and attach the log.
