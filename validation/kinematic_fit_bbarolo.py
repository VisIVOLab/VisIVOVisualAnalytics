#!/usr/bin/env python
"""The comparator's fit on discs built by another code: 3DBarolo's GALMOD.

``kinematic_fit_synthetic.py`` tests the quick fit on discs made with the
application's own model, which shares its deprojection with the fit: a test of
internal consistency only. Here the discs come from 3DBarolo (Di Teodoro &
Fraternali 2015), whose GALMOD builds a cube from a tilted-ring description the
way a real observation looks: an exponential gas surface density, a disc of
finite thickness, a velocity dispersion, and convolution with the synthesised
beam. None of that is in the comparator's model (a unit-amplitude kinematic
footprint, no beam), so this measures how far the comparator's numbers drift
on emission that is closer to a real galaxy, beam smearing included.

Two discs on a WALLABY-like grid (beam 30", pixels 6", 4 km/s channels):
a flat one and a warped one with a rising rotation curve. Noise at two peak
signal-to-noise levels; fitted by ``app.compute.kinematic_fit`` exactly as
``kinematic_fit_synthetic.py`` does.

Needs the 3DBarolo executable (version 1.8 was used), given by the BBAROLO
environment variable:

    BBAROLO=/path/to/BBarolo backend/.venv/bin/python validation/kinematic_fit_bbarolo.py

→ out/kinematic_fit_bbarolo.{md,csv}; the GALMOD cubes and parameter files are
written to out/bbarolo_discs/.
"""
from __future__ import annotations

import csv
import math
import os
import subprocess
import sys
from pathlib import Path

import numpy as np
from astropy.io import fits

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "backend"))
from app.compute.kinematic_fit import kinematic_fit  # noqa: E402

OUT = HERE / "out"
WORK = OUT / "bbarolo_discs"

NX = NY = 160
NZ = 128
PIX_ARCSEC = 6.0
BEAM_ARCSEC = 30.0
DV = 4.0                                   # km/s
VSYS = 1050.0
VEL = VSYS + (np.arange(NZ) - (NZ - 1) / 2.0) * DV
X0 = Y0 = (NX - 1) / 2.0                   # 0-based pixel centre
NOISE_SN = {"S/N 25": 25.0, "S/N 8": 8.0}
VDISP = 10.0                               # km/s
SCALE_ARCSEC = 150.0                       # exponential surface-density scale length

# Ring radii in arcsec (every 30", one beam) out to 360" = 60 px = 12 beams.
RADII = np.arange(15.0, 361.0, 30.0)
FLAT = dict(pa=40.0, incl=60.0, vrot=150.0)
WARP_PA = np.interp(RADII, [0, 150, 240, 360], [40, 40, 55, 70])
WARP_INCL = np.interp(RADII, [0, 150, 240, 360], [60, 60, 55, 50])
WARP_VROT = 150.0 * np.tanh(RADII / 60.0)


def _wrap(delta: float) -> float:
    return (delta + 180.0) % 360.0 - 180.0


def template(path: Path) -> None:
    """An empty cube with the grid, the beam and a velocity axis 3DBarolo reads."""
    h = fits.Header()
    h["NAXIS"] = 3
    h["NAXIS1"], h["NAXIS2"], h["NAXIS3"] = NX, NY, NZ
    h["CTYPE1"], h["CRPIX1"], h["CRVAL1"], h["CDELT1"], h["CUNIT1"] = (
        "RA---SIN", X0 + 1, 150.0, -PIX_ARCSEC / 3600.0, "deg")
    h["CTYPE2"], h["CRPIX2"], h["CRVAL2"], h["CDELT2"], h["CUNIT2"] = (
        "DEC--SIN", Y0 + 1, -29.0, PIX_ARCSEC / 3600.0, "deg")
    h["CTYPE3"], h["CRPIX3"], h["CRVAL3"], h["CDELT3"], h["CUNIT3"] = (
        "VRAD", 1, VEL[0] * 1000.0, DV * 1000.0, "m/s")
    h["BMAJ"] = h["BMIN"] = BEAM_ARCSEC / 3600.0
    h["BPA"] = 0.0
    h["BUNIT"] = "JY/BEAM"
    h["EQUINOX"] = 2000.0
    fits.PrimaryHDU(np.zeros((NZ, NY, NX), np.float32), h).writeto(path, overwrite=True)


