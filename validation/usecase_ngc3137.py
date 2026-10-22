#!/usr/bin/env python
"""The HI use case of the paper, run end to end on one real galaxy.

Produces every number Section 6.1 quotes, so the section reports measurements
rather than a narrative. The galaxy is NGC 3137 = WALLABY J100903-290239, the
cubelet released in WALLABY Pilot Data Release 1 (Westmeier et al. 2022, project
AS102, Hydra field). The release catalogue flags it ``kflag = 1``: automated
kinematic modelling was attempted on it and did not succeed, which is the point
of running an interactive comparator over it.

Two choices here are measurements, not taste:

* the selection uses smooth-before-threshold. A raw 2.5-sigma cut holds 47,994
  voxels around the clicked peak against the finder's 299,562, because a
  per-voxel threshold cannot see this disc's faint envelope. The geodesic budget
  is NOT the limit: see the paper's Section 4.2.4.
* the model is compared in VELOCITY, not in Jy/beam. The tilted-ring tool emits
  unit-amplitude Gaussians centred on each pixel's line-of-sight velocity -- a
  kinematic footprint, not a flux model -- so the meaningful residual is between
  the model's v_los and the data's moment-1 field.

``run()`` returns every intermediate array so the figure scripts draw from the
same products the numbers come from; ``main()`` prints the numbers and writes
``out/usecase_ngc3137.md``.

Run:  backend/.venv/bin/python validation/usecase_ngc3137.py
"""
from __future__ import annotations

import os
import sys
from pathlib import Path

import numpy as np
from astropy.io import fits

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "backend"))

from app.compute._common import beam_area_pixels               # noqa: E402
from app.compute.kinematic_fit import (cube_velocity_axis,     # noqa: E402
                                       kinematic_fit, robust_sigma)
from app.compute.selection import cube_kinematic_select        # noqa: E402
from app.fits_dataset import ScientificFitsDataset             # noqa: E402
from app.kinematic_model import (tilted_ring_model,            # noqa: E402
                                 tilted_ring_model_profile)

CUBE = os.environ.get("VISIVO_WALLABY_CUBE",
                      "/Users/fvitello/downloads/DemoData/WALLABY.fits")
SEED = (52, 85, 84)            # peak voxel of the catalogued source
SMOOTH = (2.0, 6.0, 6.0)       # sigma in voxels (v, y, x)
# The SHIPPED defaults (see CubeKinematicSelectRequest): the paper says "a
# default click", so the script must click with the defaults. The benchmark
# driver uses lambda_v = 0.7 for its own reasons; copying that here made three
# numbers in the paper wrong by a few per cent, which is exactly the kind of
# drift a reproducibility script exists to prevent.
REACH, LAMBDA_V = 8.0, 1.0
# A ring is fitted only where it has enough pixels to constrain two parameters.
# 150 pixels is about five synthesised beams of area on this cube; a ring with
# fewer is reported in the per-ring table but not used in the warped model, so
# the outer profile is held at the last populated ring (np.interp clamps).
MIN_RING_PIX = int(os.environ.get("VISIVO_USECASE_MIN_RING_PIX", "150"))
CAT_NPIX, CAT_FSUM_JYHZ = 299562, 673807.506087966
# Published WALLABY PDR1 catalogue values for this source (CASDA, AS102).
CAT_VSYS, CAT_W50_HALF, CAT_KIN_PA, CAT_ELL_PA, CAT_AXIS_RATIO = 1111.7, 125.2, 169.4, 153.5, 0.31
C_KMS = 299792.458


