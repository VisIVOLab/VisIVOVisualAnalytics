#!/usr/bin/env python3
"""Generate synthetic demo datasets for testing every VisIVO-next viewer feature.

Each dataset is small, self-contained, and targets a specific feature so a
reviewer can exercise the app end-to-end without hunting for real data. Run:

    backend/.venv/bin/python backend/scripts/generate_demo_data.py [OUTPUT_DIR]

Default OUTPUT_DIR is ~/VisIVO_demo_data (the desktop file browser starts at the
home directory, so it's immediately reachable). See docs/demo-datasets.md for the
feature → dataset → test-steps matrix.

The output is fully reproducible from this script (fixed RNG seed), so the script
itself is the "downloadable" artifact — commit it and regenerate anywhere.
"""
from __future__ import annotations

import csv
import sys
from pathlib import Path

import numpy as np
from astropy.io import fits
from astropy.table import Table

RNG = np.random.default_rng(20260701)

# HI 21 cm rest frequency (Hz) — used for the demo cube's spectral WCS.
HI_RESTFRQ = 1_420_405_751.786


# ── 1. 3-D catalogue — sky (RA/Dec/redshift) ──────────────────────────────────
def make_catalogue_sky(out: Path) -> Path:
    """Sky catalogue: RA/Dec + redshift + morphology/size/flux.

    Exercises: 3-D catalogue viewer sky path, cosmology model selector,
    FK5↔Galactic frame, morphology colour, size-by-major-axis, filters.
    """
    n = 2500
    ra = RNG.uniform(150.0, 210.0, n)          # a ~60° patch
    dec = RNG.uniform(-10.0, 40.0, n)
    z = RNG.uniform(0.02, 0.30, n)
    flux = 10.0 ** RNG.normal(1.0, 0.5, n)     # mJy
    major = RNG.uniform(0.5, 8.0, n)           # arcmin
    minor = major * RNG.uniform(0.4, 1.0, n)
    morph_kinds = ["halo", "relic", "mini-halo", "double relic",
                   "phoenix", "candidate", "diffuse"]
    morph = RNG.choice(morph_kinds, n)

    path = out / "catalogue_sky.csv"
    with path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["name", "ra", "dec", "z", "flux", "majoraxis", "minoraxis",
                    "morphology"])
        for i in range(n):
            w.writerow([f"SRC{i:04d}", f"{ra[i]:.5f}", f"{dec[i]:.5f}",
                        f"{z[i]:.4f}", f"{flux[i]:.3f}", f"{major[i]:.3f}",
                        f"{minor[i]:.3f}", morph[i]])
    return path


# ── 2. 3-D catalogue — Cartesian (X/Y/Z Mpc + velocities) ─────────────────────
def _clustered_points(n_clusters: int, per_cluster: int, n_field: int,
                      lo: float, hi: float, sigma: float) -> np.ndarray:
    """A few Gaussian clusters plus a uniform field, clipped to [lo, hi]."""
    parts = []
    for _ in range(n_clusters):
        ctr = RNG.uniform(lo * 0.7, hi * 0.7, size=3)
        parts.append(ctr + RNG.normal(0, sigma, size=(per_cluster, 3)))
    parts.append(RNG.uniform(lo, hi, size=(n_field, 3)))
    return np.clip(np.vstack(parts), lo, hi)


def make_catalogue_cartesian(out: Path) -> Path:
    """Cartesian catalogue: X/Y/Z Mpc + vx/vy/vz + mass.

    Exercises: Cartesian catalogue path, Velocity arrows shape, Speed |v| colour,
    raw-column colour, radial distance.
    """
    pos = _clustered_points(4, 3000, 5000, -90.0, 90.0, 10.0)
    vel = -300.0 * pos / 90.0 + RNG.normal(0, 40, size=pos.shape)  # infall + noise
    mass = 10.0 ** RNG.normal(11.0, 0.4, len(pos))

    path = out / "catalogue_cartesian.csv"
    with path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["x", "y", "z", "vx", "vy", "vz", "mass"])
        for i in range(len(pos)):
            w.writerow([f"{pos[i, 0]:.4f}", f"{pos[i, 1]:.4f}", f"{pos[i, 2]:.4f}",
                        f"{vel[i, 0]:.3f}", f"{vel[i, 1]:.3f}", f"{vel[i, 2]:.3f}",
                        f"{mass[i]:.3e}"])
    return path


