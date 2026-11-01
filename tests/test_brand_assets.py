"""Validation of official DSL logo resizing and safe regeneration."""
from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

from PIL import Image

SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "prepare_brand_assets.py"
spec = importlib.util.spec_from_file_location("prepare_brand_assets", SCRIPT)
assert spec and spec.loader
brand = importlib.util.module_from_spec(spec)
spec.loader.exec_module(brand)


class BrandAssetsTests(unittest.TestCase):
    def test_dimensions_and_letterboxing(self):
        with tempfile.TemporaryDirectory() as root:
            directory = Path(root)
            source = directory / "base.png"
            original = Image.new("RGB", (1920, 1080), (18, 36, 54))
            original.save(source)
            output = directory / "brand"
            brand.prepare(source, output)
            self.assertEqual(set(p.name for p in output.iterdir()), set(brand.SIZES))
            for filename, expected in brand.SIZES.items():
                with Image.open(output / filename) as img:
                    self.assertEqual(img.size, expected)
                    self.assertEqual(img.mode, "RGB")
            with Image.open(output / "dsl-logo-square.png") as img:
                self.assertEqual(img.getpixel((0, 0)), (0, 0, 0))
                self.assertEqual(img.getpixel((256, 256)), (18, 36, 54))
                self.assertEqual(img.getpixel((256, 511)), (0, 0, 0))
            # Existing assets must never be silently overwritten.
            with self.assertRaises(FileExistsError):
                brand.prepare(source, output)

    def test_rejects_wrong_source_dimensions(self):
        with tempfile.TemporaryDirectory() as root:
            source = Path(root) / "wrong.png"
            Image.new("RGB", (640, 480)).save(source)
            with self.assertRaises(ValueError):
                brand.prepare(source, Path(root) / "out")


if __name__ == "__main__":
    unittest.main()
