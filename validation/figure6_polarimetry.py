#!/usr/bin/env python
"""Figure 6 of the paper: polarimetry on the MeerKAT 1.3 GHz Bulge data.

Runs the service's own polarimetry workers -- the same code the application
calls -- on the Stokes I/Q/U sub-band cubes of one field of the SARAO data
release (Cotton et al. 2025, doi:10.48479/f2a2-qw16): polarised intensity and
angle on one sub-band over the whole field, then rotation-measure synthesis
on a cutout, reduced service-side to the peak-|F| and Faraday-depth maps as the
application does. Catalogued polarised sources inside the cutout (Mosaic_Pol_Cat
of the same release) are marked with their published RM, so the map carries its
own independent check.

Run (real data):
  backend/.venv/bin/python validation/figure6_polarimetry.py \
      --dir /Users/fvitello/downloads/DemoData/meerkat_bulge --field G000.0+2.5 \
      --centre "17h36m16.510s -27d49m09.54s" --size 900 --catalogue Mosaic_Pol_Cat.csv
   (the paper's Figure 6: the 900-px cutout holding J1735447-274134,
   J1736165-274910 and J1736289-274827; the catalogue is the release's
   Mosaic_Pol_Cat.csv)
Run (synthetic demo cubes, to exercise the path):
  backend/.venv/bin/python validation/figure6_polarimetry.py \
      --dir /Users/fvitello/downloads/DemoData/polarimetry --field DEMO --size 300
"""
from __future__ import annotations

import argparse
import base64
import csv
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from astropy.coordinates import SkyCoord
from astropy.io import fits
from astropy.wcs import WCS

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "backend"))
from app.compute.polarisation import (  # noqa: E402
    stokes_plane_coverage, worker_polarisation_pi_pa, worker_rm_synthesis)
sys.path.insert(0, str(HERE))
from datasets import parse_floats  # noqa: E402