# ── 3. Partiview .speck ───────────────────────────────────────────────────────
def make_speck(out: Path) -> Path:
    """Partiview .speck point cloud (X Y Z + datavars).

    Exercises: .speck import path (Cartesian), datavar attribute colour.
    """
    pos = _clustered_points(3, 2000, 3000, -80.0, 80.0, 9.0)
    lum = 10.0 ** RNG.normal(9.0, 0.5, len(pos))
    temp = RNG.uniform(3000, 12000, len(pos))

    path = out / "catalogue.speck"
    with path.open("w") as f:
        f.write("# VisIVO demo Partiview .speck (Cartesian Mpc)\n")
        f.write("datavar 0 luminosity\n")
        f.write("datavar 1 temperature\n")
        for i in range(len(pos)):
            f.write(f"{pos[i, 0]:.4f} {pos[i, 1]:.4f} {pos[i, 2]:.4f} "
                    f"{lum[i]:.4e} {temp[i]:.1f} # g{i}\n")
    return path


# ── 4. IPAC .tbl snapshot subtables (multi-file) ──────────────────────────────
def make_ipac_snapshot(out: Path) -> list[Path]:
    """Three IPAC .tbl subtables with identical columns (multi-file load).

    Exercises: .tbl (IPAC) format, Cartesian path, multi-file concatenation,
    Load all / Load more paging, Speed |v| + velocity arrows.
    """
    sub_dir = out / "tbl_snapshot"
    sub_dir.mkdir(parents=True, exist_ok=True)
    paths: list[Path] = []
    # 2×2×2 spatial blocks, each a sub-box of a 100 Mpc snapshot.
    blocks = [(bx, by, bz) for bx in (0, 1) for by in (0, 1) for bz in (0, 1)][:3]
    for (bx, by, bz) in blocks:
        n = 4000
        ox, oy, oz = bx * 50.0, by * 50.0, bz * 50.0
        x = RNG.uniform(ox, ox + 50.0, n)
        y = RNG.uniform(oy, oy + 50.0, n)
        z = RNG.uniform(oz, oz + 50.0, n)
        ctr = np.array([50.0, 50.0, 50.0])
        p = np.column_stack([x, y, z])
        vel = -250.0 * (p - ctr) / 50.0 + RNG.normal(0, 35, size=p.shape)
        logmass = RNG.normal(11.0, 0.35, n)
        tbl = Table({
            "x": x, "y": y, "z": z,
            "vx": vel[:, 0], "vy": vel[:, 1], "vz": vel[:, 2],
            "LogMass": logmass,
        })
        path = sub_dir / f"snap_demo_{bx + 1}_{by + 1}_{bz + 1}.tbl"
        tbl.write(path, format="ascii.ipac", overwrite=True)
        paths.append(path)
    return paths


# ── 5. Velocity field FITS (glyphs / streamlines / basins) ────────────────────
def make_velocity_field(out: Path, name: str, box: float,
                        sinks: list[np.ndarray],
                        sources: list[np.ndarray] | None = None) -> Path:
    """3-component velocity field superposing infall onto `sinks` (attractors)
    and outflow from `sources` (repellers).

    Exercises: Velocity Field viewer (arrows/streamlines), Show basins (attraction
    → sinks / repulsion → sources), Overdensity (−∇·v, at sinks) and Void (+∇·v,
    at sources), Box size (Mpc), colormap. (LOD/ROI only kick in above 128³ — kept
    at 64³ here so the download stays small.)
    """
    sources = sources or []
    n = 64
    half = box / 2.0
    c = np.linspace(-half, half, n)
    Z, Y, X = np.meshgrid(c, c, c, indexing="ij")      # arrays are [z, y, x]
    P = np.stack([X, Y, Z], axis=-1)                    # (n, n, n, 3)

    # Superpose a unit infall toward each sink and a unit outflow from each
    # source, each with a tanh speed ramp (≈0 at the feature, rising outward).
    def _contribution(feature, sign):
        d = P - feature
        r = np.linalg.norm(d, axis=-1, keepdims=True)
        return sign * (d / np.maximum(r, 1e-6)) * np.tanh(r / (0.3 * half))

    vec = np.zeros_like(P)
    for s in sinks:
        vec += _contribution(s, -1.0)     # toward the sink (convergent → overdensity)
    for s in sources:
        vec += _contribution(s, +1.0)     # away from the source (divergent → void)

    amp = 400.0
    cube = np.stack([amp * vec[..., 0], amp * vec[..., 1], amp * vec[..., 2]],
                    axis=0).astype(np.float32)

    hdu = fits.PrimaryHDU(cube)
    hdu.header["BUNIT"] = "km/s"
    hdu.header["VFRAME"] = "supergalactic"
    path = out / name
    hdu.writeto(path, overwrite=True)
    return path


