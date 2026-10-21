#!/usr/bin/env python
"""Does the comparator's fit recover a disc whose kinematics are known?

The tilted-ring tool seeds its parameters from a lasso selection with a quick
fit (``app.compute.kinematic_fit`` — what ``/v1/cube/kinematic_fit_bin`` runs):
geometry from the moment-0 light, a flat disc and a per-ring disc solved on the
velocity field. On a real galaxy there is no truth to hold it against, so this
script builds two discs with the application's own model
(``app.kinematic_model``), a flat one and a warped one, adds noise, and fits
them the way the tool does. It reports, per quantity, what was put in and what
came out, at two signal-to-noise levels.

Two things the numbers have to show honestly:

* **The V sin i degeneracy.** In a line-of-sight velocity field the rotation
  amplitude and sin i appear only as a product, so the fit takes the
  inclination from the moment-0 isophotes and solves for V_rot sin i; V_rot
  itself is only as good as that inclination. Both are reported.
* **What a ring fit adds on a warp.** The flat disc is the wrong model for the
  warped input; the question is whether the per-ring solution follows the
  input's PA and inclination run with radius, and by how much the residual
  drops — on the same pixels the viewer's residual map is made from.

Run:  backend/.venv/bin/python validation/kinematic_fit_synthetic.py
→ out/kinematic_fit_synthetic.{md,csv}
"""
from __future__ import annotations

import csv
import math
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "backend"))
from app.compute.kinematic_fit import deproject, kinematic_fit  # noqa: E402
from app.kinematic_model import tilted_ring_model, tilted_ring_model_profile  # noqa: E402

OUT = HERE / "out"
NX = NY = 96
NZ = 64
VEL = np.linspace(800.0, 1300.0, NZ)                 # km/s, ascending; 7.9 km/s channels
DV = float(abs(VEL[1] - VEL[0]))
LINE_SIGMA = 12.0                                    # km/s, the model's line width
AMPLITUDE = 0.05                                     # peak of the noiseless cube, at the centre
NOISE = {"S/N 25": AMPLITUDE / 25.0, "S/N 8": AMPLITUDE / 8.0}
# The application's model is a kinematic footprint (every pixel a unit line);
# a disc whose light is uniform has no isophotes to read an inclination from,
# so the synthetic discs get an exponential surface brightness, scale length
# = the disc radius, measured in the LOCAL ring geometry so that on the warped
# disc the isophotes follow the warp the way a real galaxy's would.
SCALE_LENGTH = 30.0
# The model solves the warped deprojection by fixed-point iteration; the light
# profile must use the same count, or it follows a slightly different geometry
# from the one generating the velocities.
DEPROJECTION_ITERATIONS = 2

FLAT = dict(x0=47.5, y0=49.0, pa=40.0, incl=60.0, vsys=1050.0, vrot=150.0, rmax=30.0)
# The warped disc: the same centre and systemic velocity; PA and inclination
# run with radius the way an outer warp does, and the rotation curve rises
# then flattens. Values at the ring radii; the model interpolates between them.
WARP_R = np.array([6.0, 12.0, 18.0, 24.0, 30.0])
WARP_PA = np.array([40.0, 40.0, 45.0, 55.0, 70.0])
WARP_INCL = np.array([60.0, 60.0, 58.0, 55.0, 50.0])
WARP_VROT = np.array([90.0, 140.0, 150.0, 150.0, 150.0])


def _wrap(delta: float) -> float:
    """A PA difference on the circle, in (-180, 180]."""
    return (delta + 180.0) % 360.0 - 180.0


def _truth_at(r: float, values: np.ndarray) -> float:
    return float(np.interp(r, WARP_R, values))


def local_radius(ring_r: np.ndarray, ring_pa: np.ndarray, ring_incl: np.ndarray,
                 iterations: int = DEPROJECTION_ITERATIONS) -> np.ndarray:
    """Deprojected radius of every pixel in the ring geometry that applies at that
    radius — the fixed-point iteration ``tilted_ring_model_profile`` performs."""
    yy, xx = np.mgrid[0:NY, 0:NX].astype(np.float64)
    dxc, dyc = xx - FLAT["x0"], yy - FLAT["y0"]
    _, r = deproject(dxc, dyc, float(ring_pa[0]), float(ring_incl[0]))
    for _ in range(iterations):
        pa_map = np.interp(r, ring_r, ring_pa)
        incl_map = np.interp(r, ring_r, ring_incl)
        par, ci = np.radians(pa_map), np.maximum(np.cos(np.radians(incl_map)), 1e-3)
        xd = -dxc * np.sin(par) + dyc * np.cos(par)
        yd = (-dxc * np.cos(par) - dyc * np.sin(par)) / ci
        r = np.hypot(xd, yd)
    return r


