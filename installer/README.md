# Installer

Unishade Setup is a single native EXE built with the rest of the project. It embeds Unishade.exe, which then downloads every effect package from ReShade's official list and the presets from `presets/`. Setup's window is drawn with [Dear ImGui](https://github.com/ocornut/imgui), which CMake downloads at configure time and checks the hash of.

```powershell
cmake -S . -B build -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release --target installer
```

Output: `build/installer/Unishade-Setup-<version>.exe`.

## What it does

Setup puts Unishade.exe, its license and its credits in the installation folder, then runs `Unishade.exe --install-effects` without a window and shows the progress it writes. That downloads the effects and presets into the folder, as **Download again** in Unishade's settings does. Cancelling, or closing the window, ends it. Unishade stays installed, and the finish page says what was left out.

Effect packages come from `EffectPackages.ini` in the `crosire/reshade-shaders` repository, the same list the ReShade installer uses. Unishade installs every package on the list, not only the ones the list enables by default, because the presets use effects from packages that are off by default there. Each package is extracted into `reshade-shaders/Shaders` and `reshade-shaders/Textures`, skipping the files the list denies. Only these file types are installed: effect sources (`.fx`, `.fxh`), textures ReShade can load (`.png`, `.jpg`, `.jpeg`, `.bmp`, `.tga`, `.dds`, `.hdr`, `.cube`), and plain-text licenses and readmes (`.txt`, `.md`, and `LICENSE`, `LICENCE`, `COPYING`, `NOTICE` or `README` without an extension). Add-ons, DLLs, programs and anything else in a package stay out. A package whose zip names a file outside its own folder, or a file name with a colon, which on Windows names a drive or a stream of another file, is left out. The code is in `src/setup/package_files.h`. The packages come from the moving branch heads the list names, so they are not checksummed; the file types are what limits them.

An effect package that cannot be downloaded or unpacked is left out, and the finish page lists it. When the list of effect packages or presets cannot be downloaded, or a preset does not match its SHA-256, the effects step stops and the finish page says why. Unishade is installed either way, and **Download again** tries again.

Unishade reads `presets/downloads.ini` from the `main` branch, downloads each listed preset and verifies its SHA-256. A preset already in the folder stays as it is. To add a preset, commit it to `presets/` and add its filename and hash to `presets/downloads.ini`.

Setup does not install add-ons. Unishade installs them from its Add-ons page.

Downloads use HTTPS and check certificates for revocation. When the revocation check cannot be completed, for example because the network blocks the revocation servers, Setup repeats the download without it, as browsers do, rather than failing every installation on such networks; a certificate Windows reports as revoked still fails. A proxy that asks to sign in with Windows authentication gets the user's Windows account.

Downloads have size limits, so a broken or hostile server cannot fill the memory or the disk: 4 MB for the lists and presets and 512 MB for an effect package. Before unpacking, an effect package may hold at most 20,000 files, 128 MB per file and 1 GB in total.

Reinstalling keeps `Unishade.ini`, presets and the saved game list in `games.ini`. Updating an installation of Unishade 0.6.3 or earlier keeps its `ReShade.ini` and `RobloxShadeHost.ini`, and Unishade starts its `Unishade.ini` from them the first time it runs. The update removes what Setup installed there for ReShade and the add-ons: `dxgi.dll`, `ReShade-LICENSE.txt`, `EffectPackages.ini`, `UnishadeUi.dll`, and the files of the depth and DLSS5 add-ons, which Unishade now keeps in its Addons folder. The `RobloxShadeHost-Setup.files` manifest keeps its original name.

When Setup creates the installation folder outside the user's profile, such as `C:\Games\Unishade`, the first folder it creates gets its own permissions: full control for the user, SYSTEM and Administrators, and read and run for other users. Otherwise it would inherit the permissions of a drive's root, where every signed-in user may change files, and another account could replace `Unishade.exe` or put a DLL beside it. Folders that already exist keep their permissions.

Setup copies itself into the installation folder as `Unishade-Setup.exe`, adds **Unishade** and **Unishade Setup** to the Start menu, and registers in Windows' app list under the key earlier Inno Setup versions used, so an update replaces their entry. Running it again from the Start menu offers updating and uninstalling. `RobloxShadeHost-Setup.files` lists the installed files for uninstalling, with the effect files Unishade downloaded while Setup ran. Setup writes the list also when copying fails partway, so whatever was copied can still be uninstalled. Entries that are absolute, contain a drive, `:` or `..`, or point outside the folder are dropped when the list is read or written, and the setup log names them.

Setup uninstalls only from a Unishade or RobloxShadeHost installation: a folder with `Unishade.exe` or `RobloxShadeHost.exe` and `RobloxShadeHost-Setup.files`. Anywhere else it refuses and deletes nothing.

Uninstalling deletes the files in `RobloxShadeHost-Setup.files`, the host's logs and locks (`Unishade.log`, `Unishade.old.log`, `Unishade.previous.log`, `unishade.lock`, `setup.lock`), its `cache` folder, the logs of Unishade 0.6.3 and ReShade (`RobloxShadeHost.log`, `RobloxShadeHost.old.log`, `ReShade.log` and its numbered copies), the Start menu shortcuts and the app list entry, then removes folders that are left empty. It also removes the **Start with Windows** entry (`Unishade` under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` and `...\Explorer\StartupApproved\Run`) when it starts the `Unishade.exe` in that folder, so uninstalling one copy leaves another copy's entry alone. It keeps `Unishade.ini`, `games.ini`, `presets`, the `Addons` folder, the effects the user added to `reshade-shaders`, and `ReShade.ini`, `ReShadePreset.ini` and `RobloxShadeHost.ini` from earlier versions.

When the user also asks to delete their files, it deletes those too: `Unishade.ini`, `games.ini`, `ReShade.ini`, `ReShadePreset.ini`, `RobloxShadeHost.ini` and the `presets`, `reshade-shaders` and `Addons` folders with everything in them. Other files, such as screenshots, stay in the folder. Setup never follows links or junctions when it deletes.

Credits open from the sidebar and are installed as `CREDITS.txt`. Removal requests go to **tiago@mouta.me**.

The uninstall identity remains `{77125AF5-DF0A-485A-A633-E64FBD50E90C}_is1`. Upgrading RobloxShadeHost reuses its registered folder, replaces the old executables and Start menu shortcuts with Unishade entries, and keeps user data. New installations default to `%LOCALAPPDATA%\Programs\Unishade`. Before it replaces or removes files, Setup closes a Unishade running from the folder through its hidden window of class `UnishadeMessages`, or through the window of class `RobloxShadeHost` for Unishade 0.6.3 and earlier.

Setup only loads system DLLs from System32, since its copy in the folder of Unishade 0.6.3 or earlier sits next to ReShade's `dxgi.dll` until the update removes it.

## Command line

Setup writes its log to `%TEMP%\Unishade-Setup.log`, or to the file given with `--log`.

Unattended installation of the host only:

```powershell
.\Unishade-Setup-<version>.exe --silent --components host
```

`--components` takes `host` and `effects`, separated by commas. Without it, Setup installs both.

| Option | Effect |
| --- | --- |
| `--silent` | Installs or uninstalls without a window. |
| `--dir <folder>` | Installation folder. Defaults to the registered installation or `%LOCALAPPDATA%\Programs\Unishade`. |
| `--components <list>` | What to install with `--silent`. |
| `--portable` | Leaves out the Start menu shortcuts, the app list entry and the copy of Setup. |
| `--launch` | With `--silent`, starts Unishade from the folder when Setup is done. A failed installation leaves the folder as it was, so then the version before starts. Unishade's own updates use it. |
| `--uninstall` | Opens the uninstall page, or uninstalls with `--silent`. |
| `--delete-user-files` | With `--uninstall --silent`, also deletes the user's files listed above. |
| `--log <file>` | Where to write the setup log. |

| Exit code | Meaning |
| --- | --- |
| 0 | Everything requested was installed or uninstalled. |
| 1 | Setup failed or was cancelled, or the command line is invalid. A failed installation leaves the folder unchanged. An uninstall refuses a folder that is not an installation, and reports files it could not delete. |
| 2 | Installed Unishade, but effects or presets were left out. The log says which. |

Tests use `--effects-url` and `--presets-url`, which Setup passes on to Unishade, to replace the download locations. They also take the full path of a local file, or for presets a local folder.

## Verification

Run `./tests/installer_tests.ps1` after building the installer. The tests install into fresh folders under `build/installer-tests` with `--portable`, so they do not touch the Start menu or Windows' app list. They check component selection, effect and preset installation, configuration preservation, updating an installation of Unishade 0.6.3, uninstalling, that uninstalling refuses other folders and ignores file list entries outside the folder, that it removes only its own **Start with Windows** entry, and that `--launch` starts Unishade. That check sets and restores the current user's `Unishade` startup entry. Internet access is required for the effect packages. On GitHub Actions, the presets come from the commit being tested; elsewhere, pass `-PresetsBaseUrl` to test presets that are not on `main` yet.
