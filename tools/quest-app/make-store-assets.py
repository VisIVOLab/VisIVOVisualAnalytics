#!/usr/bin/env python3
"""Makes the Meta Horizon Store's image assets for VisIVO VR (issue #386).

Usage: make-store-assets.py [out_dir]   (default: tools/quest-app/out/store)

Needs Pillow and numpy (the backend's virtual environment has both):

    backend/.venv/bin/python tools/quest-app/make-store-assets.py

Writes the sizes of Meta's asset guidelines
(https://developers.meta.com/horizon/resources/asset-guidelines/), each a
24-bit PNG unless said otherwise:

| File | Size | Dashboard field |
|---|---|---|
| hero-3000x900.png | 3000x900 | Hero |
| cover-landscape-2560x1440.png | 2560x1440 | Cover Landscape |
| cover-square-1440x1440.png | 1440x1440 | Cover Square |
| cover-portrait-1008x1440.png | 1008x1440 | Cover Portrait |
| mini-landscape-1080x360.png | 1080x360 | Mini Landscape |
| logo-transparent.png | 2387x1440, 32-bit with transparency | Logo |
| icon-512x512.png | 512x512, square corners, solid | Icon |

The art is the VisIVO logo (store/logo-white.png, the white logo of
docs/_static/visivo_logo_white.svg rasterised at 2400 px) over a dark
background with a soft glow made from a seeded random field (no data set:
nothing to credit), the logo centred, clear of the top and bottom 20% that
Meta asks to keep free of text. The icon is icon-maskable-512.png (the
launcher's, a full white square) without transparency. The same seed gives
the same images. Screenshots are not made here: they come from a headset.
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
LOGO = HERE / "store" / "logo-white.png"
ICON = HERE / "icon-maskable-512.png"
BACKGROUND = np.array([16, 20, 24], dtype=np.float64)       # app.json themeColor #101418
SEED = 386
# An inferno-like ramp (black, purple, red, orange, pale yellow), the colour
# map VisIVO's cubes open with in VR.
RAMP = np.array([[0, 0, 4], [40, 11, 84], [101, 21, 110], [159, 42, 99], [212, 72, 66],
                 [245, 125, 21], [250, 193, 39], [252, 255, 164]], dtype=np.float64)

# name: (width, height, logo height as a share of the image height)
COVERS = {
    "hero-3000x900.png": (3000, 900, 0.50),
    "cover-landscape-2560x1440.png": (2560, 1440, 0.50),
    "cover-square-1440x1440.png": (1440, 1440, 0.36),
    "cover-portrait-1008x1440.png": (1008, 1440, 0.34),
    "mini-landscape-1080x360.png": (1080, 360, 0.56),
}
LOGO_MAX_HEIGHT = 1440


def ramp(t: np.ndarray) -> np.ndarray:
    """t in [0, 1] → RGB through RAMP."""
    x = np.clip(t, 0, 1) * (len(RAMP) - 1)
    i = np.minimum(x.astype(int), len(RAMP) - 2)
    f = (x - i)[..., None]
    return RAMP[i] * (1 - f) + RAMP[i + 1] * f


def glow(width: int, height: int, seed: int = SEED) -> np.ndarray:
    """A soft, gas-like glow: a random field with a steep power spectrum,
    brightest off-centre, fading to the background at the edges. RGB float."""
    rng = np.random.default_rng(seed)
    # The field at the image's own aspect (short side 384 cells), so a wide
    # banner is not a stretched square.
    scale = 384 / min(width, height)
    ny, nx = max(8, round(height * scale)), max(8, round(width * scale))
    ky, kx = np.meshgrid(np.fft.fftfreq(ny), np.fft.fftfreq(nx), indexing="ij")
    k = np.hypot(kx, ky)
    k[0, 0] = 1.0
    amp = k ** -1.8
    amp[0, 0] = 0.0
    field = np.real(np.fft.ifft2(amp * np.exp(2j * np.pi * rng.random((ny, nx)))))
    field = (field - field.mean()) / field.std()
    small = Image.fromarray(field.astype(np.float32), mode="F")
    field = np.asarray(small.resize((width, height), Image.BICUBIC), dtype=np.float64)
    y, x = np.mgrid[0:height, 0:width]
    # The glow sits right of centre and fades out radially (in units of the short side).
    s = min(width, height)
    r = np.hypot((x - 0.62 * width) / s, (y - 0.5 * height) / s)
    envelope = np.exp(-(r / 0.55) ** 2)
    density = np.clip(0.5 + 0.22 * field, 0, 1) * envelope
    # Capped below the ramp's pale yellow: the white logo stays readable on it.
    colour = ramp(0.78 * density ** 1.2)
    a = np.clip(density * 1.6, 0, 1)[..., None]
    return BACKGROUND * (1 - a) + colour * a


def logo(height: int) -> Image.Image:
    im = Image.open(LOGO).convert("RGBA")
    im = im.crop(im.getbbox())
    return im.resize((round(im.width * height / im.height), height), Image.LANCZOS)


def cover(width: int, height: int, share: float) -> Image.Image:
    base = Image.fromarray(np.clip(glow(width, height), 0, 255).astype(np.uint8), "RGB").convert("RGBA")
    mark = logo(round(height * share))
    if mark.width > 0.8 * width:
        mark = mark.resize((round(0.8 * width), round(mark.height * 0.8 * width / mark.width)), Image.LANCZOS)
    base.alpha_composite(mark, ((width - mark.width) // 2, (height - mark.height) // 2))
    return base.convert("RGB")


def main(argv: list[str]) -> int:
    out = Path(argv[1]) if len(argv) > 1 else HERE / "out" / "store"
    out.mkdir(parents=True, exist_ok=True)
    for name, (w, h, share) in COVERS.items():
        im = cover(w, h, share)
        assert im.size == (w, h) and im.mode == "RGB"
        im.save(out / name, optimize=True)
    mark = logo(LOGO_MAX_HEIGHT)
    mark.save(out / "logo-transparent.png", optimize=True)
    icon = Image.open(ICON).convert("RGBA")
    solid = Image.new("RGBA", icon.size, (255, 255, 255, 255))
    solid.alpha_composite(icon)
    solid.convert("RGB").resize((512, 512), Image.LANCZOS).save(out / "icon-512x512.png", optimize=True)
    for f in sorted(out.glob("*.png")):
        with Image.open(f) as im:
            print(f"{f.name}: {im.width}x{im.height} {im.mode}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
