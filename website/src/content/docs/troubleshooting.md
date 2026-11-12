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

## The overlay doesn't appear on a Mac

macOS only. Unishade needs permission to record the screen.

- Click **Allow screen recording** in the launcher, allow Unishade in System Settings, then click **Restart Unishade**. macOS only counts the permission after a restart.
- After an update, Unishade can lose the permission. Remove it from the Screen Recording list with the minus button and add it again, or run `tccutil reset ScreenCapture me.unishade.Unishade` in Terminal and allow it again.

See [screen recording permission](/docs/macos-linux/#screen-recording-permission).

## Effects cover or miss a strip at the top of the window

macOS only. Effects leave out the title bar of a game in a window. A borderless game in a window has no title bar, so its top strip stays without effects. Turn off **Skip the title bar** in the menu's **Settings > General**. Turn it back on if effects then cover a game's title bar.

## Effects turned off after stopping the recording

macOS only. Stopping the recording from the macOS menu bar turns effects off, and the launcher says **Effects off**. Turn them back on with **Show effects** in the launcher, or with Ctrl+Cmd+O.

## A red border or red rectangles

macOS only.

- macOS draws no border around a window Unishade copies.
- Earlier versions could outline menu controls in red while the mouse was over them. That's off now, so [update](/download/).
- A colored rim around the game can come from an effect in the preset. Hold Cmd+Shift+C, which shows the game without effects. If the rim goes away, an effect draws it.
- If the border stays after you quit Unishade, it comes from another app.

If none of that fits, ask on [Discord](links:discord) with a screenshot, your macOS version and the log.

## The log

- Windows: **Open log** in the Unishade window opens `Unishade.log`. The previous run's log is `Unishade.old.log` in the same folder. Setup writes its own log to `%TEMP%\Unishade-Setup.log`.
- macOS and Linux: **Open log** in the launcher opens `Unishade.log` in the data folder, next to `Unishade.old.log`.

## Get help

Ask on [Discord](links:discord) and attach the log.
