# DSL MODS Visual Engine — Official base.png integration

The user supplied the official 1920×1080 RGB black-background DSL chrome logo artwork (DSL lettering with barbed-wire details and a sports car). Preserve the original artwork, aspect ratio, colors and car details. Do not generate a replacement logo.

## Asset pipeline

- Source: `base.png` (user-supplied, 1920×1080, RGB).
- Desktop splash: 1920×1080.
- Launcher: 960×540.
- Installer: 640×360.
- Preview: 480×270.
- Square slots: 512×512 with black letterboxing (do not stretch the art).
- Resize with high-quality Lanczos; preserve 16:9 aspect ratio.
- The background is opaque black; **do not treat it as transparent** or use alpha blending that expects transparency.
- Use `ImGui::Image` or the existing resource texture path with a 16:9 fitted rectangle and no cropping.
- Keep existing resources (`assets/Unishade.png`, `assets/Unishade.ico`, `src/Unishade.rc`, `installer/setup.rc`) unchanged until the actual binary assets are committed and verified.
- Avoid embedding the full 1920×1080 artwork as a small toolbar icon: use a legible DSL wordmark or neutral fallback instead.
- Validate rendering at 1280×720, 1920×1080 and 2560×1440, including Windows DPI scaling.
- Ensure binary asset licensing and branding are documented; preserve third-party upstream licenses and attribution.

## Reproducible generation

Run `python -m pip install Pillow` and then `python tools/prepare_brand_assets.py path/to/base.png --output assets/brand`. The script checks the official 1920×1080 dimensions and refuses to overwrite existing outputs. Keep `base.png` private until distribution rights and publication are confirmed.

## Installation behavior

The Windows setup now looks for `assets/brand/dsl-logo-square.png` next to its own executable. When it is a regular PNG file with a valid PNG signature and reasonable size (up to 12 MiB), Setup stages it alongside the program. The existing installation commit automatically records this asset in its file manifest, so uninstall removes it. If the optional image is unavailable or invalid, the program continues to use its embedded fallback logo.

The DSL Windows distribution workflow copies `assets/brand/*.png` into the ZIP when the official artwork has been added to the source tree. **The setup executable by itself does not currently contain the official bitmap**; keep the ZIP's folder structure intact when extracting.

## Integration status

The official artwork has been provided and optimized externally. The launcher, overlay and setup now support the optional `dsl-logo-square.png`, and Setup can copy it into the installation. **The actual PNG files are not yet committed to the repository**; therefore the current automated GitHub ZIP cannot be assumed to include them. A successful build proves compilation and automated tests, not visual quality in a running game.
