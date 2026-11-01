# DSL MODS Visual Engine — Brand guide

## Core identity
- Product name: DSL MODS Visual Engine
- Short UI label: DSL MODS
- Tagline: See Beyond. Play Different.
- Product description: capture-based post-processing host for supported games, powered by ReShade.

## Visual language
- Background: `#090B0E`
- Sidebar: `#050608`
- Surface/card: `#15191E`
- Hover surface: `#222830`
- Border: `#343B44`
- Strong border: `#78828D`
- Primary text: `#FFFFFF`
- Secondary text: `#B9C1CA`
- Primary accent: `#AEB8C2`
- Accent hover: `#E1E6EB`
- Accent active: `#7D8791`
- Success/warning/error retain distinct semantic colors for accessibility and status clarity.

## UI rules
- Use silver accents for primary actions, selection and focus; reserve semantic colors for status and warnings.
- Keep backgrounds near-black and cards subtly separated rather than using bright gradients everywhere.
- Preserve readable contrast and existing keyboard navigation.
- Use the premium desktop-style in-game layout while scaling or scrolling safely on smaller game viewports.
- Use the same product naming in the launcher, setup, tray tooltip, in-game menu and Windows file metadata.

## Compatibility rules
- Keep ReShade preset `.ini` and shader `.fx` / `.fxh` workflows compatible.
- Preserve upstream license notices and ReShade attribution.
- Do not alter capture/device recovery behavior as part of visual-only changes.
- Keep the external capture/host architecture clearly described; do not claim this is a conventional injected `dxgi.dll` package.

## Asset status
The current icon and embedded logo assets are still inherited from upstream. The UI must remain fully functional without the final artwork.

When the approved `base.png` is supplied:
- Validate dimensions, transparency, color profile and readability at small sizes.
- Generate dedicated scaled variants for launcher, in-game header and installer.
- Generate the Windows `.ico` from the approved source artwork.
- Keep a single source-of-truth asset directory and update the embedded resource references.
- Do not stretch, recolor or redraw the logo unless explicitly approved.
- Keep layout spacing independent from the logo dimensions so artwork can be replaced without redesigning screens.

Until then, use neutral geometric placeholders and never invent a replacement logo.