# ── 6. CosmicFlows combo: co-registered field + galaxy catalogue ──────────────
def make_combo(out: Path) -> tuple[Path, Path]:
    """A velocity field + a galaxy catalogue in the SAME 200 Mpc frame.

    Exercises: Overlay galaxy catalogue, Galaxy color/scalar bar, Velocity
    arrows (galaxy peculiar velocity vs the field), Galaxy density isosurface.
    """
    combo = out / "combo"
    combo.mkdir(parents=True, exist_ok=True)
    box, half, amp = 200.0, 100.0, 500.0

    # Radial-infall field (all arrows toward the centre).
    n = 64
    c = np.linspace(-half, half, n)
    Z, Y, X = np.meshgrid(c, c, c, indexing="ij")
    cube = np.stack([-amp * X / half, -amp * Y / half, -amp * Z / half],
                    axis=0).astype(np.float32)
    hdu = fits.PrimaryHDU(cube)
    hdu.header["BUNIT"] = "km/s"
    hdu.header["VFRAME"] = "supergalactic"
    fits_path = combo / "velocity.fits"
    hdu.writeto(fits_path, overwrite=True)

    # Galaxies in the same box, carrying the same flow velocity (+noise).
    pos = _clustered_points(5, 3500, 6000, -95.0, 95.0, 11.0)
    vel = -amp * pos / half + RNG.normal(0, 30, size=pos.shape)
    mass = 10.0 ** RNG.normal(11.0, 0.4, len(pos))
    csv_path = combo / "galaxies.csv"
    with csv_path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["x", "y", "z", "vx", "vy", "vz", "mass"])
        for i in range(len(pos)):
            w.writerow([f"{pos[i, 0]:.4f}", f"{pos[i, 1]:.4f}", f"{pos[i, 2]:.4f}",
                        f"{vel[i, 0]:.3f}", f"{vel[i, 1]:.3f}", f"{vel[i, 2]:.3f}",
                        f"{mass[i]:.3e}"])
    return fits_path, csv_path


