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

from app.compute.selection import cube_kinematic_select        # noqa: E402
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


def beam_area_px(h) -> float:
    """Gaussian beam solid angle in pixels (BMAJ/BMIN are FWHM in degrees)."""
    px = abs(float(h["CDELT1"])) * abs(float(h["CDELT2"]))
    return (np.pi * float(h["BMAJ"]) * float(h["BMIN"])) / (4.0 * np.log(2.0) * px)


def _deproject(dxc, dyc, pa_deg, incl_deg):
    """The model's own deprojection: cos(theta) and radius for a PA/incl pair."""
    par = np.radians(pa_deg)
    ci = max(np.cos(np.radians(incl_deg)), 1e-3)
    xd = (-dxc * np.sin(par) + dyc * np.cos(par))
    yd = (-dxc * np.cos(par) - dyc * np.sin(par)) / ci
    rr = np.hypot(xd, yd)
    with np.errstate(invalid="ignore", divide="ignore"):
        cost = np.where(rr > 0, xd / rr, 0.0)
    return cost, rr


def _velocity_field(model, vel):
    """Intensity-weighted mean velocity of a model cube, NaN where it is empty."""
    mw = np.nansum(model, axis=0)
    with np.errstate(invalid="ignore"):
        v = np.nansum(model * vel[:, None, None], axis=0) / mw
    return np.where(mw > 0, v, np.nan)


