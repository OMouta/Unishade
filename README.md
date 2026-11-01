<p align="center">
  <strong>DSL MODS VISUAL ENGINE</strong><br/>
  <em>See Beyond. Play Different.</em>
</p>

<p align="center">
  <strong>Windows 10/11 · x64 · ReShade-powered post-processing</strong>
</p>

DSL MODS Visual Engine is a customized desktop host for ReShade-based post-processing. It is being developed from the open-source [UniShade](https://github.com/OMouta/Unishade) project, with a new DSL MODS identity, a black-and-purple visual system, and a product roadmap focused on game capture, presets, effects and performance controls.

## Project status

This repository contains the upstream host and installer foundation. The DSL MODS customization is being introduced incrementally on a dedicated development branch so that the original working baseline remains available.

### Planned product direction

- Branded launcher and setup experience.
- Custom in-game menu using the existing Dear ImGui/ReShade integration.
- Purple-accent dark theme shared across the launcher, installer and in-game menu.
- Preset library and per-game profiles.
- Effects browser and technique controls.
- Hotkey, capture and performance settings.
- Import/export for compatible ReShade presets and shader assets.

## Important architecture note

This project follows UniShade's **external capture/host architecture**: it captures a supported game window and presents the processed image through its own host window, with ReShade running in that host. It is **not** a conventional ReShade injection package whose primary output is a drop-in dxgi.dll. The goal is to preserve this architecture and customize its application, installer and in-game overlay.

Games generally need to run windowed or borderless and must be compatible with Windows Graphics Capture. If conventional ReShade injection already works for a game, the standard ReShade installation may be the more suitable option.

## Building

See [CONTRIBUTING.md](CONTRIBUTING.md) for the upstream build prerequisites and build workflow. The project currently targets Windows x64 for the DSL MODS product.

## Upstream project and licensing

This project is derived from [OMouta/UniShade](https://github.com/OMouta/Unishade). The upstream MIT license and required copyright/permission notices are retained in [LICENSE](LICENSE). ReShade is a separate project by [crosire](https://github.com/crosire), subject to its own license and notices. DSL MODS Visual Engine is an independent customization and is not presented as an official UniShade or ReShade release.

Before distributing builds, review all third-party licenses, bundled assets, shader licenses and attribution requirements.