def run(cube: str = CUBE, verbose: bool = True) -> dict:
    """Every number of the use case, through the app's own code: the lasso
    (``cube_kinematic_select``), the kinematic fit of the selection
    (``app.compute.kinematic_fit`` — what /v1/cube/kinematic_fit_bin runs) and
    the tilted-ring model (``app.kinematic_model`` — what the overlay draws)."""
    log = print if verbose else (lambda *a, **k: None)
    if not Path(cube).exists():
        sys.exit(f"cube not found: {cube} (set VISIVO_WALLABY_CUBE)")
    h = fits.getheader(cube)
    data = np.squeeze(fits.getdata(cube)).astype(np.float32)
    nz, ny, nx = data.shape
    ds = ScientificFitsDataset(cube)
    axis = cube_velocity_axis(ds, nz)                          # as the fit route reads it
    vel, dvk = axis["vel"], axis["dvk"]
    dv = float(np.mean(np.abs(np.diff(vel))))
    beam = ds.beam
    bpx = beam_area_pixels(beam.major, beam.minor, ds.spatial_pixel_scale_arcsec(),
                           ds.spatial_pixel_area_arcsec2())
    R: dict = dict(header=h, data=data, vel=vel, dv=dv, beam_px=bpx)
    log(f"cube {nx}x{ny}x{nz}   dv={dv:.2f} km/s   beam={bpx:.1f} px   velocity {axis['convention']}")

    # ---- 1. one click ----------------------------------------------------
    mask_default = cube_kinematic_select(data, SEED, reach=REACH, lambda_v=LAMBDA_V)
    mask = cube_kinematic_select(data, SEED, reach=REACH, lambda_v=LAMBDA_V,
                                 smooth_zyx=SMOOTH)
    nsel = int(mask.sum())
    R.update(mask_default=mask_default, mask=mask, n_default=int(mask_default.sum()), n_sel=nsel)
    # Where the default selection comes from: the thresholded component the
    # click lands in (what a plain 2.5σ cut connected to the seed gives), and
    # the sub-threshold shell the geodesic reaches within its budget. The two
    # are the numbers the paper has to reconcile (68 909 > 47 994).
    from app.compute.selection import _robust_sigma  # noqa: PLC0415
    from skimage.morphology import flood  # noqa: PLC0415
    thr = 2.5 * _robust_sigma(data)
    cand = np.isfinite(data) & (data > thr)
    cand[SEED] = True
    comp = flood(cand, SEED, connectivity=1)
    R.update(n_component=int(comp.sum()),
             n_in_component=int((mask_default & comp).sum()),
             n_in_shell=int((mask_default & ~comp).sum()),
             n_component_beyond_budget=int((comp & ~mask_default).sum()))
    log(f"    2.5σ component at the seed {R['n_component']} voxels; the default selection keeps "
        f"{R['n_in_component']} of them, adds {R['n_in_shell']} sub-threshold voxels within the "
        f"budget, leaves {R['n_component_beyond_budget']} beyond it")
    log(f"\n[1] lasso, defaults {int(mask_default.sum())} voxels "
        f"({100*mask_default.sum()/CAT_NPIX:.0f}% of the SoFiA mask's {CAT_NPIX});"
        f" smoothed {nsel} ({100*nsel/CAT_NPIX:.0f}%)")

    # ---- 2-6. the kinematic fit of the selection, as the app runs it -------
    sigma = robust_sigma(data)                                 # no session estimate here
    fit = kinematic_fit(data, mask, vel, dvk, bpx, sigma, MIN_RING_PIX)
    meas, geom, flat = fit["measures"], fit["geometry"], fit["flat"]
    m0, m1 = meas["m0"], meas["m1"]
    cat_flux = CAT_FSUM_JYHZ * C_KMS / float(h["RESTFRQ"])
    R.update(isolated=np.where(mask & np.isfinite(data), data, np.nan), m0=m0, m1=m1,
             spectrum=meas["spectrum"], flux=meas["flux"], cat_flux=cat_flux,
             vsys_w20=meas["vsys_w20"], w20=meas["w20"], w50=meas["w50"],
             n_velocity_field=meas["n_velocity_field"], sigma_vox=sigma)
    log(f"    velocity field  {meas['n_velocity_field']} px above 3 sigma")
    log(f"[2] integrated flux {meas['flux']:7.2f} Jy km/s (catalogue {cat_flux:.2f})")
    log(f"    systemic vel.   {meas['vsys_w20']:7.1f} km/s (catalogue {CAT_VSYS})"
        f"   w20 {meas['w20']:.1f}  w50 {meas['w50']:.1f} km/s")

    R.update(x0=geom["x0"], y0=geom["y0"], pa_morph=geom["pa_morph"], incl=geom["incl"],
             axis_ratio=geom["axis_ratio"], rmax=geom["rmax"])
    log(f"[3] centre          ({geom['x0']:.1f}, {geom['y0']:.1f}) px")
    log(f"    PA / incl       {geom['pa_morph']:5.1f} deg / {geom['incl']:4.1f} deg"
        f"   (axis ratio {geom['axis_ratio']:.2f}; catalogue {CAT_ELL_PA} / {CAT_AXIS_RATIO})")
    log(f"    rmax            {geom['rmax']:.1f} px")

    R.update(pa_kin=flat["pa"], vsys_fit=flat["vsys"], vrot=flat["vrot"],
             vrot_sini=flat["vrot_sini"], rms_fit=flat["rms"])
    log(f"[4] kinematic PA    {flat['pa']:5.1f} deg  (morphological {geom['pa_morph']:.1f};"
        f" catalogue kin_pa {CAT_KIN_PA})")
    log(f"    V_sys (fitted)  {flat['vsys']:7.1f} km/s   V_rot {flat['vrot']:5.1f} km/s"
        f"   V_rot sin i {flat['vrot_sini']:5.1f}   (w50/2 = {CAT_W50_HALF})")
    log(f"    fit rms         {flat['rms']:5.1f} km/s")

    # The model cubes the overlay draws, from the fitted parameters.
    line_sigma = fit["line_sigma"]
    model = tilted_ring_model(nx, ny, vel, geom["x0"], geom["y0"], flat["pa"], geom["incl"],
                              flat["vsys"], flat["vrot"], geom["rmax"], sigma=line_sigma)
    vmod = fit["vmod_flat"]
    both = np.isfinite(m1) & np.isfinite(vmod)
    resid = np.abs(m1[both] - vmod[both])
    R.update(model_flat=model, vmod_flat=vmod, resid_flat=resid, resid_flat_mask=both,
             line_sigma=line_sigma)
    rf = fit["resid_flat"]
    log(f"[5] |v_data - v_model| over {rf['n_pix']} px: median {rf['median']:.1f}"
        f"  p90 {rf['p90']:.1f} km/s   ({rf['median']/dv:.1f} channels)")
    log(f"    fraction within one channel ({dv:.1f} km/s): {100*rf['within_channel']:.0f}%")

    rings = [(r["r"], r["pa"], r["incl"], r["vrot"], r["n_pix"]) for r in fit["rings"]]
    R["rings"] = rings
    if fit["vmod_warp"] is not None:
        rr_ = np.array([r[0] for r in rings]); pa_ = np.array([r[1] for r in rings])
        in_ = np.array([r[2] for r in rings]); vr_ = np.array([r[3] for r in rings])
        mprof = tilted_ring_model_profile(nx, ny, vel, geom["x0"], geom["y0"], rr_, pa_, in_, vr_,
                                          vsys=flat["vsys"], sigma=line_sigma)
        vmp = fit["vmod_warp"]
        bw = np.isfinite(m1) & np.isfinite(vmp)
        rw = np.abs(m1[bw] - vmp[bw])
        R.update(model_warp=mprof, vmod_warp=vmp, resid_warp=rw, resid_warp_mask=bw)
        log("    ring   r_px   n_px     PA    incl   V_rot")
        for (r_mid, pa_r, in_r, vr_r, n_r) in rings:
            log(f"    {r_mid:7.1f} {n_r:7d} {pa_r:7.1f} {in_r:7.1f} {vr_r:7.1f}")
        rwst = fit["resid_warp"]
        log(f"[6] warped, {len(rings)} rings   PA {pa_.min():.0f}..{pa_.max():.0f} deg, "
            f"incl {in_.min():.0f}..{in_.max():.0f} deg, V_rot {vr_.min():.0f}..{vr_.max():.0f} km/s")
        log(f"    residual        median {rwst['median']:.1f}  p90 {rwst['p90']:.1f} km/s"
            f"   (flat was {rf['median']:.1f} / {rf['p90']:.1f})")
        R["warp_median"] = float(rwst["median"])
    else:
        R["warp_median"] = float("nan")
        log("[6] too few populated rings for a warp")
    return R


