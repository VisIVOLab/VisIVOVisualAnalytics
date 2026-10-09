#!/usr/bin/env python3
"""Build icons/VisIVO.icns from the square artwork.

macOS does NOT round an application's icon — unlike iOS, what the bundle ships
is what the Dock draws. Ours was a full-bleed opaque square, so VisIVO sat in
the Dock as a white rectangle among everyone else's rounded ones (user report).

This renders Apple's shape explicitly: a continuous-corner rounded rectangle
("squircle") occupying 824/1024 of the canvas, the proportion of the macOS
icon grid, with the artwork inset inside it and everything outside transparent.

    backend/.venv/bin/python deploy/macos/make_icon.py

Needs Pillow and macOS's own `iconutil`. Re-run it when the artwork changes;
the result is committed, so a normal build does not depend on either.
"""
from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw
except ImportError:  # a clearer answer than a traceback from the import line
    print("Pillow is required: pip install pillow "
          "(the repository's backend venv already has it)")
    raise SystemExit(2) from None

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "icons" / "VisIVO_512.png"
TARGET = ROOT / "icons" / "VisIVO.icns"

CANVAS = 1024
#: The macOS icon grid: the rounded rect is 824 px wide on a 1024 px canvas with
#: a 185 px corner radius. Everything outside it is transparent, which is what
#: gives the Dock its consistent silhouette.
#:
#: The corners here are CIRCULAR, not Apple's continuous-corner curve — Pillow
#: has no squircle primitive and the difference is a few pixels of curvature at
#: the corners, invisible at Dock sizes. If it ever matters, the real shape is
#: in Apple's icon template and this is where to swap it in.
SHAPE = 824
RADIUS = 185
#: The artwork sits inside the shape rather than against its edges.
ART_INSET = 56


def squircle(size: int, radius: int, colour: tuple[int, int, int, int]) -> Image.Image:
    """A rounded rectangle, drawn at 4× and downsampled so the corners are clean."""
    scale = 4
    mask = Image.new("L", (size * scale, size * scale), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (0, 0, size * scale - 1, size * scale - 1), radius=radius * scale, fill=255)
    mask = mask.resize((size, size), Image.LANCZOS)
    shape = Image.new("RGBA", (size, size), colour)
    shape.putalpha(mask)
    return shape


def build() -> Image.Image:
    art = Image.open(SOURCE).convert("RGBA")
    # The artwork's own background is its corner pixel: the shape is filled with
    # it so the logo does not sit on a colour it was not drawn for.
    background = art.getpixel((0, 0))
    icon = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))
    shape = squircle(SHAPE, RADIUS, background)

    inner = SHAPE - 2 * ART_INSET
    art = art.resize((inner, inner), Image.LANCZOS)
    shape.alpha_composite(art, (ART_INSET, ART_INSET))

    offset = (CANVAS - SHAPE) // 2
    icon.alpha_composite(shape, (offset, offset))
    return icon


def main() -> int:
    if not SOURCE.exists():
        print(f"artwork not found: {SOURCE}")
        return 2
    if not shutil.which("iconutil"):
        print("iconutil not found — this script only runs on macOS")
        return 2

    icon = build()
    iconset = ROOT / "icons" / "VisIVO.iconset"
    if iconset.exists():
        shutil.rmtree(iconset)
    iconset.mkdir()
    # Every size the Finder, the Dock and Get Info ask for.
    for size in (16, 32, 128, 256, 512):
        icon.resize((size, size), Image.LANCZOS).save(iconset / f"icon_{size}x{size}.png")
        icon.resize((size * 2, size * 2), Image.LANCZOS).save(
            iconset / f"icon_{size}x{size}@2x.png")

    subprocess.run(["iconutil", "-c", "icns", str(iconset), "-o", str(TARGET)], check=True)
    shutil.rmtree(iconset)
    print(f"wrote {TARGET.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
