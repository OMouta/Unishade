# DSL MODS Visual Engine — Official base.png integration

The user supplied the official 1920×1080 RGB black-background DSL chrome logo artwork (DSL lettering with barbed-wire details and a sports car). Preserve the original artwork, aspect ratio, colors and car details. Do not generate a replacement logo.

## Asset pipeline

- Source: `base.png` (user-supplied, 1920×1080, RGB).
- Desktop splash: 1920×1080.
- Launcher: 960×540.
- Installer: 640×360.
- Preview: 480×270.
- Resize with high-quality Lanczos; preserve 16:9 aspect ratio.
- The background is opaque black; **do not treat it as transparent** or use alpha blending that expects transparency.
- Use `ImGui::Image` or the existing resource texture path with a 16:9 fitted rectangle and no cropping.
- Keep existing resources (`assets/Unishade.png`, `assets/Unishade.ico`, `src/Unishade.rc`, `installer/setup.rc`) unchanged until the actual binary assets are committed and verified.
- Avoid embedding the full 1920×1080 artwork as a small toolbar icon: use a legible DSL wordmark or neutral fallback instead.
- Validate rendering at 1280×720, 1920×1080 and 2560×1440, including Windows DPI scaling.
- Ensure binary asset licensing and branding are documented; preserve third-party upstream licenses and attribution.

## Integration status

The official source image was received and optimized variants were prepared externally. The binary artwork is **not yet committed to this GitHub repository**. The UI remains logo-ready with existing fallback resources. Do not claim the image is integrated into the app until binary files, resource references, CI and runtime rendering have been verified.
