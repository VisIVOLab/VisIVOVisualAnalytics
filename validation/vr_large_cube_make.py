"""A synthetic spectral cube large enough for the backend to draw it out of
core (part of epic #166), for validation/vr_large_cube.mjs.

Gaussian noise (sigma 1) with a few rotating discs (a velocity gradient along
their major axis) and a blank (NaN) border of channels and columns, written
plane by plane through a memory map, so the cube never sits in memory:

    python validation/vr_large_cube_make.py OUT.fits [--nx 2400] [--ny 2400] [--nz 520]

2400 x 2400 x 520 float32 is 11.2 GiB, above the default in-core limit of
6 GiB (VISIVO_RENDER_OUT_OF_CORE_BYTES). Write it to a scratch directory and
delete it after use.
"""
from __future__ import annotations

import argparse
import math
import time

import numpy as np
from astropy.io import fits


def discs(nx: int, ny: int, nz: int, rng):
    out = []
    for k in range(6):
        out.append({
            "x": float(rng.uniform(0.15, 0.85) * nx), "y": float(rng.uniform(0.15, 0.85) * ny),
            "z": float(rng.uniform(0.3, 0.7) * nz), "r": float(rng.uniform(0.02, 0.06) * nx),
            "pa": float(rng.uniform(0, math.pi)), "dv": float(rng.uniform(0.05, 0.12) * nz),
            "sv": float(rng.uniform(3, 8)), "peak": float(rng.uniform(4, 25)),
        })
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--nx", type=int, default=2400)
    ap.add_argument("--ny", type=int, default=2400)
    ap.add_argument("--nz", type=int, default=520)
    a = ap.parse_args()
    nx, ny, nz = a.nx, a.ny, a.nz
    hdr = fits.Header()
    hdr["SIMPLE"] = True
    hdr["BITPIX"] = -32
    hdr["NAXIS"] = 3
    hdr["NAXIS1"], hdr["NAXIS2"], hdr["NAXIS3"] = nx, ny, nz
    for k, v in {"CTYPE1": "RA---SIN", "CTYPE2": "DEC--SIN", "CTYPE3": "VRAD",
                 "CRPIX1": nx / 2, "CRPIX2": ny / 2, "CRPIX3": 1.0,
                 "CRVAL1": 150.0, "CRVAL2": -30.0, "CRVAL3": 500000.0,
                 "CDELT1": -6.0 / 3600, "CDELT2": 6.0 / 3600, "CDELT3": 4000.0,
                 "CUNIT1": "deg", "CUNIT2": "deg", "CUNIT3": "m/s", "SPECSYS": "BARYCENT",
                 "BUNIT": "Jy/beam", "OBJECT": "synthetic large cube"}.items():
        hdr[k] = v
    header_bytes = hdr.tostring().encode("ascii")
    data_bytes = nx * ny * nz * 4
    pad = (-data_bytes) % 2880
    with open(a.out, "wb") as f:
        f.write(header_bytes)
        f.truncate(len(header_bytes) + data_bytes + pad)
    cube = np.memmap(a.out, dtype=">f4", mode="r+", offset=len(header_bytes), shape=(nz, ny, nx))
    rng = np.random.default_rng(166)
    gal = discs(nx, ny, nz, rng)
    yy, xx = np.mgrid[0:ny, 0:nx].astype(np.float32)
    t0 = time.time()
    for z in range(nz):
        plane = rng.standard_normal((ny, nx), dtype=np.float32)
        for g in gal:
            x0, x1 = int(max(0, g["x"] - 3 * g["r"])), int(min(nx, g["x"] + 3 * g["r"]))
            y0, y1 = int(max(0, g["y"] - 3 * g["r"])), int(min(ny, g["y"] + 3 * g["r"]))
            if x1 <= x0 or y1 <= y0:
                continue
            dx, dy = xx[y0:y1, x0:x1] - g["x"], yy[y0:y1, x0:x1] - g["y"]
            major = dx * math.cos(g["pa"]) + dy * math.sin(g["pa"])
            rr = np.hypot(dx, dy)
            vlos = g["z"] + g["dv"] * np.tanh(major / (0.3 * g["r"]))
            plane[y0:y1, x0:x1] += (g["peak"] * np.exp(-0.5 * (rr / g["r"]) ** 2)
                                    * np.exp(-0.5 * ((z - vlos) / g["sv"]) ** 2))
        plane[:, :16] = np.nan                   # a blank border of columns
        if z < 4:
            plane[:] = np.nan                    # and of channels
        cube[z] = plane
        if z % 64 == 0:
            print(f"plane {z}/{nz}  {time.time() - t0:.0f} s", flush=True)
    cube.flush()
    del cube
    print(f"wrote {a.out}: {nx} x {ny} x {nz} float32, {data_bytes / 2**30:.2f} GiB "
          f"in {time.time() - t0:.0f} s")


if __name__ == "__main__":
    main()
