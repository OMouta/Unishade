---
title: addon.ini
description: The file that says what an add-on is, which Unishade reads without running anything.
order: 3
---

Every add-on is a folder in Unishade's Addons folder with an `addon.ini` in it. A folder without one is ignored, and so is a file that is directly in the Addons folder.

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

## Keys

| Key | Needed | What it is |
| --- | --- | --- |
| `Id` | Yes | Names the add-on in presets and in `Unishade.ini`. One to 64 lowercase letters, digits, dots, hyphens and underscores, starting with a letter or digit. |
| `Api` | Yes | The version of `unishade_addon.h` the library was built for, which is `UNISHADE_API_VERSION` there. |
| `Name` | No | Shown on the Add-ons page and in the menu. Up to 64 bytes. Without one, the `Id` shows. |
| `Version` | No | Shown beside the name. Any text up to 32 bytes. |
| `Author` | No | Shown beside the version. Up to 64 bytes. |
| `Description` | No | A sentence under the name. Up to 400 bytes. |
| `Icon` | No | A PNG in the folder. Without one, the start of the name shows on a color. |
| `Website`, `License` | No | Read and kept. The Add-ons page does not show them yet. |

Choose an `Id` you will never change. Presets list the add-ons they use by `Id` and keep their settings under it. A prefix of your own, as in `someone.my-addon`, keeps it apart from other people's.

## Systems

Each system the add-on runs on has a section with its `Library`, a path inside the folder with forward slashes.

| Section | For |
| --- | --- |
| `windows-x64`, `windows-arm64` | Windows on one processor |
| `macos-arm64`, `macos-x64` | macOS on Apple silicon or Intel |
| `linux-x64`, `linux-arm64` | Linux on one processor |
| `windows`, `macos`, `linux` | Every processor of that system, such as a universal library on macOS |

Unishade looks for the section with the processor first, then the one for the system alone. An add-on without a section for the system it is on shows as "It isn't made for this system."

Other libraries the add-on needs can sit beside its own. On Windows, the add-on's imports resolve from its own folder first.

## What Unishade does with it

Unishade reads `addon.ini` when it starts and whenever the folder changes. It shows the add-on on the Add-ons page from the file alone. The library is loaded only once the user allows it, and the answer is remembered for the `Id`.

An add-on that cannot be used stays on the page and says why:

- Its `addon.ini` has no `Id` or `Api`, or the `Id` is not valid.
- Another add-on in the Addons folder has the same `Id`. The first by folder name keeps it.
- Its `Api` is newer than this Unishade's.
- The library is missing, or is outside the folder.

## Sharing an add-on

Zip the folder. Whoever gets it unzips it into their Addons folder.
