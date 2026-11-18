---
title: macOS and Linux
description: Screen recording, full screen and Wayland, for Unishade on a Mac or on Linux.
order: 6
---

Unishade is the same on Windows, macOS and Linux, and presets move between them unchanged. This page has what's particular to a Mac or to Linux. To install, see [Install Unishade](/docs/).

The Depth and DLSS5 add-ons are for Windows.

## macOS

Mac keyboards have no Home key, so the [shortcuts](/docs/menu/#shortcuts) differ: **Cmd+Shift+U** opens the menu.

### Screen recording permission

Unishade copies the game's window through screen recording, so macOS shows its recording indicator in the menu bar while a game runs.

- macOS only counts the permission once Unishade starts again. That's what **Restart Unishade** does.
- A new version of Unishade can lose the permission. In the Screen Recording list, remove Unishade with the minus button and add it again. Or run `tccutil reset ScreenCapture me.unishade.Unishade` in Terminal, then click **Allow screen recording** again.
- macOS 15 and newer ask from time to time whether Unishade may keep recording. Allow it to keep effects running.
- If you stop the recording from the menu bar, effects turn off. Turn them back on in the Unishade window, or with Ctrl+Cmd+O.

### Full screen and windows

- Unishade covers games in full screen, also on their own Space.
- In a window, effects leave the game's title bar alone.
- A borderless game in a window has no title bar, so effects miss a strip at its top. Turn off **Skip the title bar** in **Settings**.
- While another window of the game is in front of it, such as a dialog or a file picker, effects hide.

### A colored rim around the game

Hold Cmd+Shift+C, which shows the game without effects. If the rim goes away, an effect in the preset draws it. If it stays after you quit Unishade, it comes from another app.

## Linux

Unishade works with games that draw through X11. That covers X11 desktops and, on Wayland desktops, every game that runs through XWayland, such as Wine and Proton games.

A game that draws to Wayland directly can't be captured. Unishade says so when a saved game runs that way. Switch the game to XWayland, such as with `SDL_VIDEODRIVER=x11`. For Roblox through Sober: `flatpak run --nosocket=wayland org.vinegarhq.Sober`.

## Where things are kept

**Settings > About > Data folder** opens it.

- macOS: `~/Library/Application Support/Unishade`
- Linux: `~/.local/share/unishade`

Screenshots go to `Unishade` in your pictures folder.

From a terminal, `unishade --install-effects` downloads the effects and presets. On a Mac, run the executable inside the app, `Unishade.app/Contents/MacOS/Unishade`.
