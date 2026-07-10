#!/usr/bin/env python3
"""
make_synthetic_cube.py - reproducible FITS cubes for benchmark_visivo.py.

Generates a spectral cube of a requested float32 size (GB) containing
Gaussian noise (sigma=1) plus an injected ROTATING DISK source: a tilted-disk
velocity field with a Gaussian line per pixel, so the kinematic lasso has a
realistic, velocity-coherent object to grow (peak S/N ~ 12 at the centre).
The disk centre is the cube centre; the auto-seed of the benchmark script
(brightest pixel of the central channel) lands on it by construction.

The cube is written channel-block by channel-block, so generating a 30 GB
cube needs only a few hundred MB of RAM.

Usage:
    python3 make_synthetic_cube.py --gb 1  --out cube_1gb.fits
    python3 make_synthetic_cube.py --gb 8  --out cube_8gb.fits
    python3 make_synthetic_cube.py --gb 30 --out cube_30gb.fits
Reproducibility: fixed RNG seed (--rng, default 42).
"""
from __future__ import annotations

import argparse

import numpy as np
from astropy.io import fits


def dims_for(gb: float) -> tuple[int, int, int]:
    """Pick (nx, ny, nz) with nx=ny, nz ~= nx/2, totalling ~gb GB of float32."""
    vox = gb * 1e9 / 4.0
    nx = int(round((2.0 * vox) ** (1.0 / 3.0)))
    nx -= nx % 2
    nz = nx // 2
    return nx, nx, nz


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--gb", type=float, required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--rng", type=int, default=42)
    ap.add_argument("--peak-snr", type=float, default=12.0)
    args = ap.parse_args()

    nx, ny, nz = dims_for(args.gb)
    print(f"dims {nx} x {ny} x {nz}  = {nx*ny*nz*4/1e9:.2f} GB float32")

    # ---- disk model (evaluated per channel block) -------------------------
    x = np.arange(nx) - nx / 2.0
    y = np.arange(ny) - ny / 2.0
    X, Y = np.meshgrid(x, y)                       # (ny, nx)
    pa, inc = np.deg2rad(30.0), np.deg2rad(60.0)
    xr = X * np.cos(pa) + Y * np.sin(pa)
    yr = (-X * np.sin(pa) + Y * np.cos(pa)) / np.cos(inc)
    r = np.hypot(xr, yr)
    r_disk = 0.30 * nx / 2.0                        # disk radius in pixels
    inside = r < r_disk
    with np.errstate(invalid="ignore", divide="ignore"):
        cos_th = np.where(r > 0, xr / r, 0.0)
    v_sys = nz / 2.0                                # channels
    v_rot = 0.30 * nz / 2.0                         # rotation amplitude, channels
    v_los = v_sys + v_rot * np.sin(inc) * cos_th    # line centre per pixel
    line_sigma = max(2.0, nz / 60.0)                # channels
    amp = args.peak_snr * np.exp(-r / r_disk) * inside  # radial falloff

    header = fits.Header()
    header["SIMPLE"], header["BITPIX"] = True, -32
    header["NAXIS"], header["NAXIS1"] = 3, nx
    header["NAXIS2"], header["NAXIS3"] = ny, nz
    header["BUNIT"] = "Jy/beam"
    header["CTYPE1"], header["CRPIX1"] = "RA---SIN", nx / 2
    header["CRVAL1"], header["CDELT1"] = 180.0, -2.0e-3
    header["CTYPE2"], header["CRPIX2"] = "DEC--SIN", ny / 2
    header["CRVAL2"], header["CDELT2"] = 0.0, 2.0e-3
    header["CTYPE3"], header["CRPIX3"] = "VRAD", nz / 2
    header["CRVAL3"], header["CDELT3"] = 0.0, 5.0e3   # 5 km/s channels
    header["CUNIT3"] = "m/s"
    header["BMAJ"] = header["BMIN"] = 8.0e-3
    header["BPA"] = 0.0

    rng = np.random.default_rng(args.rng)
    hdu = fits.StreamingHDU(args.out, header)
    block = max(1, min(64, nz))
    for z0 in range(0, nz, block):
        z1 = min(nz, z0 + block)
        zc = np.arange(z0, z1, dtype=np.float32)[:, None, None]
        sig = (amp[None, :, :]
               * np.exp(-0.5 * ((zc - v_los[None, :, :]) / line_sigma) ** 2))
        data = rng.standard_normal((z1 - z0, ny, nx), dtype=np.float32) + sig
        hdu.write(data.astype(np.float32))
        print(f"  channels {z0}..{z1 - 1} written", end="\r")
    hdu.close()
    print(f"\nwrote {args.out}")
    print(f"disk: centre pixel ({nx//2},{ny//2}), radius {r_disk:.0f} px, "
          f"PA 30 deg, inc 60 deg, Vrot {v_rot:.0f} channels")


if __name__ == "__main__":
    main()
