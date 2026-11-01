# In-game premium layout specification

## Purpose

The ReShade in-game control panel is the primary visual touchpoint of DSL MODS Visual Engine. It should feel like a premium graphics utility while remaining readable over a running game and preserving ReShade's existing control behavior.

## Visual system

- Background: near-black `#090B0E`.
- Navigation rail: `#050608`.
- Cards: `#15191E`; hover: `#222830`.
- Borders: `#343B44`; emphasized border: `#78828D`.
- Primary text: white; secondary text: silver-gray.
- Silver is the primary accent. Semantic green, amber and red are reserved for state, warnings and errors.
- Avoid purple, bright gradients, excessive glow and decorative motion.

## Layout

- Use an expanded desktop-style panel with a fixed header, vertical navigation rail, independent content region and fixed footer.
- The 1080p reference width is 820 logical pixels; the navigation rail is 164 logical pixels.
- Header retains the product name, tagline, current preset, save state and global effects switch.
- Navigation includes Presets, Effects, optional DLSS5, Settings and Status.
- Footer retains Compare, ReShade, screenshot actions and the return-to-game shortcut.
- At smaller viewport sizes, use the existing scale and scroll behavior rather than clipping controls or making text unreadable.

## Effects workspace

- Keep active effects visible before inactive effects.
- Preserve search, per-technique enable/disable, drag ordering and parameter expansion.
- Present each technique as a separated card with a clear hover/expanded state.
- Keep parameter controls generated from ReShade annotations; do not hardcode shader-specific controls.
- Surface compile failures and missing effect files as readable status messages.
- Do not claim FPS or frametime values unless the host has a reliable source for them.

## Presets workspace

- Preserve create, import, save, save-as-new, delete and folder navigation behavior.
- Clearly distinguish the selected preset and unsaved changes.
- Missing effects must remain visible as warnings; never silently discard preset settings.

## Logo-ready asset architecture

The UI must not depend on the final logo file to compile or run. Keep brand placement and layout geometry separate from image dimensions.

When the approved `base.png` arrives:
1. Inspect its dimensions, transparency and visual legibility.
2. Keep the untouched source in the agreed brand-assets directory.
3. Generate only the required scaled raster variants and Windows icon from that approved source.
4. Update the existing embedded resource references in the host and installer.
5. Verify the logo at small header size, launcher scale and installer scale.
6. Keep a neutral fallback resource for missing/corrupt artwork.

Do not redraw, recolor, crop or stretch the source logo without approval.

## Non-regression requirements

- Preserve ReShade runtime APIs, technique state, uniform editing, preset format and hotkeys.
- Preserve compare mode, screenshots, save behavior, dialogs and status notices.
- Keep keyboard navigation and tooltips usable.
- Validate the layout at 1280x720, 1920x1080, 2560x1440 and ultrawide resolutions.
- A successful CI build is necessary but not sufficient: perform an in-game smoke test before declaring the redesign complete.