def main() -> None:
    R = run()
    resid, rw = R["resid_flat"], R.get("resid_warp", np.array([np.nan]))
    out = HERE / "out" / "usecase_ngc3137.md"
    out.parent.mkdir(exist_ok=True)
    out.write_text(
        "| quantity | measured | reference |\n|---|---|---|\n"
        f"| lasso selection, defaults | {R['n_default']} voxels | SoFiA mask {CAT_NPIX} |\n"
        f"| of which in the 2.5σ component / in the sub-threshold shell | {R['n_in_component']} / {R['n_in_shell']} | component {R['n_component']} voxels, {R['n_component_beyond_budget']} of them beyond the budget |\n"
        f"| lasso selection, smoothed (2,6,6) | {R['n_sel']} voxels | SoFiA mask {CAT_NPIX} |\n"
        f"| integrated flux | {R['flux']:.2f} Jy km/s | catalogue {R['cat_flux']:.2f} |\n"
        f"| systemic velocity (w20 midpoint / fit) | {R['vsys_w20']:.1f} / {R['vsys_fit']:.1f} km/s | catalogue {CAT_VSYS} |\n"
        f"| morphological PA / axis ratio | {R['pa_morph']:.1f} deg / {R['axis_ratio']:.2f} | catalogue {CAT_ELL_PA} / {CAT_AXIS_RATIO} |\n"
        f"| kinematic PA / inclination | {R['pa_kin']:.1f} / {R['incl']:.1f} deg | catalogue kin_pa {CAT_KIN_PA} |\n"
        f"| V_rot / V_rot sin i | {R['vrot']:.1f} / {R['vrot_sini']:.1f} km/s | w50/2 = {CAT_W50_HALF} |\n"
        f"| median velocity residual, flat | {np.median(resid):.1f} km/s | channel {R['dv']:.1f} km/s |\n"
        f"| median velocity residual, warped | {R['warp_median']:.1f} km/s | - |\n")
    print(f"\nwritten: {out}")


if __name__ == "__main__":
    main()
