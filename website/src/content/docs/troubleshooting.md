---
title: Troubleshooting
description: What to check when a game isn't picked up or effects don't show, and where the log is.
order: 5
---

**Messages** at the top of the Unishade window lists any problem since it started. Most messages say what to do. Only the newest version is supported, so [update](/download/) first.

## The game isn't detected

- Run the game in windowed or borderless mode.
- Check that the game is on the shelf, and that **Use Unishade** is on for it.
- If the game moved to another folder, remove it and [add it again](/docs/games/#add-a-game).
- Try **Pick a window** in the Unishade window.
- On Linux, the game has to draw through X11 or XWayland. See [Linux](/docs/macos-linux/#linux).

Some games block screen capture or overlays, and those may not work.

## Effects are not installed yet

Click **Download effects and presets** in the Unishade window. **Settings > Download effects and presets again** gets them again later.

## An effect's tile says Failed

That effect could not start on your graphics card. The rest of the preset still runs. Remove the effect, or pick another preset.

## A shortcut is used by another program

Pick other keys for it in **Settings > Shortcuts**, or close the other program.

## Lower frame rate

Effects cost frame rate. Turn off the heaviest ones or pick a lighter preset. The Depth add-on costs some too.

[Performance](/docs/games/#performance) limits the frame rate and, on Windows, lowers the effect resolution.

## Unishade is inside Roblox's folder

Windows only. Roblox replaces its folder when it updates, which deletes Unishade. Uninstall and install again into another folder.

## The overlay doesn't appear on a Mac

Unishade needs permission to record the screen. See [screen recording permission](/docs/macos-linux/#screen-recording-permission).

## The log

**Settings > About > Open log** opens `Unishade.log`. The previous run's log is `Unishade.old.log` in the same folder. On Windows, Setup writes its own log to `%TEMP%\Unishade-Setup.log`.

## Get help

Ask on [Discord](links:discord) and attach the log.
