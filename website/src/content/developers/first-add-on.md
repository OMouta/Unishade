---
title: Your first add-on
description: Build the example add-on, load it in Unishade and change it.
order: 2
---

This takes a C compiler and CMake. You end up with an add-on that shows a message, has controls in the menu and draws the frame rate over the game.

## Get the example

Put these four files in a folder:

- [addon.ini](/developers/example/addon.ini)
- [example.c](/developers/example/example.c)
- [CMakeLists.txt](/developers/example/CMakeLists.txt)
- [unishade_addon.h](links:addonHeader)

The first three are [src/addons/example](links:addonExample) in Unishade's source.

## Build it

```sh
cmake -S . -B build
cmake --build build --config Release
```

The library is `Example.dll` in `build/Release` on Windows, `build/Example.dylib` on macOS and `build/Example.so` on Linux.

## Put it in Unishade

1. In Unishade, click **Add-ons**, then **Open folder**.
2. Make a folder there. Its name is up to you.
3. Copy `addon.ini` and the library into it.

Unishade looks at the folder again within a few seconds. The add-on shows on the Add-ons page with its name, version and author, and asks whether to load it. Click **Load**.

## Try it

Start a game Unishade runs on and open the menu. Under **More**, the **Add-ons** tab has the example's settings and controls.

- **Say it** shows a message at the bottom of the game.
- **Show the frame rate** draws it in the top left corner.
- **Turn effects off** does what the power button does.

## Change it

Edit `example.c` and build again. Unishade keeps a loaded library as it is, and Windows does not let you replace one that is loaded. So turn the add-on off on the Add-ons page, copy the new library over the old one, and turn it on again. Unishade does not ask a second time. It remembers the answer for the add-on's `Id`.

When something goes wrong, the add-on's row says why. `host->log` writes to `Unishade.log` in Unishade's data folder, and warnings and errors also show under **Messages**.

## Make it yours

Change every line of `addon.ini`, the `Id` first. Then read on: [addon.ini](/developers/addon-ini/) for the folder, [The library](/developers/library/) for the code.
