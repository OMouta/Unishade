# DSL MODS Visual Engine — Development baseline

## Product identity
- Product: DSL MODS Visual Engine
- Tagline: See Beyond. Play Different.
- Initial platform: Windows 10/11 x64
- UI direction: near-black surfaces, purple accent, restrained borders and clear status colors
- UI technology: existing Dear ImGui integration from the upstream host

## Architecture decision
Keep the upstream external-capture host architecture. The application captures a window using Windows Graphics Capture, processes frames through its host graphics pipeline with ReShade, and draws its own overlay/menu. This is not a traditional in-process ReShade injector and must not be marketed as a drop-in dxgi.dll build.

## Implementation phases
1. **Brand foundation** — product README, shared palette and dedicated development branch.
2. **Application identity** — visible names, window titles, tray labels, installer text, version resources and output filenames.
3. **In-game overlay** — DSL MODS header/branding, navigation, effects and preset workflows while retaining ReShade compatibility.
4. **Launcher and installer** — matching identity, game profiles, setup flow and safe uninstall/repair.
5. **Quality** — build validation, regression tests, license audit, packaging and release workflow.

## Compatibility and safety
- Preserve upstream license and copyright notices.
- Do not remove ReShade attribution.
- Keep preset .ini and shader .fx / .fxh compatibility.
- Avoid changing capture, device recovery or shader-runtime behavior during cosmetic refactors.
- Do not claim a build works until it has been compiled and tested on Windows.
