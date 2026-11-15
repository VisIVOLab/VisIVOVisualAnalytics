#!/usr/bin/env python3
"""Makes the headset app's launcher icons from the VisIVO logo.

Usage: make-icons.py [logo.png]   (default: icons/VisIVO_512.png)

Writes, beside this script, two 512x512 PNGs that app.json names:

* icon-512.png: the logo on a white rounded square (Android's "any"
  purpose: shown as it is);
* icon-maskable-512.png: the logo on a full white square, scaled so that
  every visible pixel lies inside the centre circle of 80% of the width
  (Android's "maskable" safe zone: the launcher may crop the rest to any
  shape).

White, because the logo's text is dark blue: on a transparent background it
would vanish against Horizon OS's dark panels. The logo file itself is never
changed. Needs Pillow; the PNGs are committed, so a build does not.
"""
from __future__ import annotations

import math
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
SIZE = 512
BACKGROUND = (255, 255, 255, 255)
#: The plain icon: the logo fills this share of the square's width.
PLAIN_FILL = 0.88
#: The plain icon's corner radius, as a share of the width.
PLAIN_RADIUS = 0.18
#: Android's maskable safe zone: a centred circle of 80% of the width.
SAFE_DIAMETER = 0.80
#: Alpha below this is not part of the logo (antialiasing haze).
ALPHA_FLOOR = 8
#: Every channel at or above this is the logo's own white background
#: (VisIVO_512.png is opaque white around the drawing), not ink.
WHITE_FLOOR = 245


def _ink(img: Image.Image) -> Image.Image:
    """255 where ``img`` (RGBA) has a visible, non-white pixel, else 0."""
    r, g, b, a = img.split()
    visible = a.point(lambda v: 255 if v >= ALPHA_FLOOR else 0)
    light = [c.point(lambda v: 255 if v >= WHITE_FLOOR else 0) for c in (r, g, b)]
    white = ImageChops.multiply(ImageChops.multiply(light[0], light[1]), light[2])
    return ImageChops.multiply(visible, ImageChops.invert(white))


def _logo(path: Path) -> Image.Image:
    """The logo cropped to its ink."""
    img = Image.open(path).convert("RGBA")
    box = _ink(img).getbbox()
    if box is None:
        raise SystemExit(f"{path} has no visible pixels")
    return img.crop(box)


def _reach(logo: Image.Image) -> float:
    """The farthest visible pixel's distance from the logo's centre, in
    pixels: what must fit inside the safe circle (the logo is an ellipse, so
    its bounding box's corners are empty and fitting the box would shrink it
    for nothing)."""
    w, h = logo.size
    cx, cy = w / 2.0, h / 2.0
    ink = _ink(logo).load()
    reach = 0.0
    for y in range(h):
        for x in range(w):
            if ink[x, y]:
                # The pixel's farthest corner, not its centre.
                dx = max(abs(x - cx), abs(x + 1 - cx))
                dy = max(abs(y - cy), abs(y + 1 - cy))
                reach = max(reach, math.hypot(dx, dy))
    return reach


def _place(canvas: Image.Image, logo: Image.Image, scale: float) -> Image.Image:
    w, h = (max(1, round(d * scale)) for d in logo.size)
    resized = logo.resize((w, h), Image.LANCZOS)
    canvas.alpha_composite(resized, ((SIZE - w) // 2, (SIZE - h) // 2))
    return canvas


def plain_icon(logo: Image.Image) -> Image.Image:
    canvas = _place(Image.new("RGBA", (SIZE, SIZE), BACKGROUND), logo, SIZE * PLAIN_FILL / max(logo.size))
    # The corners are cut last, so the logo's own white background never
    # shows past them.
    corners = Image.new("L", (SIZE, SIZE), 0)
    ImageDraw.Draw(corners).rounded_rectangle((0, 0, SIZE - 1, SIZE - 1),
                                              radius=round(SIZE * PLAIN_RADIUS), fill=255)
    canvas.putalpha(ImageChops.multiply(canvas.getchannel("A"), corners))
    return canvas


def maskable_icon(logo: Image.Image) -> Image.Image:
    canvas = Image.new("RGBA", (SIZE, SIZE), BACKGROUND)
    # The one-pixel margin absorbs rounding of the resized size.
    scale = (SIZE * SAFE_DIAMETER / 2.0 - 1.0) / _reach(logo)
    return _place(canvas, logo, scale)


def main(argv: list[str]) -> int:
    if len(argv) > 2:
        print(__doc__, file=sys.stderr)
        return 2
    src = Path(argv[1]) if len(argv) == 2 else REPO / "icons" / "VisIVO_512.png"
    logo = _logo(src)
    # optimize=False and no metadata: the same logo gives the same bytes.
    plain_icon(logo).save(HERE / "icon-512.png", optimize=False)
    maskable_icon(logo).convert("RGB").save(HERE / "icon-maskable-512.png", optimize=False)
    print(f"{HERE / 'icon-512.png'}\n{HERE / 'icon-maskable-512.png'}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
