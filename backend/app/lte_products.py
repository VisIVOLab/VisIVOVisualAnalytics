"""The LTE isotopologue product (issue #416, item 4): maps and per-region numbers
of optical depth, excitation temperature, column densities, N(H₂) and mass from
¹³CO and C¹⁸O (and optionally ¹²CO). The physics is in :mod:`app.lte_co`; this
module reads the data, resolves every assumption and records it.

Two ways in:

* **datasets**: cubes (integrated over a velocity window here) or integrated
  intensity maps (moment 0, K km/s) of ¹³CO and C¹⁸O, optionally ¹²CO (a cube,
  or a map of its peak temperature). C¹⁸O and ¹²CO are put on the ¹³CO grid
  (reprojected when their sky grids differ, as Pixel Math does). Maps are
  written, and regions (canonical image-frame regions on the ¹³CO grid) are
  measured on them.
* **measurements**: the integrals (and optionally peaks) of one region measured
  elsewhere, e.g. Gaussian fits of its mean spectra, with their errors.

Nothing is assumed silently: η_mb, X, T_ex, the abundances and the distance
each come from the request or the header, and the answer (``assumptions``) and
every map's header say which and why.
"""
from __future__ import annotations

import math
import os
import warnings
from typing import Any

import numpy as np
from astropy.io import fits
from astropy.wcs import WCS

from . import lte_co as L

#: Channels of a cube read at once are limited to about this many values.
_BLOCK_VALUES = 16 * 1024 * 1024
_MAD_TO_SIGMA = 1.4826
#: Area of a Gaussian beam over FWHM_maj · FWHM_min: π / (4 ln 2).
_BEAM_AREA_FACTOR = math.pi / (4.0 * math.log(2.0))
#: Flag of a pixel where a line has no data; written as NaN.
FLAG_NO_DATA = -1

MAP_QUANTITIES = {
    "ratio": ("", "W(13CO)/W(C18O) on the T_mb scale"),
    "tau_13co": ("", "optical depth of 13CO from the ratio"),
    "tau_c18o": ("", "optical depth of C18O, tau(13CO)/X"),
    "tex": ("K", "excitation temperature"),
    "n_13co": ("cm-2", "LTE column density of 13CO, opacity corrected"),
    "n_c18o": ("cm-2", "LTE column density of C18O, opacity corrected"),
    "n_h2": ("cm-2", "H2 column density"),
    "flag": ("", L.FLAG_MEANING),
}


# ── assumptions ──────────────────────────────────────────────────────────────

_MAIN_BEAM_TOKENS = ("tmb", "t_mb", "main beam", "main-beam", "mainbeam")


def eta_mb_for(header: fits.Header | None, user: float | None, what: str) -> tuple[float, str]:
    """η_mb to divide T_A* by, and where it came from.

    The user's value wins. Otherwise data already on the main-beam scale
    (TEMPSCAL, BUNIT, BTYPE naming T_mb) take 1; a GILDAS/CLASS header gives
    BEAMEFF/FORWEFF (its T_mb = T_A* · FORWEFF/BEAMEFF), an ETAMB card its value.
    ETAL (JCMT) is the rear-spillover efficiency, already in T_A*, and is never
    used. Anything else must be typed: there is no safe default."""
    if user is not None:
        if not (0.0 < user <= 1.0):
            raise ValueError(f"eta_mb for {what} must be in (0, 1], got {user}.")
        return float(user), "user"
    if header is not None:
        for key in ("TEMPSCAL", "BUNIT", "BTYPE"):
            text = str(header.get(key, "") or "").strip().lower()
            if any(tok in text for tok in _MAIN_BEAM_TOKENS):
                return 1.0, f"data already on the T_mb scale ({key} = '{header.get(key)}')"
        beff, feff = header.get("BEAMEFF"), header.get("FORWEFF")
        try:
            if beff is not None and feff is not None and float(beff) > 0 and float(feff) > 0:
                return float(beff) / float(feff), "header BEAMEFF/FORWEFF"
            if header.get("ETAMB") is not None and 0 < float(header["ETAMB"]) <= 1:
                return float(header["ETAMB"]), "header ETAMB"
        except (TypeError, ValueError):
            pass
    raise ValueError(
        f"eta_mb for {what} is required: the header names no main-beam efficiency "
        "(ETAL is the rear-spillover efficiency, already in T_A*, not eta_mb).")


def resolve_x(req: dict[str, Any], r_gc: float | None) -> tuple[float, str]:
    if req.get("x_ratio") is not None:
        x = float(req["x_ratio"])
        if not (x > 1.0):
            raise ValueError(f"x_ratio must be above 1, got {x}.")
        return x, "user"
    if r_gc is None:
        raise ValueError("The abundance ratio X(13CO)/X(C18O) needs x_ratio, or r_gc_kpc, "
                         "or a distance (with a sky position) to compute R_GC.")
    grad = req.get("carbon_gradient") or "milam2005"
    x = L.abundance_ratio(r_gc, grad)
    return x, (f"(16O/18O)/(12C/13C) at R_GC = {r_gc:.2f} kpc: {L.OXYGEN_GRADIENT[2]}; "
               f"{L.CARBON_GRADIENTS[grad][2]}")


def resolve_r_gc(req: dict[str, Any], glon_glat: tuple[float, float] | None
                 ) -> tuple[float | None, str]:
    if req.get("r_gc_kpc") is not None:
        return float(req["r_gc_kpc"]), "user"
    d = req.get("distance_kpc")
    if d is not None and glon_glat is not None:
        r = L.galactocentric_radius_kpc(glon_glat[0], glon_glat[1], float(d))
        return r, (f"from l = {glon_glat[0]:.4f}, b = {glon_glat[1]:.4f}, d = {float(d):g} kpc, "
                   f"R0 = {L.R0_KPC} kpc")
    return None, ""




