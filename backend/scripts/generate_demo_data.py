#!/usr/bin/env python3
"""Generate synthetic demo datasets for testing every VisIVO Visual Analytics viewer feature.

Each dataset is small, self-contained, and targets a specific feature so a
reviewer can exercise the app end-to-end without hunting for real data. Run:

    backend/.venv/bin/python backend/scripts/generate_demo_data.py [OUTPUT_DIR]

Default OUTPUT_DIR is ~/VisIVO_demo_data (the desktop file browser starts at the
home directory, so it's immediately reachable). See docs/technical/demo-datasets.md for the
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

# Speed of light (m/s) — the Stokes cubes need it for λ² = (c/ν)².
C_LIGHT = 2.997_924_58e8


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


# ── 9. Full-Stokes cubes, one per plane (cube polarimetry / RM synthesis) ─────
def make_stokes_cubes(out: Path) -> list[Path]:
    """Four cubes, one per Stokes parameter, in the MeerKAT MGCLS file layout.

    Exercises: cube viewer → Load Stokes Q/U/V Companions, Polarised Intensity
    & Angle, Rotation Measure Synthesis; the companion-naming rules; and the
    backend's per-file Stokes path.

    Written deliberately in the shape the real MGCLS release uses
    (doi:10.48479/f2a2-qw16), including the three things about it that make a
    naive reader get the wrong answer. Each is reproduced on purpose, because a
    demo file that is tidier than the archive tests nothing:

      1. A DEGENERATE Stokes axis: each file carries one plane, and which plane
         it is lives in CRVAL4. This is the layout companions exist for.
      2. The Obit MFImage plane layout: the spectral axis holds NTERM fitted
         planes (total flux, spectral index) BEFORE NSPEC sub-bands, and the
         sub-band frequencies are in per-plane FREQnnnn keywords, not on the
         WCS axis — whose CRVAL3/CDELT3 here describe a different grid
         entirely, exactly as in the real files. Reading the WCS axis instead
         gives the wrong lambda^2 and feeds two fitted planes into the
         transform.
      3. A V cube whose CRVAL4 says 1, i.e. "I". The real MGCLS V cubes carry
         this, and the client is expected to trust the role it asked for.

    The rotation measure varies across the field (a left-to-right gradient plus
    two blobs) so the recovered Faraday-depth map has structure to check rather
    than one flat value. See docs/user/reference/polarimetry.md.
    """
    nx = ny = 256
    n_term, n_spec = 2, 14
    # The real G000.0+2.5 sub-band centres.
    freqs = 9.080371093750e8 + np.arange(n_spec) * 5.755108e7
    lam2 = (C_LIGHT / freqs) ** 2

    yy, xx = np.mgrid[0:ny, 0:nx].astype(np.float64)
    rm = 30.0 * (xx / nx - 0.5) * 2.0                     # -30 … +30 rad/m²
    pol = 0.05 * np.ones((ny, nx))
    for cy, cx, rm_amp, p_amp, sig in [(75, 90, 45.0, 1.0, 11.0),
                                       (170, 165, -55.0, 0.7, 15.0),
                                       (210, 60, 0.0, 0.4, 7.0)]:
        blob = np.exp(-((yy - cy) ** 2 + (xx - cx) ** 2) / (2.0 * sig ** 2))
        rm += rm_amp * np.exp(-((yy - cy) ** 2 + (xx - cx) ** 2)
                              / (2.0 * (2.5 * sig) ** 2))
        pol += p_amp * blob
    total_i = 3.0 * pol + 0.1

    def planes(role: str) -> np.ndarray:
        arr = np.zeros((1, n_term + n_spec, ny, nx), dtype=np.float32)
        arr[0, 0] = total_i          # fitted plane: total flux
        arr[0, 1] = -0.7             # fitted plane: spectral index
        for k in range(n_spec):
            angle = 2.0 * rm * lam2[k]
            if role == "I":
                plane = total_i
            elif role == "Q":
                plane = pol * np.cos(angle)
            elif role == "U":
                plane = pol * np.sin(angle)
            else:
                plane = 0.02 * total_i
            arr[0, n_term + k] = plane + RNG.normal(0, 0.01, (ny, nx))
        return arr

    def write(role: str, crval4: float, wcs_freq0: float) -> Path:
        hdu = fits.PrimaryHDU(planes(role))
        h = hdu.header
        h["BUNIT"] = "JY/BEAM"
        h["OBJECT"] = "DEMO_FIELD"
        h["TELESCOP"] = "MeerKAT"
        h["CTYPE1"], h["CRVAL1"], h["CRPIX1"], h["CDELT1"] = \
            "GLON-SIN", 0.0, nx / 2.0, -4.166667e-4
        h["CTYPE2"], h["CRVAL2"], h["CRPIX2"], h["CDELT2"] = \
            "GLAT-SIN", 2.5, ny / 2.0, 4.166667e-4
        # SPECLNMF, no CUNIT, and a grid that is NOT the sub-band grid: the
        # header as MFImage really writes it.
        h["CTYPE3"], h["CRVAL3"], h["CRPIX3"], h["CDELT3"] = \
            "SPECLNMF", wcs_freq0, 1.0, 5.755108e7
        h["CTYPE4"], h["CRVAL4"], h["CRPIX4"], h["CDELT4"] = \
            "STOKES", crval4, 1.0, 1.0
        h["NTERM"], h["NSPEC"] = n_term, n_spec
        for i, f in enumerate(freqs, start=1):
            h[f"FREQ{i:04d}"] = float(f)
        path = out / f"stokes_DEMO_{role}.fits"
        hdu.writeto(path, overwrite=True)
        return path

    # The I cube's WCS spectral axis describes a different grid from Q/U's,
    # while all four carry the same FREQnnnn sub-band keywords — which is
    # exactly what the real release does. So this set exercises the hazard that
    # actually bites (WCS axis vs keywords), not a difference between the
    # files' true frequencies, which there is none of.
    return [
        write("I", 1.0, 1.342562580617e9),
        write("Q", 2.0, 9.080371093750e8),
        write("U", 3.0, 9.080371093750e8),
        write("V", 1.0, 9.080371093750e8),   # CRVAL4 = "I": the real V header
    ]


def write_pluto(out: Path, fields: dict[str, np.ndarray], *, mode: str = "single_file",
                precision: str = "dbl", endian: str = "little",
                edges: tuple[np.ndarray, np.ndarray, np.ndarray] | None = None,
                geometry: str = "CARTESIAN", index: int = 0, time: float = 0.0,
                step: int = 0, omit: tuple[str, ...] = (), append: bool = False) -> Path:
    """Write one PLUTO binary output into ``out`` as PLUTO 4 does: a line in
    ``dbl.out``/``flt.out``, ``grid.out`` and the data (``data.NNNN.ext`` or
    one ``<var>.NNNN.ext`` per variable). ``fields`` are (nx3, nx2, nx1)
    arrays, x1 fastest; ``edges`` the cell edges per direction (uniform on
    [-1, 1] by default); ``omit`` names variables whose file is not written
    (a run copied one variable at a time). Returns the first data file."""
    out.mkdir(parents=True, exist_ok=True)
    names = list(fields)
    shape = fields[names[0]].shape
    if edges is None:
        edges = tuple(np.linspace(-1.0, 1.0, n + 1) for n in shape[::-1])
    dims = sum(1 for n in shape if n > 1)
    lines = ["# " + "*" * 54, "# PLUTO 4.4 Grid File", "# ",
             f"# DIMENSIONS: {dims}", f"# GEOMETRY:   {geometry}"]
    for d, e in enumerate(edges[:dims], start=1):
        lines.append(f"# X{d}: [{e[0]: f}, {e[-1]: f}], {e.size - 1} point(s), 2 ghosts")
    lines.append("# " + "*" * 54)
    for e in edges:
        lines.append(f"{e.size - 1} ")
        lines += [f" {i}   {a:.12e}    {b:.12e}" for i, (a, b) in enumerate(zip(e[:-1], e[1:]),
                                                                         start=1)]
    (out / "grid.out").write_text("\n".join(lines) + "\n")
    with open(out / f"{precision}.out", "a" if append else "w") as fh:
        fh.write(f"{index} {time:.6e} 1.000000e-03 {step} {mode} {endian} "
                 + " ".join(names) + " \n")
    code = ("<" if endian == "little" else ">") + ("f8" if precision == "dbl" else "f4")
    if mode == "single_file":
        path = out / f"data.{index:04d}.{precision}"
        with open(path, "wb") as fh:
            for name in names:
                np.ascontiguousarray(fields[name], dtype=code).tofile(fh)
        return path
    written = []
    for name in names:
        if name in omit:
            continue
        path = out / f"{name}.{index:04d}.{precision}"
        np.ascontiguousarray(fields[name], dtype=code).tofile(path)
        written.append(path)
    return written[0]


def make_pluto(out: Path) -> list[Path]:
    """PLUTO binary outputs (issue #315): a 3-D blast wave, two outputs of
    density and pressure in single_file mode (the .out opens the newer), and
    a 2-D run on a grid stretched along x1 (opened on cell indices)."""
    nz, ny, nx = 40, 48, 64
    z, y, x = np.meshgrid(np.linspace(-1, 1, nz), np.linspace(-1, 1, ny),
                          np.linspace(-1, 1, nx), indexing="ij")
    made = []
    for index, radius in enumerate((0.3, 0.6)):
        r = np.sqrt(x ** 2 + y ** 2 + z ** 2)
        shell = np.exp(-((r - radius) / 0.08) ** 2)
        rho = 1e-3 + 1e-1 * shell + 1e-4 * RNG.random((nz, ny, nx))
        prs = 1e-2 * (1.0 + 10.0 * shell)
        made.append(write_pluto(out / "pluto_blast", {"rho": rho, "prs": prs},
                                index=index, time=0.1 * index, step=100 * index,
                                append=index > 0))
    ny2, nx2 = 96, 128
    x1 = np.concatenate([[0.0], np.cumsum(np.geomspace(0.005, 0.05, nx2))])
    x2 = np.linspace(-1.0, 1.0, ny2 + 1)
    yy, xx = np.meshgrid(0.5 * (x2[:-1] + x2[1:]), 0.5 * (x1[:-1] + x1[1:]), indexing="ij")
    jet = np.exp(-(yy / 0.1) ** 2) * np.exp(-xx / 2.0)
    made.append(write_pluto(out / "pluto_jet2d",
                            {"rho": (0.1 + jet)[None], "vx1": jet[None]},
                            mode="multiple_files", precision="flt", endian="big",
                            edges=(x1, x2, np.array([0.0, 1.0]))))
    return made


# ── Gadget snapshots (issue #31) ──────────────────────────────────────────────

#: Block order a writer follows: Gadget-2's, then the extra blocks of a
#: hydrodynamical run with black holes.
GADGET_BLOCK_ORDER = ("POS", "VEL", "ID", "MASS", "U", "RHO", "NE", "NH", "HSML", "SFR",
                      "AGE", "Z", "POT", "ACCE", "ENDT", "TSTP", "BHMA", "BHMD")
#: The blocks format 1 has (no names: Gadget-2's order only).
GADGET_FORMAT1_BLOCKS = GADGET_BLOCK_ORDER[:16]
#: cgs constants and the hydrogen fraction the reader uses (app/gadget_snapshot.py).
PROTON_MASS_G = 1.67262192e-24
BOLTZMANN_ERG_K = 1.380649e-16
HYDROGEN_MASS_FRACTION = 0.76
GAMMA = 5.0 / 3.0


def _gadget_header(npart, mass, time, redshift, npart_total, num_files, box,
                   omega0, omega_l, hubble, endian, *, cooling=1, sfr=1, double=False,
                   metals=0, age=0) -> bytes:
    e = endian
    low = [int(n) & 0xFFFFFFFF for n in npart_total]
    high = [int(n) >> 32 for n in npart_total]
    raw = (np.array(npart, dtype=e + "i4").tobytes() + np.array(mass, dtype=e + "f8").tobytes()
           + np.array([time, redshift], dtype=e + "f8").tobytes()
           + np.array([sfr, 1], dtype=e + "i4").tobytes()
           + np.array(low, dtype=e + "u4").tobytes()
           + np.array([cooling, num_files], dtype=e + "i4").tobytes()
           + np.array([box, omega0, omega_l, hubble], dtype=e + "f8").tobytes()
           + np.array([age, metals], dtype=e + "i4").tobytes()
           + np.array(high, dtype=e + "u4").tobytes()
           + np.array([0, int(double)], dtype=e + "i4").tobytes())
    return raw + b"\0" * (256 - len(raw))


def _gadget_record(data: bytes, e: str) -> bytes:
    m = np.array([len(data)], dtype=e + "i4").tobytes()
    return m + data + m


def _gadget_block(name: str, data: bytes, e: str, fmt: int = 2) -> bytes:
    """A block as Gadget writes it: in format 2 a name record first."""
    label = name.ljust(4).encode("ascii")[:4] + np.array([len(data) + 8], dtype=e + "i4").tobytes()
    return (_gadget_record(label, e) if fmt == 2 else b"") + _gadget_record(data, e)


def write_gadget(path: Path, particles: dict, *, fmt: int = 2, endian: str = "<",
                 num_files: int = 1, split: list | None = None, mass_table=None,
                 time: float = 0.5, redshift: float | None = None, box: float = 1000.0,
                 omega0: float = 0.3, omega_l: float = 0.7, hubble: float = 0.7,
                 double: bool = False, long_ids: bool = False, cooling: int = 1,
                 sfr: int = 1, header_npart_total=None) -> list[Path]:
    """Write a Gadget snapshot as Gadget writes one. ``particles``: type ->
    {block name -> array of (n,) or (n, k)}, every array of a type n rows.
    ``num_files`` > 1 writes ``<path>.0`` ... with ``split[f][t]`` rows of
    type t in file f (default: as even as possible). Format 1 writes only
    Gadget-2's blocks, in its order. A block is not written into a file that
    has none of its particles; MASS leaves out the types of the mass table.
    Returns the files."""
    e = endian
    ftype = e + ("f8" if double else "f4")
    itype = e + ("u8" if long_ids else "u4")
    counts = [len(particles[t]["POS"]) if t in particles else 0 for t in range(6)]
    mass_table = list(mass_table or [0.0] * 6)
    if split is None:
        split = [[(counts[t] * (f + 1)) // num_files - (counts[t] * f) // num_files
                  for t in range(6)] for f in range(num_files)]
    assert [sum(s[t] for s in split) for t in range(6)] == counts
    if redshift is None:
        redshift = 1.0 / time - 1.0
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    names = [n for n in GADGET_BLOCK_ORDER
             if any(n in particles.get(t, {}) for t in range(6))
             and (fmt == 2 or n in GADGET_FORMAT1_BLOCKS)]
    if fmt == 2:
        # Any other block, after Gadget-2's, in the order first given.
        for t in range(6):
            names += [n for n in particles.get(t, {}) if n not in names]
    written = []
    start = [0] * 6
    for f in range(num_files):
        out = path if num_files == 1 else path.with_name(f"{path.name}.{f}")
        npart = split[f]
        chunks = [_gadget_block("HEAD", _gadget_header(
            npart, mass_table, time, redshift,
            header_npart_total if header_npart_total is not None else counts,
            num_files, box, omega0, omega_l, hubble, e, cooling=cooling, sfr=sfr,
            double=double), e, fmt)]
        for name in names:
            parts = []
            for t in range(6):
                block = particles.get(t, {}).get(name)
                if block is None or npart[t] == 0:
                    continue
                if name == "MASS" and mass_table[t] != 0:
                    continue
                rows = np.asarray(block)[start[t]:start[t] + npart[t]]
                parts.append(np.ascontiguousarray(rows, dtype=itype if name == "ID" else ftype))
            if parts:
                chunks.append(_gadget_block(name, b"".join(p.tobytes() for p in parts), e, fmt))
        out.write_bytes(b"".join(chunks))
        written.append(out)
        start = [start[t] + npart[t] for t in range(6)]
    return written


#: The synthetic halo's truths (docs/technical/demo-datasets.md): a hot gas
#: sphere whose temperature and density follow these profiles exactly, with
#: the electron abundance of a fully ionised gas (n_e / n_H = 1 + Y / 2X).
GADGET_HALO = {"centre": 500.0, "rs": 50.0, "rmax": 250.0, "t0": 5.0e7, "rc": 30.0,
               "rho0": 1.0e-3,
               "ne": 1.0 + (1.0 - HYDROGEN_MASS_FRACTION) / (2.0 * HYDROGEN_MASS_FRACTION)}
#: Dark matter's mass (1e10 Msun/h), from the header's mass table.
GADGET_DM_MASS = 6.0e-3


def gadget_temperature(r):
    """The synthetic gas sphere's temperature in K at radius r (kpc/h)."""
    h = GADGET_HALO
    return h["t0"] / (1.0 + r / h["rc"])


def gadget_density(r):
    """Its density in code units (1e10 Msun/h / (kpc/h)^3, comoving)."""
    h = GADGET_HALO
    return h["rho0"] / (1.0 + (r / h["rc"]) ** 2)


def gadget_halo_particles(rng, n_gas=2000, n_dm=4000, n_star=500, n_bh=2,
                          shift=0.0) -> dict:
    """An NFW-like dark-matter halo (mass from the table), a hot gas sphere
    with the analytic T(r) and rho(r) above (U from T and NE), stars, and
    black holes, centred in a 1000 kpc/h box; ``shift`` moves it along x."""
    h = GADGET_HALO
    c = np.array([h["centre"] + shift, h["centre"], h["centre"]])

    def directions(n):
        v = rng.normal(size=(n, 3))
        return v / np.linalg.norm(v, axis=1)[:, None]

    def nfw_radii(n):
        # Inverse transform of the NFW enclosed mass out to rmax.
        x = np.linspace(1e-4, h["rmax"] / h["rs"], 4096)
        m = np.log1p(x) - x / (1 + x)
        return h["rs"] * np.interp(rng.random(n) * m[-1], m, x)

    out = {}
    r_gas = h["rmax"] * rng.random(n_gas) ** (1 / 3)
    x = HYDROGEN_MASS_FRACTION
    mu = 4.0 / (1.0 + 3.0 * x + 4.0 * x * h["ne"])
    # u = k T / ((gamma - 1) mu m_p), in (km/s)^2.
    u = gadget_temperature(r_gas) * BOLTZMANN_ERG_K / ((GAMMA - 1.0) * mu * PROTON_MASS_G) / 1e10
    out[0] = {"POS": c + directions(n_gas) * r_gas[:, None],
              "VEL": rng.normal(0, 100, (n_gas, 3)),
              "ID": np.arange(1, n_gas + 1),
              "MASS": 1.0e-4 * (1 + 0.01 * np.arange(n_gas) / n_gas),
              "U": u, "RHO": gadget_density(r_gas),
              # Gadget-2 with cooling writes NE and NH (here fully ionised).
              "HSML": 5.0 + r_gas / 50.0, "NE": np.full(n_gas, h["ne"]),
              "NH": np.zeros(n_gas)}
    r_dm = nfw_radii(n_dm)
    out[1] = {"POS": c + directions(n_dm) * r_dm[:, None],
              "VEL": rng.normal(0, 300, (n_dm, 3)),
              "ID": np.arange(100001, 100001 + n_dm)}
    r_st = rng.exponential(10.0, n_star)
    out[4] = {"POS": c + directions(n_star) * r_st[:, None],
              "VEL": rng.normal(0, 200, (n_star, 3)),
              "ID": np.arange(200001, 200001 + n_star),
              "MASS": np.full(n_star, 2.0e-5)}
    out[5] = {"POS": c + np.array([[0.0, 0.0, 0.0], [3.0, -2.0, 1.0]])[:n_bh],
              "VEL": np.zeros((n_bh, 3)), "ID": np.arange(300001, 300001 + n_bh),
              "MASS": np.full(n_bh, 1.0e-2), "BHMA": np.array([0.02, 0.005])[:n_bh]}
    return out


def write_subfind(path: Path, centre, rvir: float, mvir: float, endian: str = "<", *,
                  time: float = 0.5, vmax: float | None = None) -> Path:
    """A SubFind-like group file in format 2 (issue #392): the header counts
    one group as "type" 0 and one subhalo as "type" 1, including the fields
    needed to frame and inspect both objects. ``time`` is the scale factor;
    ``vmax`` adds a VMAX block."""
    e, f4, u4 = endian, endian + "f4", endian + "u4"
    npart = [1, 1, 0, 0, 0, 0]
    head = _gadget_header(npart, [0.0] * 6, time, 1.0 / time - 1.0, npart, 1, 1000.0, 0.3,
                          0.7, 0.7, e)
    extra = b"" if vmax is None else _gadget_block(
        "VMAX", np.array([vmax], dtype=f4).tobytes(), e)
    body = (_gadget_block("HEAD", head, e)
            + _gadget_block("GPOS", np.array([centre], dtype=f4).tobytes(), e)
            + _gadget_block("MVIR", np.array([mvir], dtype=f4).tobytes(), e)
            + _gadget_block("RVIR", np.array([rvir], dtype=f4).tobytes(), e)
            + _gadget_block("M500", np.array([0.7 * mvir], dtype=f4).tobytes(), e)
            + _gadget_block("R500", np.array([0.6 * rvir], dtype=f4).tobytes(), e)
            + _gadget_block("GLEN", np.array([12000], dtype=u4).tobytes(), e)
            + _gadget_block("NSUB", np.array([1], dtype=u4).tobytes(), e)
            + _gadget_block("SPOS", np.array([centre], dtype=f4).tobytes(), e)
            + _gadget_block("SVEL", np.array([[120.0, -30.0, 8.0]], dtype=f4).tobytes(), e)
            + _gadget_block("MSUB", np.array([0.25 * mvir], dtype=f4).tobytes(), e)
            + _gadget_block("RHMS", np.array([0.1 * rvir], dtype=f4).tobytes(), e)
            + _gadget_block("GRNR", np.array([0], dtype=u4).tobytes(), e)
            + extra)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(body)
    return path


def gadget_split(parts: dict) -> list[list[int]]:
    """How the demo series spreads its particles over three files: black
    holes in the first only, no stars in the last, uneven gas and DM."""
    n = [len(parts[t]["POS"]) if t in parts else 0 for t in range(6)]
    return [[n[0] // 2, n[1] // 3, 0, 0, n[4] // 2, n[5]],
            [n[0] // 3, n[1] // 3, 0, 0, n[4] - n[4] // 2, 0],
            [n[0] - n[0] // 2 - n[0] // 3, n[1] - 2 * (n[1] // 3), 0, 0, 0, 0]]


#: Scale factors of the demo series.
GADGET_SERIES_A = (0.25, 0.4, 0.5)


def make_gadget(out: Path) -> list[Path]:
    """Gadget snapshots (issue #31): a series of three multi-file format-2
    snapshots of the halo moving along x (``gadget/snapdir_00N/snap_00N.0..2``),
    a format-1 single file, a big-endian format-2 two-file copy, and a
    SubFind-like group file beside the last snapshot."""
    mass_table = [0.0, GADGET_DM_MASS, 0.0, 0.0, 0.0, 0.0]
    made = []
    for k, a in enumerate(GADGET_SERIES_A):
        parts = gadget_halo_particles(np.random.default_rng(310 + k), shift=20.0 * k)
        made += write_gadget(out / "gadget" / f"snapdir_{k:03d}" / f"snap_{k:03d}", parts,
                             num_files=3, split=gadget_split(parts), mass_table=mass_table,
                             time=a)
    made.append(write_subfind(out / "gadget" / "groups_002" / "sub_002.0",
                              [540.0, 500.0, 500.0], 250.0, 120.0))
    parts = gadget_halo_particles(np.random.default_rng(31))
    made += write_gadget(out / "gadget_format1" / "snap_005", parts, fmt=1,
                         mass_table=mass_table)
    made += write_gadget(out / "gadget_bigendian" / "snap_005", parts, endian=">",
                         num_files=2, mass_table=mass_table)
    return made


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
    made += [str(p) for p in make_stokes_cubes(out)]
    made += [str(p) for p in make_pluto(out)]
    made += [str(p) for p in make_gadget(out)]

    print(f"Wrote {len(made)} demo files under {out}:")
    for m in made:
        print("  ", m)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