def f32(b64: str, h: int, w: int) -> np.ndarray:
    return np.frombuffer(base64.b64decode(b64), dtype="<f4").reshape(h, w)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dir", required=True); ap.add_argument("--field", required=True)
    ap.add_argument("--centre", default="", help="sky position of the cutout centre; brightest PI if empty")
    ap.add_argument("--size", type=int, default=700, help="cutout side in pixels")
    ap.add_argument("--catalogue", default="", help="Mosaic_Pol_Cat.csv of the release, to mark sources")
    ap.add_argument("--phi", type=lambda t: parse_floats(t, 3, "--phi", "-300,300,1"),
                    default=(-300.0, 300.0, 1.0), help="phi_min,phi_max,d_phi in rad/m^2")
    a = ap.parse_args()
    d = Path(a.dir)
    paths = {s: str(d / f"{a.field}_{s}.fits") for s in "IQU"}
    for p in paths.values():
        if not Path(p).exists():
            sys.exit(f"missing {p}")
    comp = {"Q": paths["Q"], "U": paths["U"]}

    # Which sub-bands carry data, by the service's own rule (stokes_plane_coverage,
    # the one the RM worker applies), judged on a central sample of Q and U read
    # through the service's own reader. PI/PA is a single-plane product, so take
    # the sub-band with data nearest the middle of the band. `plane` counts file
    # planes, 0-based, fitted planes included: what the workers take.
    hdr = fits.getheader(paths["Q"])
    nxf, nyf = int(hdr["NAXIS1"]), int(hdr["NAXIS2"])
    w, h = min(800, nxf), min(800, nyf)                    # a field can be smaller than the sample
    cov = stokes_plane_coverage(paths["I"], comp, region=((nxf - w) // 2, (nyf - h) // 2, w, h))
    freq = np.array([c["freq_hz"] for c in cov])
    offset = cov[0]["plane"]
    ok = [i for i, c in enumerate(cov) if c["has_data"]]
    if not ok:
        sys.exit("no sub-band carries data in the centre of the field")
    s = min(ok, key=lambda i: abs(i - len(cov) // 2))
    k = cov[s]["plane"]
    blank = [c["plane"] for c in cov if not c["has_data"]]
    print(f"{a.field}: {len(cov)} sub-bands {freq.min()/1e6:.0f}-{freq.max()/1e6:.0f} MHz after {offset} fitted planes; "
          f"blank planes {blank or 'none'}; PI/PA on plane {k} ({freq[s]/1e6:.0f} MHz)")

    # 1. polarised intensity and angle over the whole field, one sub-band
    r = worker_polarisation_pi_pa(paths["I"], k, None, companions=comp)
    ny, nx = r["height"], r["width"]
    pi = f32(r["pi_base64"], ny, nx)
    # A single pointing is blank (NaN or exact 0) outside its primary-beam area.
    pi = np.where(pi == 0.0, np.nan, pi)
    frac_valid = np.isfinite(pi).mean()
    print(f"PI map {nx}x{ny}, {100*frac_valid:.0f}% valid pixels")

    # 2. the cutout: around a sky position, or around the brightest PI
    hdr = fits.getheader(paths["I"]); w = WCS(hdr).celestial
    if a.centre:
        c = SkyCoord(a.centre.split()[0], a.centre.split()[1], frame="icrs")
        cx, cy = (float(v) for v in w.world_to_pixel(c))
    else:
        from scipy import ndimage
        sm = ndimage.gaussian_filter(np.nan_to_num(pi), 2.0)
        cy, cx = (float(v) for v in np.unravel_index(np.nanargmax(sm), sm.shape))
    if a.size > min(nx, ny):
        sys.exit(f"--size {a.size} exceeds the {nx}×{ny} field")
    half = a.size // 2
    x0 = int(np.clip(round(cx) - half, 0, nx - a.size)); y0 = int(np.clip(round(cy) - half, 0, ny - a.size))
    print(f"cutout {a.size}x{a.size} at x0={x0} y0={y0} (centre {cx:.0f},{cy:.0f})")

    # 3. RM synthesis on the cutout, reduced to peak maps (what the viewer asks for)
    pmin, pmax, dphi = a.phi
    if dphi <= 0 or pmax <= pmin:
        sys.exit("--phi wants phi_min < phi_max and a positive step")
    rr = worker_rm_synthesis(paths["I"], pmin, pmax, dphi, companions=comp, reduce="peak",
                             region=(x0, y0, a.size, a.size))
    if rr.get("dropped_planes"):
        mhz = ", ".join(f"{f / 1e6:.0f}" for f in rr["dropped_freq_hz"])
        print(f"RM synthesis dropped blank planes {rr['dropped_planes']} ({mhz} MHz); {rr['n_freq']} sub-bands used")
    peak = f32(rr["data_base64"], a.size, a.size); phi = f32(rr["phi_peak_base64"], a.size, a.size)
    peak = np.where(peak == 0.0, np.nan, peak)
    fpk = peak[np.isfinite(peak)]
    if fpk.size < 100:
        sys.exit("the cutout is blank: choose a centre inside the pointing's primary-beam area")
    # The noise of the peak map as the service reports it (peak_sigma): the same
    # number the viewer's "Faraday depth (peak |F| > 6σ)" product is cut with.
    sig_f = float(rr["peak_sigma"])
    print(f"RM synthesis: phi {pmin}..{pmax} step {dphi}; sigma_|F| = {sig_f*1e3:.3f} mJy/beam")

    # 4. figure: the two maps the viewer produces, as it produces them.
    # Left, Polarised Intensity & Angle's PI map over the cutout (the angle is not
    # drawn: on a 22' cutout the sources are a few beams across, so angle ticks
    # inside them would be below the resolution of the printed figure).
    pic = pi[y0:y0 + a.size, x0:x0 + a.size]
    fig, ax = plt.subplots(1, 2, figsize=(8.6, 4.3), constrained_layout=True)
    vmax = np.nanpercentile(pic, 99.7)
    ip = ax[0].imshow(pic * 1e3, origin="lower", cmap="inferno", vmin=0, vmax=vmax * 1e3,
                      interpolation="nearest")
    cb0 = fig.colorbar(ip, ax=ax[0], shrink=0.85); cb0.set_label("polarised intensity (mJy beam$^{-1}$)", fontsize=8.5)
    ax[0].set_title(f"Polarised intensity at {freq[s]/1e6:.0f} MHz", fontsize=8.5)
    good = np.isfinite(phi) & (peak > 6.0 * sig_f)
    lim = float(np.nanpercentile(np.abs(phi[good]), 98)) if good.any() else abs(pmax)
    im = ax[1].imshow(np.ma.masked_where(~good, phi), origin="lower", cmap="RdBu_r", vmin=-lim, vmax=lim,
                      interpolation="nearest")
    ax[1].set_facecolor("#eeeeee")
    cb = fig.colorbar(im, ax=ax[1], shrink=0.85); cb.set_label("Faraday depth at peak |F(φ)|  (rad m$^{-2}$)", fontsize=8.5)
    ax[1].set_title("Faraday depth where peak |F| > 6σ", fontsize=8.5)
    # catalogue cross-check: mark published RMs inside the cutout
    if a.catalogue and Path(a.catalogue).exists():
        n_marked = 0
        for row in csv.DictReader(open(a.catalogue)):
            if not row["RM"] or not row["PPol"]:
                continue
            s = row["Name"][1:]; ra, dec = s[:7], s[7:]
            c = SkyCoord(f"{ra[:2]}h{ra[2:4]}m{ra[4:6]}.{ra[6:]}s", f"{dec[:3]}d{dec[3:5]}m{dec[5:]}s", frame="icrs")
            px, py = (float(v) for v in w.world_to_pixel(c)); px -= x0; py -= y0
            if 0 <= px < a.size and 0 <= py < a.size and float(row["PPol"]) > 200:
                yy_, xx_ = int(round(py)), int(round(px))
                win = (slice(max(0, yy_ - 2), yy_ + 3), slice(max(0, xx_ - 2), xx_ + 3))
                # What the viewer's box region reports on the masked map: the
                # median of its finite pixels (5 × 5 px around the position).
                ours = np.nanmedian(np.where(good[win], phi[win], np.nan)) if good[win].any() else float("nan")
                for axx in ax:
                    axx.plot(px, py, "o", mfc="none", mec="#2ca02c", ms=9, mew=1.2)
                ax[1].annotate(f"cat {float(row['RM']):+.0f} / ours {ours:+.0f}", (px, py), xytext=(6, 6),
                               textcoords="offset points", fontsize=8, color="#1a5e1a",
                               bbox=dict(fc="white", ec="none", alpha=0.8, pad=0.6))
                n_marked += 1
                print(f"  {row['Name']}: catalogue RM {float(row['RM']):+.1f} ± {row['RM_err']}, ours {ours:+.1f} rad/m^2")
        print(f"{n_marked} catalogued polarised sources inside the cutout")
    for axx in ax:
        axx.set_xticks([]); axx.set_yticks([])
    fig.suptitle(f"MeerKAT 1.3 GHz towards the Milky Way Bulge, field {a.field}: {a.size}×{a.size} px cutout", fontsize=10)
    out = HERE / "out" / "figure6_polarimetry.png"
    fig.savefig(out, dpi=220); print("written", out)


if __name__ == "__main__":
    main()