# ── reading the data ─────────────────────────────────────────────────────────

class _Line2D:
    """One line: integrated T_A* (K km/s), its σ and the peak T_A* (K, cubes
    only) on the grid ``wcs``; for a cube, also the file and its window, so a
    region's mean spectrum can be read (``own_wcs`` is the cube's own grid)."""

    def __init__(self, w, sigma_w, peak, header, wcs, kind, notes, path=None,
                 window=None, own_wcs=None):
        self.w, self.sigma_w, self.peak = w, sigma_w, peak
        self.header, self.wcs, self.kind, self.notes = header, wcs, kind, notes
        self.path, self.window = path, window
        self.own_wcs = own_wcs if own_wcs is not None else wcs


def _celestial_header(header: fits.Header) -> tuple[WCS, fits.Header]:
    try:
        wcs = WCS(header).celestial
    except Exception:  # noqa: BLE001
        wcs = WCS(naxis=2)
    return wcs, wcs.to_header()


def _window(vel: np.ndarray, v_min: float, v_max: float) -> tuple[int, int]:
    """First and last channel of [v_min, v_max]; the channels between them are
    the window (the axis is monotonic)."""
    lo, hi = min(v_min, v_max), max(v_min, v_max)
    inside = np.flatnonzero((vel >= lo) & (vel <= hi))
    if inside.size == 0:
        raise ValueError(f"No channel lies in {lo:g} to {hi:g} km/s (the cube spans "
                         f"{np.nanmin(vel):g} to {np.nanmax(vel):g}).")
    return int(inside[0]), int(inside[-1])


