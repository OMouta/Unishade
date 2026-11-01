#!/usr/bin/env python3
"""Generate reproducible, non-destructive DSL MODS brand images from official base.png.

Usage: python tools/prepare_brand_assets.py path/to/base.png
Requires: pip install Pillow
No images are generated or committed without the original user-provided artwork.
"""
from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image

SIZES = {
    "dsl-logo-splash.png": (1920, 1080),
    "dsl-logo-launcher.png": (960, 540),
    "dsl-logo-installer.png": (640, 360),
    "dsl-logo-preview.png": (480, 270),
    "dsl-logo-square.png": (512, 512),
}


def prepare(source: Path, destination: Path) -> None:
    if not source.is_file():
        raise FileNotFoundError(f"Official source image not found: {source}")
    destination.mkdir(parents=True, exist_ok=True)
    with Image.open(source) as image:
        image.load()
        if image.size != (1920, 1080):
            raise ValueError(f"Expected official 1920x1080 artwork; got {image.size}")
        if image.mode not in ("RGB", "RGBA"):
            raise ValueError(f"Unsupported color mode: {image.mode}")
        # Original black background is opaque, not a transparency mask.
        original = image.convert("RGB")
        for filename, size in SIZES.items():
            target = destination / filename
            if target.exists():
                raise FileExistsError(f"Refusing to overwrite existing asset: {target}")
            if size[0] == size[1]:
                # Square slots in ImGui must never stretch the 16:9 DSL artwork.
                scaled = Image.new("RGB", size, (0, 0, 0))
                fitting = original.resize((size[0], size[0] * 9 // 16), Image.Resampling.LANCZOS)
                scaled.paste(fitting, (0, (size[1] - fitting.height) // 2))
            else:
                scaled = original.resize(size, Image.Resampling.LANCZOS)
            scaled.save(target, format="PNG", optimize=True)
            print(f"{target}: {size[0]}x{size[1]}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Path to official base.png")
    parser.add_argument("--output", type=Path, default=Path("assets/brand"))
    args = parser.parse_args()
    prepare(args.source, args.output)