def galmod(name: str, pa, incl, vrot, exe: str) -> np.ndarray:
    """Run GALMOD for one disc; return the model cube (z, y, x)."""
    d = WORK / name
    d.mkdir(parents=True, exist_ok=True)
    tpl = d / "template.fits"
    template(tpl)
    dens = 10.0 * np.exp(-RADII / SCALE_ARCSEC)
    rings = d / "rings.txt"
    with rings.open("w") as f:
        f.write("# RAD VROT VDISP INC PA DENS\n")
        for r, v, i, p, n in zip(RADII, np.broadcast_to(vrot, RADII.shape),
                                 np.broadcast_to(incl, RADII.shape),
                                 np.broadcast_to(pa, RADII.shape), dens):
            f.write(f"{r:.1f} {v:.3f} {VDISP:.1f} {i:.3f} {p:.3f} {n:.5f}\n")
    par = d / "galmod.par"
    par.write_text("\n".join([
        f"FITSFILE  {tpl}",
        "GALMOD    true",
        f"OUTFOLDER {d}/",
        f"NRADII    {len(RADII)}",
        "RADII     file(rings.txt,1)",
        "VROT      file(rings.txt,2)",
        "VDISP     file(rings.txt,3)",
        "INC       file(rings.txt,4)",
        "PA        file(rings.txt,5)",
        "DENS      file(rings.txt,6)",
        f"XPOS      {X0:.2f}",
        f"YPOS      {Y0:.2f}",
        f"VSYS      {VSYS}",
        "Z0        0",
        "LTYPE     1",
        "CDENS     10",
        "NV        20",
        "SM        true",
    ]) + "\n")
    log = subprocess.run([exe, "-p", str(par)], cwd=d, capture_output=True, text=True)
    (d / "bbarolo.log").write_text(log.stdout + log.stderr)
    cubes = sorted(p for p in d.rglob("*mod*.fits") if "template" not in p.name)
    if log.returncode != 0 or not cubes:
        raise RuntimeError(f"GALMOD failed for {name}; see {d / 'bbarolo.log'}")
    return np.nan_to_num(fits.getdata(cubes[0]).astype(np.float64))


def fit_one(model: np.ndarray, sn: float, seed: int) -> dict:
    peak = float(model.max())
    noise = peak / sn
    rng = np.random.default_rng(seed)
    data = (model + rng.normal(0.0, noise, model.shape)).astype(np.float32)
    # The footprint of the noiseless model, as in kinematic_fit_synthetic.py:
    # a selection the fit is given, which the noise does not shape.
    mask = model > 0.05 * peak
    dvk = np.full(NZ, DV)
    return kinematic_fit(data, mask, VEL, dvk, beam_px=BEAM_ARCSEC / PIX_ARCSEC,
                         sigma=noise, min_ring_pix=60)