def run(cube: str = CUBE, verbose: bool = True) -> dict:
    log = print if verbose else (lambda *a, **k: None)
    if not Path(cube).exists():
        sys.exit(f"cube not found: {cube} (set VISIVO_WALLABY_CUBE)")
    h = fits.getheader(cube)
    data = np.squeeze(fits.getdata(cube)).astype(np.float32)
    nz, ny, nx = data.shape
    dv = abs(float(h["CDELT3"])) / 1000.0                      # km/s per channel
    vel = (float(h["CRVAL3"]) + (np.arange(nz) + 1 - float(h["CRPIX3"]))
           * float(h["CDELT3"])) / 1000.0                      # km/s, descending
    bpx = beam_area_px(h)
    R: dict = dict(header=h, data=data, vel=vel, dv=dv, beam_px=bpx)
    log(f"cube {nx}x{ny}x{nz}   dv={dv:.2f} km/s   beam={bpx:.1f} px")

    # ---- 1. one click ----------------------------------------------------
    mask_default = cube_kinematic_select(data, SEED, reach=REACH, lambda_v=LAMBDA_V)
    mask = cube_kinematic_select(data, SEED, reach=REACH, lambda_v=LAMBDA_V,
                                 smooth_zyx=SMOOTH)
    nsel = int(mask.sum())
    R.update(mask_default=mask_default, mask=mask, n_default=int(mask_default.sum()), n_sel=nsel)
    log(f"\n[1] lasso, defaults {int(mask_default.sum())} voxels "
        f"({100*mask_default.sum()/CAT_NPIX:.0f}% of the SoFiA mask's {CAT_NPIX});"
        f" smoothed {nsel} ({100*nsel/CAT_NPIX:.0f}%)")

    # ---- 2. isolate, then measure on the RAW cube under the mask ---------
    d = np.where(mask & np.isfinite(data), data, np.nan)
    m0 = np.nansum(d, axis=0) * dv / bpx                       # Jy km/s per pixel
    with np.errstate(invalid="ignore"):
        wsum = np.nansum(d, axis=0)
        m1 = np.nansum(d * vel[:, None, None], axis=0) / wsum  # km/s
    # A moment-1 map needs a signal cut, not merely a non-zero denominator: a
    # pixel whose masked channels sum to a hair above zero yields an arbitrary
    # velocity, and a handful of those dominate any least-squares fit. Noise on
    # the summed map grows as sqrt(nchan), so the cut is per pixel.
    nchan = np.nansum(mask, axis=0)
    finite = data[np.isfinite(data)]
    sigma_vox = float(1.4826 * np.median(np.abs(finite - np.median(finite))))
    snr_ok = wsum > 3.0 * sigma_vox * np.sqrt(np.maximum(nchan, 1))
    in_axis = (m1 >= vel.min()) & (m1 <= vel.max())
    m1 = np.where(np.isfinite(wsum) & snr_ok & in_axis, m1, np.nan)
    n_vf = int(np.isfinite(m1).sum())
    log(f"    velocity field  {n_vf} px above 3 sigma")

    spectrum = np.nansum(d, axis=(1, 2)) / bpx                 # Jy per channel
    flux = float(np.nansum(spectrum) * dv)                     # Jy km/s
    # SoFiA-2 already divides by the beam solid angle when the header carries a
    # beam, so f_sum is a flux (Jy Hz), not a flux density summed over beams.
    cat_flux = CAT_FSUM_JYHZ * C_KMS / float(h["RESTFRQ"])
    w = spectrum > 0.2 * spectrum.max()
    vsys_w20 = float(0.5 * (vel[w].min() + vel[w].max()))      # w20 midpoint
    R.update(isolated=d, m0=m0, m1=m1, spectrum=spectrum, flux=flux, cat_flux=cat_flux,
             vsys_w20=vsys_w20, n_velocity_field=n_vf, sigma_vox=sigma_vox)
    log(f"[2] integrated flux {flux:7.2f} Jy km/s (catalogue {cat_flux:.2f})")
    log(f"    systemic vel.   {vsys_w20:7.1f} km/s (catalogue {CAT_VSYS})")

    # ---- 3. geometry from the moment maps --------------------------------
    good = np.isfinite(m0) & (m0 > 0)
    yy, xx = np.mgrid[0:ny, 0:nx]
    wgt = np.where(good, m0, 0.0)
    x0 = float((wgt * xx).sum() / wgt.sum())
    y0 = float((wgt * yy).sum() / wgt.sum())
    dxc, dyc = xx - x0, yy - y0
    mxx = float((wgt * dxc * dxc).sum() / wgt.sum())
    myy = float((wgt * dyc * dyc).sum() / wgt.sum())
    mxy = float((wgt * dxc * dyc).sum() / wgt.sum())
    theta = 0.5 * np.arctan2(2 * mxy, mxx - myy)
    common, diff = mxx + myy, np.hypot(mxx - myy, 2 * mxy)
    a = np.sqrt(max(0.5 * (common + diff), 1e-9)); b = np.sqrt(max(0.5 * (common - diff), 1e-9))
    axis_ratio = b / a
    incl = float(np.degrees(np.arccos(np.clip(axis_ratio, 0.0, 1.0))))
    pa_morph = float((np.degrees(theta) + 90.0) % 180.0)
    rmax = float(2.5 * a)   # 2.5 sigma of the light distribution covers the disc
    R.update(x0=x0, y0=y0, pa_morph=pa_morph, incl=incl, axis_ratio=float(axis_ratio), rmax=rmax)
    log(f"[3] centre          ({x0:.1f}, {y0:.1f}) px")
    log(f"    PA / incl       {pa_morph:5.1f} deg / {incl:4.1f} deg   (axis ratio {axis_ratio:.2f};"
        f" catalogue {CAT_ELL_PA} / {CAT_AXIS_RATIO})")
    log(f"    rmax            {rmax:.1f} px")

    # ---- 4. kinematic PA and rotation, fitted with the MODEL's own geometry
    # The morphological PA from the moment-0 ellipse is defined up to 180 deg and
    # need not share the model's angle convention; a kinematic model must be fitted
    # in the frame it will be evaluated in. So we scan the position angle over the
    # full circle and, at each one, solve the linear problem for vsys and vrot
    # using exactly the deprojection tilted_ring_model() applies.
    ok = np.isfinite(m1) & good
    si = np.sin(np.radians(incl))
    best = None
    for pa_try in np.arange(0.0, 360.0, 1.0):
        cost, _ = _deproject(dxc, dyc, pa_try, incl)
        A = np.column_stack([np.ones(int(ok.sum())), cost[ok] * si])
        sol, *_ = np.linalg.lstsq(A, m1[ok], rcond=None)
        rms = float(np.sqrt(np.mean((A @ sol - m1[ok]) ** 2)))
        if best is None or rms < best[0]:
            best = (rms, float(pa_try), float(sol[0]), float(sol[1]))
    rms_fit, pa_kin, vsys_fit, vrot = best
    # A negative amplitude is the same disc seen from the other side: fold it into
    # the position angle rather than passing abs(vrot) to the model, which would
    # mirror the velocity field and leave a residual the size of the line width.
    if vrot < 0.0:
        pa_kin = (pa_kin + 180.0) % 360.0
        vrot = -vrot
    vrot_sini = vrot * si
    R.update(pa_kin=pa_kin, vsys_fit=vsys_fit, vrot=vrot, vrot_sini=vrot_sini, rms_fit=rms_fit)
    log(f"[4] kinematic PA    {pa_kin:5.1f} deg  (morphological {pa_morph:.1f}; catalogue kin_pa {CAT_KIN_PA})")
    log(f"    V_sys (fitted)  {vsys_fit:7.1f} km/s   V_rot {vrot:5.1f} km/s"
        f"   V_rot sin i {vrot_sini:5.1f}   (w50/2 = {CAT_W50_HALF})")
    log(f"    fit rms         {rms_fit:5.1f} km/s")
    pa, vsys = pa_kin, vsys_fit

    # ---- 5. model vs data, in velocity -----------------------------------
    line_sigma = max(2.0 * dv, 8.0)
    model = tilted_ring_model(nx, ny, vel, x0, y0, pa, incl, vsys, vrot, rmax, sigma=line_sigma)
    vmod = _velocity_field(model, vel)
    both = ok & np.isfinite(vmod)
    resid = np.abs(m1[both] - vmod[both])
    R.update(model_flat=model, vmod_flat=vmod, resid_flat=resid, resid_flat_mask=both,
             line_sigma=line_sigma)
    log(f"[5] |v_data - v_model| over {int(both.sum())} px: "
        f"median {np.median(resid):.1f}  p90 {np.percentile(resid, 90):.1f} km/s"
        f"   ({np.median(resid)/dv:.1f} channels)")
    log(f"    fraction within one channel ({dv:.1f} km/s): {100*np.mean(resid <= dv):.0f}%")

    # ---- 6. does a warp do better than the flat model? -------------------
    # Inclination per ring comes from the SHAPE of the emission, never from the
    # velocity field: v_los carries V_rot and sin(i) only as a product, so the two
    # are exactly degenerate there and a "fitted" inclination would be an artefact
    # of whatever regularisation produced it. We therefore fit isophotes of the
    # moment-0 map -- second moments of the pixels above a falling series of levels
    # -- and read the axis ratio at the radius each isophote encloses.
    levels = np.percentile(m0[good], [92, 84, 76, 68, 60, 52, 44, 36, 28, 20])
    iso_r, iso_i = [], []
    for lev in levels:
        sel_i = good & (m0 >= lev)
        if int(sel_i.sum()) < 60:
            continue
        wi = np.where(sel_i, m0, 0.0); tot = wi.sum()
        cx = float((wi * xx).sum() / tot); cy = float((wi * yy).sum() / tot)
        ddx, ddy = xx - cx, yy - cy
        axx = float((wi * ddx * ddx).sum() / tot); ayy = float((wi * ddy * ddy).sum() / tot)
        axy = float((wi * ddx * ddy).sum() / tot)
        com, dif = axx + ayy, np.hypot(axx - ayy, 2 * axy)
        aa = np.sqrt(max(0.5 * (com + dif), 1e-9)); bb_ = np.sqrt(max(0.5 * (com - dif), 1e-9))
        iso_r.append(2.0 * aa)
        iso_i.append(float(np.degrees(np.arccos(np.clip(bb_ / aa, 0.0, 1.0)))))
    iso_r = np.asarray(iso_r); iso_i = np.asarray(iso_i)
    if iso_r.size >= 3:
        o = np.argsort(iso_r); iso_r, iso_i = iso_r[o], iso_i[o]
        log(f"    isophotal incl  {iso_i.min():.0f}..{iso_i.max():.0f} deg "
            f"over r = {iso_r.min():.0f}..{iso_r.max():.0f} px")

    _, rad = _deproject(dxc, dyc, pa, incl)
    edges = np.arange(0.0, rmax + 1e-9, max(rmax / 8.0, 4.0))
    rings = []
    for lo, hi in zip(edges[:-1], edges[1:]):
        sel = ok & (rad >= lo) & (rad < hi)
        if int(sel.sum()) < MIN_RING_PIX:
            continue
        bb = None
        for pa_try in np.arange(pa - 40.0, pa + 40.0, 2.0):
            ct, _ = _deproject(dxc, dyc, pa_try, incl)
            basis = ct[sel] * si
            denom = float(np.sum(basis * basis))
            if denom <= 1e-9:
                continue
            vr = float(np.sum((m1[sel] - vsys) * basis) / denom)
            r_ = float(np.sqrt(np.mean((vsys + vr * basis - m1[sel]) ** 2)))
            if bb is None or r_ < bb[0]:
                bb = (r_, float(pa_try), vr)
        if bb is None:
            continue
        r_mid = 0.5 * (lo + hi)
        incl_r = (float(np.interp(r_mid, iso_r, iso_i)) if iso_r.size >= 3 else incl)
        # Re-solve the amplitude at this ring's own inclination: V_rot and sin(i)
        # trade off exactly, so changing one without the other would be a bug.
        ct, _ = _deproject(dxc, dyc, bb[1], incl_r)
        basis = ct[sel] * np.sin(np.radians(incl_r))
        den = float(np.sum(basis * basis))
        vr_r = float(np.sum((m1[sel] - vsys) * basis) / den) if den > 1e-9 else bb[2]
        rings.append((r_mid, bb[1], incl_r, vr_r, int(sel.sum())))
    R["rings"] = rings
    if len(rings) >= 3:
        rr_ = np.array([r[0] for r in rings]); pa_ = np.array([r[1] for r in rings])
        in_ = np.array([r[2] for r in rings]); vr_ = np.array([r[3] for r in rings])
        mprof = tilted_ring_model_profile(nx, ny, vel, x0, y0, rr_, pa_, in_, vr_,
                                          vsys=vsys, sigma=line_sigma)
        vmp = _velocity_field(mprof, vel)
        bw = ok & np.isfinite(vmp)
        rw = np.abs(m1[bw] - vmp[bw])
        R.update(model_warp=mprof, vmod_warp=vmp, resid_warp=rw, resid_warp_mask=bw)
        log("    ring   r_px   n_px     PA    incl   V_rot")
        for (r_mid, pa_r, in_r, vr_r, n_r) in rings:
            log(f"    {r_mid:7.1f} {n_r:7d} {pa_r:7.1f} {in_r:7.1f} {vr_r:7.1f}")
        log(f"[6] warped, {len(rings)} rings   PA {pa_.min():.0f}..{pa_.max():.0f} deg, "
            f"incl {in_.min():.0f}..{in_.max():.0f} deg, V_rot {vr_.min():.0f}..{vr_.max():.0f} km/s")
        log(f"    residual        median {np.median(rw):.1f}  p90 {np.percentile(rw, 90):.1f} km/s"
            f"   (flat was {np.median(resid):.1f} / {np.percentile(resid, 90):.1f})")
        R["warp_median"] = float(np.median(rw))
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
