---
title: Overview
description: Add-ons extend Unishade with native code. Here is what they can do and where to start.
order: 1
---

An add-on is a folder with a small text file and a native library. Unishade loads the library and calls it. Add-ons are how Unishade's own Depth and DLSS5 work, and anyone can write one.

## What an add-on can do

- [Add settings](/developers/settings/) that Unishade shows in its menu and saves.
- [Show controls of its own](/developers/controls/), such as text that changes, progress and buttons.
- [Draw over the game](/developers/drawing/), such as a crosshair or a frame rate.
- [Ask Unishade things and act on it](/developers/functions/). It can read the game and the preset, show a message, take a screenshot, switch presets, and turn effects on and off or change their values.
- [React to what happens](/developers/functions/#events), such as a game starting or a screenshot being saved.
- [Work on the game's picture](/developers/frames/) on the graphics card, before effects run or after.

Most add-ons need only the first few. Working on the picture is the hard part, and you can skip it.

## How it fits together

The whole interface is one C header, [unishade_addon.h](links:addonHeader). Any language that can build a shared library with C functions works. The header's comments are the reference, and these pages explain them.

1. [addon.ini](/developers/addon-ini/) says what the add-on is. Unishade reads it without running anything.
2. [The library](/developers/library/) exports one function that says what the add-on does and what to call.
3. Unishade calls those functions on its main thread, and the add-on calls back through the host it was given.

## Start here

[Your first add-on](/developers/first-add-on/) builds the [example add-on](links:addonExample) and loads it. It is about 160 lines of C with no graphics code, and it uses settings, controls, drawing and events.

## For agents

Every page here is also Markdown. **Copy as Markdown** at the top of a page copies it. The file's address is the page's with `.md` in place of the last slash, such as `/developers/settings.md`, and this page is `/developers/overview.md`. [llms.txt](/llms.txt) lists the pages, and [llms-full.txt](/llms-full.txt) is all of them in one file with the header and the example add-on.

## Trust

An add-on runs as part of Unishade, with the same access to the computer. Unishade asks the user once before it loads an add-on that is not one of its own. A crash in an add-on is a crash in Unishade.

Questions go to [Discord](links:discord).