def read_cube(path: str, v_min: float, v_max: float, rms_chan: float | None,
              need_noise: bool = True) -> _Line2D:
    """∫T dv over [v_min, v_max] (each channel by its own width), the peak, and
    σ_W = σ_chan sqrt(Σ Δv²), which takes the channels as independent. σ_chan is
    the user's, or the robust (MAD) σ of the channels outside the window at each
    pixel. Read in blocks of rows, as float32."""
    from .compute.kinematic_fit import cube_velocity_axis  # noqa: PLC0415
    from .fits_dataset import ScientificFitsDataset, intensity_unit_from_header  # noqa: PLC0415

    ds = ScientificFitsDataset(path)
    hdul, raw = ds.open_memmap()
    try:
        data = ds.squeeze_to_3d(raw)
        if data.ndim != 3:
            raise ValueError(f"{path!r} is not a cube.")
        depth = int(data.shape[0])
        axis = cube_velocity_axis(ds, depth)
        vel, dvk = np.asarray(axis["vel"]), np.asarray(axis["dvk"])
        c0, c1 = _window(vel, v_min, v_max)
        n_off = depth - (c1 - c0 + 1)
        measure = need_noise and rms_chan is None
        if measure and n_off < 10:
            raise ValueError("Fewer than 10 channels outside the velocity window to measure "
                             "the noise: give rms_13co/rms_c18o (K per channel).")
        ny, nx = int(data.shape[1]), int(data.shape[2])
        w = np.full((ny, nx), np.nan)
        peak = np.full((ny, nx), np.nan)
        sig = np.full((ny, nx), np.nan)
        rows = max(1, _BLOCK_VALUES // max(1, depth * nx))
        dv_in = dvk[c0:c1 + 1]
        dv_norm = float(np.sqrt(np.sum(dv_in ** 2)))
        for y0 in range(0, ny, rows):
            y1 = min(ny, y0 + rows)
            sel = np.asarray(data[c0:c1 + 1, y0:y1, :], dtype=np.float32)
            good = np.isfinite(sel).all(axis=0)
            with np.errstate(all="ignore"):
                w[y0:y1] = np.where(good, np.tensordot(dv_in, sel, axes=(0, 0)), np.nan)
                peak[y0:y1] = np.where(good, np.max(sel, axis=0), np.nan)
            del sel
            if measure:
                off = np.concatenate([np.asarray(data[:c0, y0:y1, :], dtype=np.float32),
                                      np.asarray(data[c1 + 1:, y0:y1, :], dtype=np.float32)])
                with np.errstate(all="ignore"), warnings.catch_warnings():
                    warnings.simplefilter("ignore", RuntimeWarning)
                    med = np.nanmedian(off, axis=0)
                    off -= med
                    np.abs(off, out=off)
                    mad = np.nanmedian(off, axis=0)
                sig[y0:y1] = _MAD_TO_SIGMA * mad * dv_norm
            elif rms_chan is not None:
                sig[y0:y1] = float(rms_chan) * dv_norm
        header = ds.header.copy()
    finally:
        hdul.close()
    unit, unit_note = intensity_unit_from_header(header)
    if unit and unit.upper() != "K":
        raise ValueError(f"{path!r} is in {unit}, not K: LTE needs brightness temperatures.")
    wcs, _ = _celestial_header(header)
    notes = [f"integrated over channels {c0 + 1}-{c1 + 1} of {depth} "
             f"({vel[c0]:g} to {vel[c1]:g} km/s, "
             f"{axis['convention']})"]
    if need_noise:
        notes.append("noise: user rms per channel, channels independent" if rms_chan is not None
                     else "noise: robust sigma of the channels outside the window, per pixel, "
                          "channels independent")
    if unit_note:
        notes.append(unit_note)
    return _Line2D(w, sig, peak, header, wcs, "cube", notes, path=path, window=(c0, c1))


def read_map(path: str, rms: float | None, role: str) -> _Line2D:
    """A 2-D map: integrated intensity (K km/s) for ``role = 'integrated'``, a
    peak temperature (K) for ``role = 'peak'``."""
    from .fits_dataset import intensity_unit_from_header  # noqa: PLC0415
    from .products_advanced import _robust_std, load_image2d  # noqa: PLC0415

    img, header, wcs = load_image2d(path)
    img = np.asarray(img, dtype=np.float64)
    unit, unit_note = intensity_unit_from_header(header)
    u = unit.replace(" ", "").lower()
    notes = [unit_note] if unit_note else []
    if role == "integrated":
        if u and u not in ("kkm/s", "kkms-1", "k.km/s", "k*km/s", "kkm.s-1", "kkms**-1"):
            notes.append(f"map unit '{unit}' read as K km/s")
        sigma = float(rms) if rms is not None else _robust_std(img)
        notes.append("noise: user rms" if rms is not None
                     else f"noise: robust sigma of the map ({sigma:.4g}), emission included")
        return _Line2D(img, np.full_like(img, sigma), None, header, wcs, "map", notes)
    if u and u != "k":
        notes.append(f"map unit '{unit}' read as K")
    return _Line2D(None, None, img, header, wcs, "map", notes)


def _align(other: _Line2D, ref: _Line2D) -> tuple[_Line2D, bool]:
    from .products_advanced import _align_to_a  # noqa: PLC0415
    base = ref.w if ref.w is not None else ref.peak
    moved = False
    out = []
    for arr in (other.w, other.sigma_w, other.peak):
        if arr is None:
            out.append(None)
            continue
        a, rep = _align_to_a(np.asarray(base, dtype=np.float32), ref.wcs,
                             np.asarray(arr, dtype=np.float32), other.wcs)
        moved = moved or rep
        out.append(np.asarray(a, dtype=np.float64))
    return _Line2D(out[0], out[1], out[2], other.header, ref.wcs, other.kind, other.notes,
                   path=other.path, window=other.window, own_wcs=other.own_wcs), moved


def _glon_glat(wcs: WCS, shape: tuple[int, int]) -> tuple[float, float] | None:
    if not wcs.has_celestial:
        return None
    try:
        from astropy.coordinates import SkyCoord  # noqa: PLC0415
        c = wcs.pixel_to_world((shape[1] - 1) / 2.0, (shape[0] - 1) / 2.0)
        if not isinstance(c, SkyCoord):
            return None
        g = c.galactic
        return float(g.l.deg), float(g.b.deg)
    except Exception:  # noqa: BLE001
        return None


def _pixel_scales_deg(wcs: WCS) -> tuple[float, float] | None:
    if not wcs.has_celestial:
        return None
    from astropy.wcs.utils import proj_plane_pixel_scales  # noqa: PLC0415
    s = proj_plane_pixel_scales(wcs)
    return float(s[0]), float(s[1])


def _pixels_on(own_wcs: WCS, ref_wcs: WCS, xs: np.ndarray, ys: np.ndarray
               ) -> tuple[np.ndarray, np.ndarray]:
    """Nearest pixels (x, y) of ``own_wcs`` at the sky positions of the
    reference pixels (xs, ys). The positions go through SkyCoord, never through
    raw world values: the two grids may order their celestial axes differently
    (a cutout with GLAT on axis 1 and GLON on axis 2), and may be in different
    frames (Galactic and ICRS), which astropy converts. Positions that fall
    off the sky of ``own_wcs`` are dropped."""
    try:
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            sky = ref_wcs.pixel_to_world(np.asarray(xs, dtype=float), np.asarray(ys, dtype=float))
            px, py = own_wcs.world_to_pixel(sky)
    except Exception as exc:  # noqa: BLE001 — incompatible frames, bad WCS
        raise ValueError(f"The region cannot be placed on the cube's own sky grid: {exc}.") from None
    px, py = np.asarray(px, dtype=np.float64), np.asarray(py, dtype=np.float64)
    keep = np.isfinite(px) & np.isfinite(py)
    return np.rint(px[keep]).astype(int), np.rint(py[keep]).astype(int)


def _region_peak(line: _Line2D, ref_wcs: WCS, ys: np.ndarray, xs: np.ndarray) -> float:
    """Peak T_A* of the region's mean spectrum (a cube), or the mean of a peak
    map. The region's pixels (on the reference grid) are found on the cube's
    own grid through the sky, nearest pixel. A mean of per-pixel peaks would be
    raised by noise and by velocity gradients across the region."""
    if line.kind != "cube" or line.path is None:
        vals = line.peak[ys, xs] if line.peak is not None else np.array([])
        vals = vals[np.isfinite(vals)]
        return float(np.mean(vals)) if vals.size else float("nan")
    from .fits_dataset import ScientificFitsDataset  # noqa: PLC0415
    if line.own_wcs is ref_wcs or not (ref_wcs.has_celestial and line.own_wcs.has_celestial):
        oy, ox = ys, xs
    else:
        ox, oy = _pixels_on(line.own_wcs, ref_wcs, xs, ys)
    ds = ScientificFitsDataset(line.path)
    hdul, raw = ds.open_memmap()
    try:
        data = ds.squeeze_to_3d(raw)
        inb = (ox >= 0) & (ox < data.shape[2]) & (oy >= 0) & (oy < data.shape[1])
        if not inb.any():
            return float("nan")
        pix = np.unique(np.stack([oy[inb], ox[inb]], axis=1), axis=0)
        c0, c1 = line.window
        spectra = np.asarray(data[c0:c1 + 1][:, pix[:, 0], pix[:, 1]], dtype=np.float64)
    finally:
        hdul.close()
    with np.errstate(all="ignore"), warnings.catch_warnings():
        warnings.simplefilter("ignore", RuntimeWarning)
        mean = np.nanmean(spectra, axis=1)
    return float(np.nanmax(mean)) if np.isfinite(mean).any() else float("nan")


# ── the computation ──────────────────────────────────────────────────────────

def _tau_interval(r: float, sr: float, x: float) -> list[float | None]:
    """[τ(R + σ_R), τ(R − σ_R)]: τ falls as R rises. None for an unbounded end."""
    lo_t, lo_f = L.tau_from_ratio(r + sr, x)
    hi_t, hi_f = L.tau_from_ratio(r - sr, x)
    lo = float(lo_t) if int(lo_f) != L.FLAG_NO_SOLUTION else None
    hi = float(hi_t) if int(hi_f) != L.FLAG_NO_SOLUTION else None
    return [lo, hi]


def _finite_or_none(v) -> float | None:
    try:
        f = float(v)
    except (TypeError, ValueError):
        return None
    return f if math.isfinite(f) else None


def _json_safe(obj):
    """Non-finite floats become None: the JSON answer allows no NaN or inf."""
    if isinstance(obj, dict):
        return {k: _json_safe(v) for k, v in obj.items()}
    if isinstance(obj, (list, tuple)):
        return [_json_safe(v) for v in obj]
    if isinstance(obj, (float, np.floating)):
        return _finite_or_none(obj)
    if isinstance(obj, np.integer):
        return int(obj)
    return obj


def valid_tex(t):
    """T_ex where it is finite and above T_bg, NaN elsewhere."""
    t = np.asarray(t, dtype=np.float64)
    return np.where(np.isfinite(t) & (t > L.T_CMB_K), t, np.nan)


def tex_from_13co_peak(peak13_mb, peak18_mb, x: float, ln13: L.Line):
    """T_ex from the ¹³CO peak with τ at the peak, from the ratio of the PEAKS:
    τ from the integrated ratio is a line average, smaller than at the peak,
    and would raise T_ex. NaN where the peaks give no finite positive τ (an
    optically thin line only bounds T_ex from below)."""
    with np.errstate(divide="ignore", invalid="ignore"):
        tau_pk, _ = L.tau_from_ratio(np.asarray(peak13_mb) / np.asarray(peak18_mb), x)
    tau_pk = np.where(tau_pk > 0, tau_pk, np.nan)
    return valid_tex(L.tex_from_peak_and_tau(peak13_mb, tau_pk, ln13))


def lte_quantities(*, w13, s13, w18, s18, x, t_ex, ln13, ln18, snr_min,
                   x_h2: float | None, h2_species: str):
    """Arrays (or scalars) of R, τ₁₃, τ₁₈, flag, N₁₃, N₁₈, N(H₂) from T_mb
    integrals. Below S/N (either line under snr_min σ) τ and N are NaN, flag 3.
    Thin (R ≥ X): τ = 0, N without opacity correction. No solution: NaN.
    N is NaN wherever T_ex is unknown."""
    w13, s13, w18, s18 = (np.asarray(v, dtype=np.float64) for v in (w13, s13, w18, s18))
    with np.errstate(divide="ignore", invalid="ignore"):
        r = w13 / w18
    tau13, flag = L.tau_from_ratio(r, x)
    has = np.isfinite(w13) & np.isfinite(w18) & np.isfinite(s13) & np.isfinite(s18)
    low = has & ~((w13 >= snr_min * s13) & (w18 >= snr_min * s18))
    tau13 = np.where(low | ~has, np.nan, tau13)
    flag = np.where(low, L.FLAG_LOW_SNR, np.where(has, flag, FLAG_NO_DATA)).astype(np.int16)
    tau18 = tau13 / x
    t_ex = valid_tex(t_ex)
    n13 = L.column_density(w13, ln13, t_ex, tau13)
    n18 = L.column_density(w18, ln18, t_ex, tau18)
    known = np.isfinite(tau13) & np.isfinite(t_ex)
    n13 = np.where(known, n13, np.nan)
    n18 = np.where(known, n18, np.nan)
    nh2 = None
    if x_h2 is not None:
        nh2 = (n18 if h2_species == "C18O" else n13) / x_h2
    return r, tau13, tau18, flag, n13, n18, nh2


def compute_lte(req: dict[str, Any], *, path13: str | None = None, path18: str | None = None,
                path12: str | None = None, kinds: dict[str, str] | None = None,
                output_paths: dict[str, str] | None = None) -> dict[str, Any]:
    """The whole product; ``req`` is the route's request as a dict."""
    transition = str(req.get("transition") or "")
    ln13, ln18 = L.line("13CO", transition), L.line("C18O", transition)
    ln12 = L.line("12CO", transition)
    snr_min = float(req.get("snr_min", 3.0))
    assumptions: dict[str, Any] = {
        "transition": transition,
        "line_constants": {
            s.species: {"freq_hz": s.freq_hz, "eu_k": s.eu_k, "a_ul_s": s.a_ul, "g_u": s.g_u,
                        "b_hz": s.b_hz, "source": s.cdms}
            for s in (ln13, ln18, ln12)},
        "t_bg_k": L.T_CMB_K,
        "partition_function": "exact sum over J of (2J+1) exp(-hBJ(J+1)/kT_ex)",
        "column_density": "Mangum & Shirley 2015 (PASP 127, 266) eqs. 29-31, A_ul form, "
                          "opacity corrected by tau/(1-exp(-tau))",
        "snr_min": snr_min,
        "ratio_from": "integrated intensities",
    }
    warnings_out: list[str] = []
    measurements = req.get("measurements")
    if measurements:
        out = _compute_measurements(req, measurements, ln13, ln18, ln12, assumptions,
                                    warnings_out, snr_min)
        return _json_safe(out)
    if not path13 or not path18:
        raise ValueError("Give both a 13CO and a C18O dataset, or measurements.")
    kinds = kinds or {}
    vmin, vmax = req.get("v_min_kms"), req.get("v_max_kms")

    def load(path, kind, rms, role, need_noise=True):
        if kind == "cube":
            if vmin is None or vmax is None:
                raise ValueError("A cube needs v_min_kms and v_max_kms (the line's window).")
            return read_cube(path, float(vmin), float(vmax), rms, need_noise)
        return read_map(path, rms, role)

    d13 = load(path13, kinds.get("13co"), req.get("rms_13co"), "integrated")
    d18 = load(path18, kinds.get("c18o"), req.get("rms_c18o"), "integrated")
    unused12 = bool(path12) and req.get("t_ex_k") is not None
    d12 = (load(path12, kinds.get("12co"), None, "peak", need_noise=False)
           if path12 and not unused12 else None)
    if unused12:
        warnings_out.append("12CO was given but not used: T_ex was typed.")
    d18, moved18 = _align(d18, d13)
    if moved18:
        warnings_out.append("C18O was reprojected onto the 13CO grid (bilinear).")
    if d12 is not None:
        d12, moved12 = _align(d12, d13)
        if moved12:
            warnings_out.append("12CO was reprojected onto the 13CO grid (bilinear).")
    shape = d13.w.shape
    shared = np.isfinite(d13.w) & np.isfinite(d18.w)
    if not shared.any():
        raise ValueError("The 13CO and C18O data share no pixel on the sky.")

    eta13, src13 = eta_mb_for(d13.header, req.get("eta_mb_13co"), "13CO")
    eta18, src18 = eta_mb_for(d18.header, req.get("eta_mb_c18o"), "C18O")
    assumptions["eta_mb"] = {"13CO": [eta13, src13], "C18O": [eta18, src18]}
    w13, s13 = d13.w / eta13, d13.sigma_w / eta13
    w18, s18 = d18.w / eta18, d18.sigma_w / eta18
    assumptions["inputs"] = {"13CO": d13.notes, "C18O": d18.notes}
    if d12 is not None:
        assumptions["inputs"]["12CO"] = d12.notes

    glgb = _glon_glat(d13.wcs, shape)
    r_gc, r_gc_src = resolve_r_gc(req, glgb)
    if r_gc is not None:
        assumptions["r_gc_kpc"] = [r_gc, r_gc_src]
    x, x_src = resolve_x(req, r_gc)
    assumptions["x_ratio"] = [x, x_src]

    tex_mode = _tex_mode(req, d12, d13, d18)
    # η_mb of 12CO only when its peak gives T_ex: a 12CO dataset that is not
    # used (T_ex typed) needs no efficiency.
    eta12 = None
    if tex_mode == "12co":
        eta12, src12 = eta_mb_for(d12.header, req.get("eta_mb_12co"), "12CO")
        assumptions["eta_mb"]["12CO"] = [eta12, src12]
    if tex_mode == "user":
        t_user = _user_tex(req)
        tex = np.full(shape, t_user)
        assumptions["t_ex"] = {"value_k": t_user, "source": "user (an assumption, not measured)"}
    elif tex_mode == "12co":
        tex = valid_tex(L.tex_from_thick_peak(d12.peak / eta12, ln12))
        assumptions["t_ex"] = {"source": "peak T_mb of 12CO, assumed optically thick and filling "
                                         "the beam (a lower limit if it does not)"}
    else:
        tex = tex_from_13co_peak(d13.peak / eta13, d18.peak / eta18, x, ln13)
        assumptions["t_ex"] = {"source": "peak T_mb of 13CO, with tau at the peak from the ratio "
                                         "of the 13CO and C18O peaks; unknown where the peaks "
                                         "give no optical depth"}

    x_h2, h2_species, h2_src = _resolve_h2(req, r_gc)
    if x_h2 is not None:
        assumptions["h2"] = {"from": h2_species, "abundance": x_h2, "source": h2_src}
    else:
        warnings_out.append(h2_src)

    r, tau13, tau18, flag, n13, n18, nh2 = lte_quantities(
        w13=w13, s13=s13, w18=w18, s18=s18, x=x, t_ex=tex, ln13=ln13, ln18=ln18,
        snr_min=snr_min, x_h2=x_h2, h2_species=h2_species)
    tex_map = np.where(np.isfinite(tau13), tex, np.nan)

    scales = _pixel_scales_deg(d13.wcs)
    d_kpc = req.get("distance_kpc")
    area = L.pixel_area_cm2(float(d_kpc), *scales) if (d_kpc is not None and scales) else None
    if area is not None:
        assumptions["distance_kpc"] = float(d_kpc)
        assumptions["pixel_area_cm2"] = area
        assumptions["mu_h2"] = [float(req.get("mu_h2") or L.MU_H2_DEFAULT),
                                "Kauffmann et al. 2008, A&A 487, 993, App. A"]

    beam_pix = _beam_pixels(path13, scales)
    ctx = {"shape": shape, "w13": w13, "s13": s13, "w18": w18, "s18": s18, "tex": tex,
           "tex_mode": tex_mode, "x": x, "ln13": ln13, "ln18": ln18, "ln12": ln12,
           "snr_min": snr_min, "x_h2": x_h2, "h2_species": h2_species, "area": area,
           "beam_pix": beam_pix, "req": req, "lines": (d13, d18, d12),
           "etas": (eta13, eta18, eta12)}
    regions_out = [_region_numbers(i, spec, ctx)
                   for i, spec in enumerate(req.get("regions") or [])]

    flag_map = np.where(flag == FLAG_NO_DATA, np.nan, flag).astype(np.float32)
    maps = {"ratio": np.where(flag == FLAG_NO_DATA, np.nan, r), "tau_13co": tau13,
            "tau_c18o": tau18, "tex": tex_map, "n_13co": n13, "n_c18o": n18, "flag": flag_map}
    if nh2 is not None:
        maps["n_h2"] = nh2
    written = []
    for q, arr in maps.items():
        if output_paths and q in output_paths:
            written.append(_write_map(output_paths[q], q, arr, d13, assumptions))
    counts = {"ok": int(np.sum(flag == L.FLAG_OK)), "thin": int(np.sum(flag == L.FLAG_THIN)),
              "no_solution": int(np.sum(flag == L.FLAG_NO_SOLUTION)),
              "below_snr": int(np.sum(flag == L.FLAG_LOW_SNR)),
              "no_tex": int(np.sum(np.isfinite(tau13) & ~np.isfinite(tex)))}
    return _json_safe({"assumptions": assumptions, "warnings": warnings_out,
                       "regions": regions_out, "maps": written, "pixel_flags": counts,
                       "width": int(shape[1]), "height": int(shape[0])})


def _remove_files(paths, keep: set[str] | None = None) -> None:
    for p in paths:
        if keep and p in keep:
            continue
        try:
            os.unlink(p)
        except OSError:
            pass


def run_lte_job(req: dict[str, Any], *, path13=None, path18=None, path12=None, kinds=None,
                output_paths: dict[str, str] | None = None) -> dict[str, Any]:
    """:func:`compute_lte` owning the files reserved for its maps: on failure
    every one is removed (maps already written included, none was reported),
    on success the ones not written. It runs in a worker thread, so this
    happens even when the request that started it was cancelled."""
    output_paths = output_paths or {}
    try:
        result = compute_lte(req, path13=path13, path18=path18, path12=path12, kinds=kinds,
                             output_paths=output_paths)
    except BaseException:
        _remove_files(output_paths.values())
        raise
    _remove_files(output_paths.values(), keep={m["output_path"] for m in result["maps"]})
    return result


def _user_tex(req) -> float:
    t = float(req["t_ex_k"])
    if not (math.isfinite(t) and t > L.T_CMB_K):
        raise ValueError(f"t_ex_k must exceed T_bg = {L.T_CMB_K} K, got {t}.")
    return t


def _tex_mode(req, d12, d13, d18) -> str:
    if req.get("t_ex_k") is not None:
        return "user"
    if d12 is not None:
        if d12.peak is None:
            raise ValueError("12CO gives T_ex from its peak: give a cube or a peak map.")
        return "12co"
    if d13.peak is not None and d18.peak is not None:
        return "13co"
    raise ValueError("T_ex is needed: give t_ex_k, a 12CO dataset, or 13CO and C18O as cubes "
                     "(their peaks give T_ex with the optical depth at the peak).")


def _resolve_h2(req, r_gc):
    species = "C18O" if (req.get("h2_from") or "c18o").lower() == "c18o" else "13CO"
    key = "abundance_c18o" if species == "C18O" else "abundance_13co"
    if req.get(key) is not None:
        return float(req[key]), species, "user"
    if r_gc is None:
        return None, species, ("N(H2) and mass not computed: give r_gc_kpc or distance_kpc, or "
                               f"{key}.")
    x12 = float(req.get("x12co") or L.X12CO_DEFAULT)
    xa, src = L.isotopologue_abundance(species, r_gc, req.get("carbon_gradient") or "milam2005",
                                       x12)
    return xa, species, src + "; X(12CO) from Frerking, Langer & Wilson 1982, ApJ 262, 590"


def _beam_pixels(path: str, scales) -> float:
    """Pixels per beam (1 when no beam is known): pixels inside one beam are
    not independent, so a region mean's sigma uses n / beam_pix. The beam is
    the header's or the one typed for the dataset (/v1/datasets/beam)."""
    from .fits_dataset import ScientificFitsDataset  # noqa: PLC0415
    try:
        beam = ScientificFitsDataset(path).beam
        major, minor = beam.major, beam.minor
    except Exception:  # noqa: BLE001
        major = minor = None
    if not major or not minor or not scales:
        return 1.0
    return max(1.0, _BEAM_AREA_FACTOR * major * minor / (scales[0] * scales[1]))


def _region_pixels(i: int, spec: dict, shape) -> tuple[np.ndarray, np.ndarray]:
    from .region_stats import Region  # noqa: PLC0415
    try:
        reg = Region(spec)
        x0, x1, y0, y1 = reg.bounds()
    except (KeyError, TypeError, ValueError) as exc:
        raise ValueError(f"Region {i} is not a valid region: {exc!r}.") from None
    x0, y0 = max(0, x0), max(0, y0)
    x1, y1 = min(shape[1] - 1, x1), min(shape[0] - 1, y1)
    if x0 > x1 or y0 > y1:
        return np.array([], dtype=int), np.array([], dtype=int)
    yy, xx = np.mgrid[y0:y1 + 1, x0:x1 + 1]
    m = reg.mask(xx, yy)
    return yy[m], xx[m]


def _region_numbers(i, spec, c):
    shape = c["shape"]
    ys, xs = _region_pixels(i, spec, shape)
    w13, s13, w18, s18 = c["w13"], c["s13"], c["w18"], c["s18"]
    if ys.size:
        ok = (np.isfinite(w13[ys, xs]) & np.isfinite(w18[ys, xs])
              & np.isfinite(s13[ys, xs]) & np.isfinite(s18[ys, xs]))
        ys, xs = ys[ok], xs[ok]
    n = int(ys.size)
    out: dict[str, Any] = {"index": i, "n_pixels": n, "beam_pixels": c["beam_pix"]}
    if n == 0:
        out["error"] = "The region holds no pixel where both lines have data."
        return out
    n_ind = max(1.0, n / c["beam_pix"])
    vals = (float(np.mean(w13[ys, xs])), float(np.sqrt(np.mean(s13[ys, xs] ** 2)) / math.sqrt(n_ind)),
            float(np.mean(w18[ys, xs])), float(np.sqrt(np.mean(s18[ys, xs] ** 2)) / math.sqrt(n_ind)))
    d13, d18, d12 = c["lines"]
    eta13, eta18, eta12 = c["etas"]
    mode = c["tex_mode"]
    if mode == "user":
        t_ex = float(c["req"]["t_ex_k"])
    elif mode == "12co":
        pk = _region_peak(d12, d13.wcs, ys, xs) / eta12
        t_ex = float(valid_tex(L.tex_from_thick_peak(pk, c["ln12"])))
    else:
        pk13 = _region_peak(d13, d13.wcs, ys, xs) / eta13
        pk18 = _region_peak(d18, d13.wcs, ys, xs) / eta18
        t_ex = float(tex_from_13co_peak(pk13, pk18, c["x"], c["ln13"]))
        out["peak_13co_k"], out["peak_c18o_k"] = pk13, pk18
    out.update(_scalar_lte(*vals, c["x"], t_ex, c["ln13"], c["ln18"], c["snr_min"],
                           c["x_h2"], c["h2_species"]))
    out["t_ex_k"] = _finite_or_none(t_ex)
    if c["area"] is not None and out.get("n_h2_cm2") is not None:
        mu = float(c["req"].get("mu_h2") or L.MU_H2_DEFAULT)
        out["mass_msun"] = L.mass_msun(out["n_h2_cm2"] * n, c["area"], mu)
    return out


def _scalar_lte(w13, s13, w18, s18, x, t_ex, ln13, ln18, snr_min, x_h2, h2_species):
    r, tau13, tau18, flag, n13, n18, nh2 = lte_quantities(
        w13=w13, s13=s13, w18=w18, s18=s18, x=x, t_ex=t_ex, ln13=ln13, ln18=ln18,
        snr_min=snr_min, x_h2=x_h2, h2_species=h2_species)
    sr = float(L.ratio_error(w13, s13, w18, s18))
    f = int(flag)
    res = {
        "w_13co_kkms": w13, "sigma_w_13co_kkms": s13,
        "w_c18o_kkms": w18, "sigma_w_c18o_kkms": s18,
        "ratio": _finite_or_none(r), "sigma_ratio": _finite_or_none(sr),
        "tau_13co": _finite_or_none(tau13), "tau_c18o": _finite_or_none(tau18),
        "tau_13co_interval": (_tau_interval(float(r), sr, x)
                              if f in (L.FLAG_OK, L.FLAG_THIN) and math.isfinite(sr)
                              else [None, None]),
        "flag": f,
        "n_13co_cm2": _finite_or_none(n13), "n_c18o_cm2": _finite_or_none(n18),
        "n_h2_cm2": _finite_or_none(nh2) if nh2 is not None else None,
    }
    if f == L.FLAG_THIN:
        res["note"] = "R >= X: optically thin, column densities without opacity correction"
    elif f == L.FLAG_NO_SOLUTION:
        res["note"] = "R <= 1: no LTE solution (self-absorption, different filling, or noise)"
    elif f == L.FLAG_LOW_SNR:
        res["note"] = f"below {snr_min:g} sigma in one of the lines"
    if f in (L.FLAG_OK, L.FLAG_THIN) and res["n_13co_cm2"] is None:
        res["note"] = (res.get("note", "") + "; " if res.get("note") else "") + \
            "T_ex unknown: no column densities"
    return res


def _compute_measurements(req, m, ln13, ln18, ln12, assumptions, warnings_out, snr_min):
    scale = str(m.get("scale") or "ta*").lower()
    if scale not in ("ta*", "tmb"):
        raise ValueError("measurements.scale must be 'ta*' or 'tmb'.")
    eta12 = None
    if scale == "tmb":
        eta13 = eta18 = eta12 = 1.0
        assumptions["eta_mb"] = {"all": [1.0, "measurements already on the T_mb scale"]}
    else:
        eta13, s1 = eta_mb_for(None, req.get("eta_mb_13co"), "13CO")
        eta18, s2 = eta_mb_for(None, req.get("eta_mb_c18o"), "C18O")
        assumptions["eta_mb"] = {"13CO": [eta13, s1], "C18O": [eta18, s2]}
        if m.get("peak_12co") is not None and req.get("t_ex_k") is None:
            eta12, s3 = eta_mb_for(None, req.get("eta_mb_12co"), "12CO")
            assumptions["eta_mb"]["12CO"] = [eta12, s3]
    w13, s13 = float(m["w_13co"]) / eta13, float(m["sigma_13co"]) / eta13
    w18, s18 = float(m["w_c18o"]) / eta18, float(m["sigma_c18o"]) / eta18
    assumptions["inputs"] = "measurements (integrals of one region)"
    glgb = None
    if m.get("glon_deg") is not None and m.get("glat_deg") is not None:
        glgb = (float(m["glon_deg"]), float(m["glat_deg"]))
    r_gc, r_gc_src = resolve_r_gc(req, glgb)
    if r_gc is not None:
        assumptions["r_gc_kpc"] = [r_gc, r_gc_src]
    x, x_src = resolve_x(req, r_gc)
    assumptions["x_ratio"] = [x, x_src]
    x_h2, species, h2_src = _resolve_h2(req, r_gc)
    if x_h2 is not None:
        assumptions["h2"] = {"from": species, "abundance": x_h2, "source": h2_src}
    else:
        warnings_out.append(h2_src)
    res: dict[str, Any] = {}
    if req.get("t_ex_k") is not None:
        t_ex = _user_tex(req)
        assumptions["t_ex"] = {"value_k": t_ex, "source": "user (an assumption, not measured)"}
    elif m.get("peak_12co") is not None:
        t_ex = float(valid_tex(L.tex_from_thick_peak(float(m["peak_12co"]) / eta12, ln12)))
        assumptions["t_ex"] = {"source": "peak T_mb of 12CO, assumed optically thick"}
    elif m.get("peak_13co") is not None and m.get("peak_c18o") is not None:
        pk13, pk18 = float(m["peak_13co"]) / eta13, float(m["peak_c18o"]) / eta18
        t_ex = float(tex_from_13co_peak(pk13, pk18, x, ln13))
        res["peak_13co_k"], res["peak_c18o_k"] = pk13, pk18
        assumptions["t_ex"] = {"source": "peak T_mb of 13CO, with tau at the peak from the ratio "
                                         "of the 13CO and C18O peaks"}
    else:
        raise ValueError("T_ex is needed: give t_ex_k, measurements.peak_12co, or "
                         "measurements.peak_13co and peak_c18o.")
    res.update(_scalar_lte(w13, s13, w18, s18, x, t_ex, ln13, ln18, snr_min, x_h2, species))
    res["t_ex_k"] = _finite_or_none(t_ex)
    res["index"] = 0
    npix, pix_arcsec = m.get("n_pixels"), m.get("pixel_arcsec")
    d_kpc = req.get("distance_kpc")
    if npix and pix_arcsec and d_kpc is not None and res.get("n_h2_cm2") is not None:
        area = L.pixel_area_cm2(float(d_kpc), float(pix_arcsec) / 3600.0,
                                float(pix_arcsec) / 3600.0)
        mu = float(req.get("mu_h2") or L.MU_H2_DEFAULT)
        res["mass_msun"] = L.mass_msun(res["n_h2_cm2"] * int(npix), area, mu)
        res["n_pixels"] = int(npix)
        assumptions["distance_kpc"] = float(d_kpc)
        assumptions["mu_h2"] = [mu, "Kauffmann et al. 2008, A&A 487, 993, App. A"]
    return {"assumptions": assumptions, "warnings": warnings_out, "regions": [res], "maps": [],
            "pixel_flags": {}, "width": 0, "height": 0}


# ── output ───────────────────────────────────────────────────────────────────

def _card(header: fits.Header, key: str, value, comment: str) -> None:
    """A card whose comment is cut to fit 80 characters with its value."""
    room = max(0, 80 - 33)
    header[key] = (value, comment[:room])


def _write_map(path: str, quantity: str, arr, ref: _Line2D, assumptions) -> dict[str, Any]:
    bunit, title = MAP_QUANTITIES[quantity]
    data = np.asarray(arr, dtype=np.float32)
    _, header = _celestial_header(ref.header)
    header["BUNIT"] = bunit
    header["BTYPE"] = quantity
    _card(header, "LTEQUANT", quantity, title)
    _card(header, "LTETRANS", assumptions["transition"], "rotational transition J_u-J_l")
    eta = assumptions.get("eta_mb", {})
    for sp, key in (("13CO", "ETAMB13"), ("C18O", "ETAMB18"), ("12CO", "ETAMB12")):
        if sp in eta:
            _card(header, key, float(eta[sp][0]), f"eta_mb {sp}: {eta[sp][1]}")
    _card(header, "XRATIO", float(assumptions["x_ratio"][0]), "X(13CO)/X(C18O)")
    if "r_gc_kpc" in assumptions:
        _card(header, "RGC_KPC", float(assumptions["r_gc_kpc"][0]), "galactocentric radius, kpc")
    _card(header, "TBG_K", L.T_CMB_K, "background temperature, K")
    if "value_k" in assumptions["t_ex"]:
        _card(header, "TEX_K", assumptions["t_ex"]["value_k"], "assumed T_ex, K")
    _card(header, "SNRMIN", float(assumptions["snr_min"]), "S/N threshold on both lines")
    for sp, key in (("13CO", "13"), ("C18O", "18")):
        c = assumptions["line_constants"][sp]
        _card(header, f"NU{key}", c["freq_hz"], f"{sp} rest frequency, Hz")
        _card(header, f"AUL{key}", c["a_ul_s"], f"{sp} A_ul, s-1 ({c['source']})")
        _card(header, f"EU{key}", c["eu_k"], f"{sp} E_u/k, K")
    _card(header, "GU", assumptions["line_constants"]["13CO"]["g_u"], "g_u = 2J_u+1")
    if "h2" in assumptions:
        _card(header, "XH2", assumptions["h2"]["abundance"],
              f"X({assumptions['h2']['from']}) relative to H2")
    if "distance_kpc" in assumptions:
        _card(header, "DIST_KPC", assumptions["distance_kpc"], "distance, kpc")
    for line_text in _provenance_lines(assumptions, quantity):
        header.add_history(line_text)
    fits.PrimaryHDU(data=data, header=header).writeto(path, overwrite=True)
    finite = data[np.isfinite(data)]
    return {"quantity": quantity, "output_path": path, "bunit": bunit,
            "range_min": float(finite.min()) if finite.size else 0.0,
            "range_max": float(finite.max()) if finite.size else 0.0}


def _provenance_lines(a: dict[str, Any], quantity: str) -> list[str]:
    lines = [f"VisIVO LTE CO isotopologues: {quantity} ({MAP_QUANTITIES[quantity][1]})",
             "T_mb = T_A*/eta_mb; R = (1-exp(-tau13))/(1-exp(-tau13/X))",
             "R from integrated intensities; both lines share J(T_ex)-J(T_bg)",
             "N: " + a["column_density"],
             "Q: " + a["partition_function"],
             "Line constants: CDMS (Mueller et al. 2005), entries 029501, 030502, 028503",
             f"X = {a['x_ratio'][0]:.4g}: {a['x_ratio'][1]}",
             f"T_ex: {a['t_ex'].get('value_k', '')} {a['t_ex']['source']}".strip()]
    for sp, (val, src) in a.get("eta_mb", {}).items():
        lines.append(f"eta_mb {sp} = {val:.4g} ({src})")
    if "h2" in a:
        lines.append(f"N(H2) = N({a['h2']['from']})/{a['h2']['abundance']:.4g}: {a['h2']['source']}")
    inputs = a.get("inputs")
    if isinstance(inputs, dict):
        for sp, notes in inputs.items():
            for n in notes:
                lines.append(f"{sp}: {n}")
    lines.append("flag: " + L.FLAG_MEANING)
    out = []
    for text in lines:
        while len(text) > 70:
            out.append(text[:70])
            text = "  " + text[70:]
        out.append(text)
    return out