def _noisy(model: np.ndarray, brightness: np.ndarray, noise: float, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    cube = AMPLITUDE * brightness[None, :, :] * model
    return (cube + rng.normal(0.0, noise, model.shape)).astype(np.float32)


def fit_one(model: np.ndarray, brightness: np.ndarray, noise: float, seed: int) -> dict:
    data = _noisy(model, brightness, noise, seed)
    # The selection the fit is given: the object's footprint, as a lasso at the
    # model's own threshold would have it. The noise does not enter the mask.
    mask = model > 0.05
    dvk = np.full(NZ, DV)
    return kinematic_fit(data, mask, VEL, dvk, beam_px=None, sigma=noise, min_ring_pix=60)


def main() -> None:
    OUT.mkdir(exist_ok=True)
    flat_model = tilted_ring_model(NX, NY, VEL, FLAT["x0"], FLAT["y0"], FLAT["pa"], FLAT["incl"],
                                   FLAT["vsys"], FLAT["vrot"], FLAT["rmax"], sigma=LINE_SIGMA)
    warp_model = tilted_ring_model_profile(NX, NY, VEL, FLAT["x0"], FLAT["y0"], WARP_R, WARP_PA,
                                           WARP_INCL, WARP_VROT, vsys=FLAT["vsys"],
                                           sigma=LINE_SIGMA, iterations=DEPROJECTION_ITERATIONS)
    flat_light = np.exp(-local_radius(np.array([FLAT["rmax"]]), np.array([FLAT["pa"]]),
                                      np.array([FLAT["incl"]])) / SCALE_LENGTH)
    warp_light = np.exp(-local_radius(WARP_R, WARP_PA, WARP_INCL) / SCALE_LENGTH)
    rows: list[dict] = []
    md = ["# The comparator's fit on discs with known kinematics",
          "",
          f"Two discs built with the application's tilted-ring model on a {NX}×{NY}×{NZ} "
          f"grid ({DV:.1f} km/s channels, line σ {LINE_SIGMA:g} km/s), Gaussian noise at two "
          f"peak signal-to-noise levels, fitted by `app.compute.kinematic_fit` — what the "
          f"viewer's *Fit from the lasso selection* runs — on the footprint of the noiseless "
          f"model. The discs carry an exponential surface brightness (scale length "
          f"{SCALE_LENGTH:g} px, in the local ring geometry), so their isophotes mean something. "
          f"Inclination comes from the moment-0 isophotes; the velocity field fixes "
          f"V_rot sin i, so V_rot is only as good as that inclination.", ""]

    # ── flat disc ──
    md += ["## Flat disc", "",
           f"Input: PA {FLAT['pa']:g}°, i {FLAT['incl']:g}°, V_sys {FLAT['vsys']:g} km/s, "
           f"V_rot {FLAT['vrot']:g} km/s (V_rot sin i = "
           f"{FLAT['vrot'] * math.sin(math.radians(FLAT['incl'])):.1f}), centre "
           f"({FLAT['x0']:g}, {FLAT['y0']:g}).", "",
           "| noise | centre Δx, Δy (px) | PA (°) | i (°) | V_sys (km/s) | V_rot sin i (km/s) | "
           "V_rot (km/s) | median residual flat / rings (km/s) | ring PA spread (°) |",
           "|---|---|---|---|---|---|---|---|---|"]
    vsini_true = FLAT["vrot"] * math.sin(math.radians(FLAT["incl"]))
    for label, noise in NOISE.items():
        f = fit_one(flat_model, flat_light, noise, seed=1)
        g, fl = f["geometry"], f["flat"]
        pas = [r["pa"] for r in f["rings"]]
        spread = (max(pas) - min(pas)) if pas else float("nan")
        rw = f["resid_warp"]["median"] if f["resid_warp"] else float("nan")
        md.append(f"| {label} | {g['x0'] - FLAT['x0']:+.1f}, {g['y0'] - FLAT['y0']:+.1f} | "
                  f"{fl['pa']:.1f} ({_wrap(fl['pa'] - FLAT['pa']):+.1f}) | "
                  f"{g['incl']:.1f} ({g['incl'] - FLAT['incl']:+.1f}) | "
                  f"{fl['vsys']:.1f} ({fl['vsys'] - FLAT['vsys']:+.1f}) | "
                  f"{fl['vrot_sini']:.1f} ({fl['vrot_sini'] - vsini_true:+.1f}) | "
                  f"{fl['vrot']:.1f} ({fl['vrot'] - FLAT['vrot']:+.1f}) | "
                  f"{f['resid_flat']['median']:.1f} / {rw:.1f} | {spread:.1f} |")
        rows.append(dict(disc="flat", noise=label, dx=g["x0"] - FLAT["x0"], dy=g["y0"] - FLAT["y0"],
                         pa=fl["pa"], dpa=_wrap(fl["pa"] - FLAT["pa"]), incl=g["incl"],
                         dincl=g["incl"] - FLAT["incl"], vsys=fl["vsys"],
                         dvsys=fl["vsys"] - FLAT["vsys"], vrot_sini=fl["vrot_sini"],
                         dvrot_sini=fl["vrot_sini"] - vsini_true, vrot=fl["vrot"],
                         resid_flat=f["resid_flat"]["median"], resid_rings=rw,
                         ring_pa_spread=spread))
    md.append("")

    # ── warped disc ──
    md += ["## Warped disc", "",
           "Input, at the ring radii (interpolated between them):", "",
           "| r (px) | PA (°) | i (°) | V_rot (km/s) | V_rot sin i (km/s) |", "|---|---|---|---|---|"]
    for r, pa, inc, vr in zip(WARP_R, WARP_PA, WARP_INCL, WARP_VROT):
        md.append(f"| {r:g} | {pa:g} | {inc:g} | {vr:g} | {vr * math.sin(math.radians(inc)):.1f} |")
    md.append("")
    for label, noise in NOISE.items():
        f = fit_one(warp_model, warp_light, noise, seed=2)
        fl = f["flat"]
        rw = f["resid_warp"]["median"] if f["resid_warp"] else float("nan")
        md += [f"### {label}", "",
               f"Flat disc through it: PA {fl['pa']:.1f}°, V_rot sin i {fl['vrot_sini']:.1f} km/s, "
               f"median residual {f['resid_flat']['median']:.1f} km/s; the ring solution brings "
               f"the residual to {rw:.1f} km/s.", "",
               "| ring r (px) | pixels | PA fit / true (°) | i fit / true (°) | "
               "V_rot sin i fit / true (km/s) |", "|---|---|---|---|---|"]
        for ring in f["rings"]:
            r = ring["r"]
            pa_t, in_t = _truth_at(r, WARP_PA), _truth_at(r, WARP_INCL)
            vs_t = _truth_at(r, WARP_VROT) * math.sin(math.radians(in_t))
            vs_f = ring["vrot"] * math.sin(math.radians(ring["incl"]))
            md.append(f"| {r:.1f} | {ring['n_pix']} | {ring['pa']:.1f} / {pa_t:.1f} "
                      f"({_wrap(ring['pa'] - pa_t):+.1f}) | {ring['incl']:.1f} / {in_t:.1f} "
                      f"({ring['incl'] - in_t:+.1f}) | {vs_f:.1f} / {vs_t:.1f} ({vs_f - vs_t:+.1f}) |")
            rows.append(dict(disc="warped", noise=label, r=r, n_pix=ring["n_pix"], pa=ring["pa"],
                             pa_true=pa_t, dpa=_wrap(ring["pa"] - pa_t), incl=ring["incl"],
                             incl_true=in_t, dincl=ring["incl"] - in_t, vrot_sini=vs_f,
                             vrot_sini_true=vs_t, dvrot_sini=vs_f - vs_t,
                             resid_flat=f["resid_flat"]["median"], resid_rings=rw))
        md.append("")

    (OUT / "kinematic_fit_synthetic.md").write_text("\n".join(md) + "\n")
    keys: list[str] = []
    for row in rows:
        for k in row:
            if k not in keys:
                keys.append(k)
    with open(OUT / "kinematic_fit_synthetic.csv", "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=keys)
        w.writeheader()
        w.writerows(rows)
    print("\n".join(md))
    print("written", OUT / "kinematic_fit_synthetic.md")


if __name__ == "__main__":
    main()
