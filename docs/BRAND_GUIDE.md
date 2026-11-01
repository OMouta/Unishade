# DSL MODS Visual Engine — Brand guide

## Core identity
- Product name: DSL MODS Visual Engine
- Short UI label: DSL MODS
- Tagline: See Beyond. Play Different.
- Product description: capture-based post-processing host for supported games, powered by ReShade.

## Visual language
- Background: `#100B16`
- Sidebar: `#0B0710`
- Surface/card: `#191222`
- Hover surface: `#241832`
- Border: `#332440`
- Strong border: `#62427D`
- Primary text: `#F4ECFA`
- Secondary text: `#A99AB5`
- Primary accent: `#A855F7`
- Accent hover: `#C084FC`
- Accent active: `#9333EA`
- Success/warning/error retain distinct semantic colors for accessibility and status clarity.

## UI rules
- Use purple for primary actions, selection and focus; do not use it for every label.
- Keep backgrounds near-black and cards subtly separated rather than using bright gradients everywhere.
- Preserve readable contrast and existing keyboard navigation.
- Keep the in-game menu compact and avoid obscuring the game view.
- Use the same product naming in the launcher, setup, tray tooltip, in-game menu and Windows file metadata.

## Compatibility rules
- Keep ReShade preset `.ini` and shader `.fx` / `.fxh` workflows compatible.
- Preserve upstream license notices and ReShade attribution.
- Do not alter capture/device recovery behavior as part of visual-only changes.
- Keep the external capture/host architecture clearly described; do not claim this is a conventional injected `dxgi.dll` package.

## Asset status
The current icon and embedded logo assets are still inherited from upstream. Replace them with DSL MODS artwork only after a final logo asset is supplied and its dimensions/format have been validated. Do not silently ship upstream artwork as DSL MODS original branding.