# ── 7. HI PPV cube (cube viewer / kinematic model / movie / linked views) ─────
def make_hi_cube(out: Path) -> Path:
    """A rotating-disk HI PPV cube with a spectral (velocity) WCS + rest freq.

    Exercises: cube 3-D volume/slice viewer, Kinematic Model Overlay (tilted
    ring) + 2-D channel contours, Export Movie (channel scan), Link Views,
    region analysis / spectral profiler.
    """
    nx = ny = 64
    nchan = 64
    pa = np.deg2rad(30.0)          # position angle
    incl = np.deg2rad(60.0)        # inclination
    vrot, vsys, sig_v = 150.0, 0.0, 18.0   # km/s
    rmax = 26.0                    # pixels
    dv = 10.0                      # km/s per channel
    v0 = -(nchan // 2) * dv        # first channel velocity (km/s)

    yy, xx = np.mgrid[0:ny, 0:nx].astype(np.float64)
    x0, y0 = nx / 2.0, ny / 2.0
    dx, dy = xx - x0, yy - y0
    # Deprojection copied verbatim from backend/app/kinematic_model.py so the
    # Kinematic Model Overlay lines up when the reviewer enters these exact
    # params (x0=32, y0=32, PA=30, incl=60, Vsys=0, Vrot=150, rmax=26, sigma=18).
    cos_i = max(np.cos(incl), 1e-3)
    x_d = -dx * np.sin(pa) + dy * np.cos(pa)
    y_d = (-dx * np.cos(pa) - dy * np.sin(pa)) / cos_i
    r = np.hypot(x_d, y_d)
    cos_theta = np.divide(x_d, r, out=np.zeros_like(x_d), where=r > 0)
    vlos = vsys + vrot * np.sin(incl) * cos_theta       # (ny, nx)
    disk = np.exp(-r / (0.5 * rmax)) * (r <= rmax)      # add a radial taper for looks

    cube = np.zeros((nchan, ny, nx), dtype=np.float32)
    for k in range(nchan):
        vch = v0 + k * dv
        cube[k] = disk * np.exp(-((vch - vlos) ** 2) / (2.0 * sig_v ** 2))
    cube += RNG.normal(0, 0.01, cube.shape).astype(np.float32)

    hdu = fits.PrimaryHDU(cube)
    h = hdu.header
    h["BUNIT"] = "Jy/beam"
    h["BMAJ"], h["BMIN"], h["BPA"] = 3 / 3600.0, 3 / 3600.0, 0.0
    h["RESTFRQ"] = HI_RESTFRQ
    # Celestial WCS (RA/Dec, ~2.5" pixels).
    h["CTYPE1"], h["CRVAL1"], h["CRPIX1"], h["CDELT1"], h["CUNIT1"] = \
        "RA---SIN", 202.4842, x0 + 1, -0.0007, "deg"
    h["CTYPE2"], h["CRVAL2"], h["CRPIX2"], h["CDELT2"], h["CUNIT2"] = \
        "DEC--SIN", 47.2306, y0 + 1, 0.0007, "deg"
    # Spectral axis in radio-convention velocity (m/s).
    h["CTYPE3"], h["CRVAL3"], h["CRPIX3"], h["CDELT3"], h["CUNIT3"] = \
        "VRAD", v0 * 1000.0, 1.0, dv * 1000.0, "m/s"
    path = out / "hi_cube.fits"
    hdu.writeto(path, overwrite=True)
    return path


# ── 8. 2-D image FITS (image viewer / region IO / profile / contours) ─────────
def make_image_2d(out: Path) -> Path:
    """A 2-D image with a celestial WCS, a beam, and a few Gaussian sources.

    Exercises: 2-D image viewer, region drawing + stats + IO (CRTF/DS9),
    profile/probe, contours, colour-scale (Linear/Log/Sqrt/…).
    """
    nx = ny = 256
    yy, xx = np.mgrid[0:ny, 0:nx]
    img = RNG.normal(0, 0.02, (ny, nx)).astype(np.float32)
    for _ in range(8):
        cx, cy = RNG.uniform(30, nx - 30), RNG.uniform(30, ny - 30)
        amp = RNG.uniform(0.5, 5.0)
        s = RNG.uniform(3.0, 10.0)
        img += (amp * np.exp(-((xx - cx) ** 2 + (yy - cy) ** 2)
                             / (2.0 * s ** 2))).astype(np.float32)

    hdu = fits.PrimaryHDU(img)
    h = hdu.header
    h["BUNIT"] = "Jy/beam"
    h["BMAJ"], h["BMIN"], h["BPA"] = 5 / 3600.0, 4 / 3600.0, 20.0
    h["CTYPE1"], h["CRVAL1"], h["CRPIX1"], h["CDELT1"], h["CUNIT1"] = \
        "RA---SIN", 83.6331, nx / 2.0, -0.0004, "deg"
    h["CTYPE2"], h["CRVAL2"], h["CRPIX2"], h["CDELT2"], h["CUNIT2"] = \
        "DEC--SIN", 22.0145, ny / 2.0, 0.0004, "deg"
    path = out / "image_2d.fits"
    hdu.writeto(path, overwrite=True)
    return path


def main() -> int:
    out = Path(sys.argv[1]).expanduser() if len(sys.argv) > 1 \
        else Path.home() / "VisIVO_demo_data"
    out.mkdir(parents=True, exist_ok=True)

    made: list[str] = []
    made.append(str(make_catalogue_sky(out)))
    made.append(str(make_catalogue_cartesian(out)))
    made.append(str(make_speck(out)))
    made += [str(p) for p in make_ipac_snapshot(out)]
    made.append(str(make_velocity_field(
        out, "velocity_field.fits", 200.0,
        sinks=[np.array([-45.0, -30.0, 20.0])],      # attractor  → overdensity
        sources=[np.array([50.0, 40.0, -25.0])])))   # repeller   → void
    fpath, cpath = make_combo(out)
    made += [str(fpath), str(cpath)]
    made.append(str(make_hi_cube(out)))
    made.append(str(make_image_2d(out)))

    print(f"Wrote {len(made)} demo files under {out}:")
    for m in made:
        print("  ", m)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