def main() -> None:
    exe = os.environ.get("BBAROLO")
    if not exe:
        sys.exit("set BBAROLO to the 3DBarolo executable")
    OUT.mkdir(exist_ok=True)
    flat_model = galmod("flat", FLAT["pa"], FLAT["incl"], FLAT["vrot"], exe)
    warp_model = galmod("warped", WARP_PA, WARP_INCL, WARP_VROT, exe)
    rows: list[dict] = []
    vsini = FLAT["vrot"] * math.sin(math.radians(FLAT["incl"]))
    md = ["# The comparator's fit on discs built by 3DBarolo (GALMOD)", "",
          f"Grid {NX}×{NY}×{NZ}, {PIX_ARCSEC:g}\" pixels, {BEAM_ARCSEC:g}\" beam "
          f"({BEAM_ARCSEC / PIX_ARCSEC:g} px), {DV:g} km/s channels; exponential surface "
          f"density (scale {SCALE_ARCSEC:g}\"), dispersion {VDISP:g} km/s, beam-convolved. "
          f"Rings every {RADII[1] - RADII[0]:g}\" out to {RADII[-1]:g}\" "
          f"({RADII[-1] / PIX_ARCSEC:g} px). Fitted by `app.compute.kinematic_fit`.", "",
          "## Flat disc", "",
          f"Input: PA {FLAT['pa']:g}°, i {FLAT['incl']:g}°, V_sys {VSYS:g}, V_rot "
          f"{FLAT['vrot']:g} km/s (V_rot sin i {vsini:.1f}).", "",
          "| noise | PA (°) | i (°) | V_sys (km/s) | V_rot sin i (km/s) | median residual "
          "flat / rings (km/s) | pixels |", "|---|---|---|---|---|---|---|"]
    for label, sn in NOISE_SN.items():
        f = fit_one(flat_model, sn, seed=1)
        g, fl = f["geometry"], f["flat"]
        rw = f["resid_warp"]["median"] if f["resid_warp"] else float("nan")
        md.append(f"| {label} | {fl['pa']:.1f} ({_wrap(fl['pa'] - FLAT['pa']):+.1f}) | "
                  f"{g['incl']:.1f} ({g['incl'] - FLAT['incl']:+.1f}) | "
                  f"{fl['vsys']:.1f} ({fl['vsys'] - VSYS:+.1f}) | "
                  f"{fl['vrot_sini']:.1f} ({fl['vrot_sini'] - vsini:+.1f}) | "
                  f"{f['resid_flat']['median']:.1f} / {rw:.1f} | {f['resid_flat']['n_pix']} |")
        rows.append(dict(disc="flat", noise=label, dpa=_wrap(fl["pa"] - FLAT["pa"]),
                         dincl=g["incl"] - FLAT["incl"], dvsys=fl["vsys"] - VSYS,
                         dvrot_sini=fl["vrot_sini"] - vsini,
                         resid_flat=f["resid_flat"]["median"], resid_rings=rw,
                         n_pix=f["resid_flat"]["n_pix"]))
    md += ["", "## Warped disc with a rising rotation curve", ""]
    for label, sn in NOISE_SN.items():
        f = fit_one(warp_model, sn, seed=2)
        fl = f["flat"]
        rw = f["resid_warp"]["median"] if f["resid_warp"] else float("nan")
        md += [f"### {label}", "",
               f"Flat disc: PA {fl['pa']:.1f}°, median residual "
               f"{f['resid_flat']['median']:.1f} km/s; rings: {rw:.1f} km/s.", "",
               "| ring r (px) | pixels | PA fit / true (°) | i fit / true (°) | "
               "V_rot sin i fit / true (km/s) |", "|---|---|---|---|---|"]
        r_px = RADII / PIX_ARCSEC
        for ring in f["rings"]:
            r = ring["r"]
            pa_t = float(np.interp(r, r_px, WARP_PA))
            in_t = float(np.interp(r, r_px, WARP_INCL))
            vs_t = float(np.interp(r, r_px, WARP_VROT)) * math.sin(math.radians(in_t))
            vs_f = ring["vrot"] * math.sin(math.radians(ring["incl"]))
            md.append(f"| {r:.1f} | {ring['n_pix']} | {ring['pa']:.1f} / {pa_t:.1f} "
                      f"({_wrap(ring['pa'] - pa_t):+.1f}) | {ring['incl']:.1f} / {in_t:.1f} "
                      f"({ring['incl'] - in_t:+.1f}) | {vs_f:.1f} / {vs_t:.1f} "
                      f"({vs_f - vs_t:+.1f}) |")
            rows.append(dict(disc="warped", noise=label, r=r, n_pix=ring["n_pix"],
                             dpa=_wrap(ring["pa"] - pa_t), dincl=ring["incl"] - in_t,
                             dvrot_sini=vs_f - vs_t, beams_from_centre=r / (BEAM_ARCSEC / PIX_ARCSEC),
                             resid_flat=f["resid_flat"]["median"], resid_rings=rw))
        md.append("")
    (OUT / "kinematic_fit_bbarolo.md").write_text("\n".join(md) + "\n")
    keys: list[str] = []
    for row in rows:
        keys += [k for k in row if k not in keys]
    with (OUT / "kinematic_fit_bbarolo.csv").open("w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=keys)
        w.writeheader()
        w.writerows(rows)
    print((OUT / "kinematic_fit_bbarolo.md").read_text())


if __name__ == "__main__":
    main()
