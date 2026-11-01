#!/usr/bin/env python3
"""
make_synthetic_cube.py - a float32 FITS cube of any size, written plane by
plane: unit Gaussian noise plus 40 Gaussian sources (seeded), for measuring
views of cubes larger than a GPU's memory.

Usage:

    python3 make_synthetic_cube.py OUT.fits [NZ NY NX]   # default 1500 2048 2048 (23.4 GiB)
"""
from __future__ import annotations

import os
import sys

import numpy as np
from astropy.io import fits


def main() -> None:
    if len(sys.argv) not in (2, 5):
        raise SystemExit("usage: make_synthetic_cube.py OUT.fits [NZ NY NX]")
    out = sys.argv[1]
    nz, ny, nx = (int(v) for v in sys.argv[2:5]) if len(sys.argv) == 5 else (1500, 2048, 2048)
    if min(nz, ny, nx) < 1:
        raise SystemExit("the sizes must be positive")
    if os.path.exists(out):
        # StreamingHDU would append a second HDU and the viewer opens the first.
        raise SystemExit(f"{out} exists: remove it first")
    rng = np.random.default_rng(70)
    # z, y, x, sigma (voxels), peak (noise sigmas)
    sources = [(rng.uniform(0, nz), rng.uniform(0, ny), rng.uniform(0, nx),
                rng.uniform(8, 40), rng.uniform(3, 12)) for _ in range(40)]

    header = fits.Header()
    header["SIMPLE"] = True
    header["BITPIX"] = -32
    header["NAXIS"] = 3
    header["NAXIS1"], header["NAXIS2"], header["NAXIS3"] = nx, ny, nz
    stream = fits.StreamingHDU(out, header)
    yy, xx = np.mgrid[0:ny, 0:nx].astype(np.float32)
    for z in range(nz):
        plane = rng.standard_normal((ny, nx), dtype=np.float32)
        for sz, sy, sx, sigma, peak in sources:
            dz = (z - sz) / sigma
            if abs(dz) > 4:
                continue
            y0, y1 = max(0, int(sy - 4 * sigma)), min(ny, int(sy + 4 * sigma) + 1)
            x0, x1 = max(0, int(sx - 4 * sigma)), min(nx, int(sx + 4 * sigma) + 1)
            r2 = ((yy[y0:y1, x0:x1] - sy) ** 2 + (xx[y0:y1, x0:x1] - sx) ** 2) / sigma ** 2
            plane[y0:y1, x0:x1] += peak * np.exp(-0.5 * (r2 + dz * dz))
        stream.write(plane[np.newaxis])
        if z % 100 == 0:
            print(f"plane {z} of {nz}", flush=True)
    stream.close()
    print("written", out)


if __name__ == "__main__":
    main()
