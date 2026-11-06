"""SoFiA-2 parameters suggested from the cube itself (issue #64).

A user facing SoFiA-2's sixty parameters keeps the defaults, and the defaults
are a guess about a cube SoFiA has not seen: kernels of 3 and 6 pixels whatever
the beam, boxcars of 3-15 channels whatever the channel width, a 5σ clip with
no reliability filter whatever the volume. The cube says most of what is
needed: the beam in pixels, the channel width in km/s, how the noise varies
across the field and the band, whether adjacent channels are correlated, and
how many independent resolution elements there are to model the noise from.

:func:`measure` reads all of it in ONE strided pass over the cube: pairs of
adjacent channels spread over the band, every ``stride``-th row and column of
each, so the reads are a bounded number of planes whatever the cube's size
and the cube is never in memory (it works on a memory map, locally or on a
cluster node, on cubes larger than RAM). :func:`suggest` turns the
measurements into SoFiA-2 2.7 parameters, each with the reason it has that
value; the rules are the radio astronomer's spec for issue #64, section 2,
and are pre-registered for the validation (``validation/sofia_suggest.py``):
change one and the validation has to be re-run on a held-out cube.

The noise is measured as SoFiA measures it (``scfind.statistic = mad``,
``fluxRange = negative``): from the values on the noise side of ZERO, below
it for emission and above it for absorption (the cube SoFiA will search,
after ``input.invert``). Bright sources add no value there; faint emission
filling most of the field or band does pull the noise-side values towards
zero and reads σ low, for SoFiA as for this measurement (+1σ over 75 % of
the field and 60 % of the band reads 0.9σ).

Only spectral-line cubes are in scope: 2-D images, Stokes Q/U/V cubes and
volumes without a spectral axis (simulation grids, VBT fields) are refused
with the reason, since SoFiA's Gaussian noise model does not describe them.
"""
from __future__ import annotations

import dataclasses
import math
import warnings
from dataclasses import asdict, dataclass, field
from typing import Any

import numpy as np

from .finders.sofia2 import DEFAULTS

C_KMS = 299792.458

#: SoFiA-2 version whose defaults and parameter names the rules are written for.
SOFIA_VERSION = "2.7.0"

#: Built-in defaults of SoFiA-2 2.7.0, read from the one table of what
#: SoFiA-2 accepts (``finders.sofia2``).
SOFIA_DEFAULTS: dict[str, str] = DEFAULTS

DATA_TYPES = ("emission", "absorption", "continuum")
LINE_LADDERS = ("hi", "galactic")

#: Line widths (km/s) the spectral boxcars are matched to: dwarf to massive
#: spiral for extragalactic HI; narrow Galactic and molecular lines otherwise.
LADDER_KMS = {"hi": (15.0, 30.0, 60.0, 120.0), "galactic": (2.0, 5.0, 10.0, 20.0)}
#: The narrowest line expected, which sets the minimum spectral size of a
#: source: HI at 8000 K has a thermal FWHM of ~19 km/s; for Galactic lines
#: the ladder's first rung.
NARROWEST_LINE_KMS = {"hi": 20.0, "galactic": 2.0}

HI_REST_HZ = 1420405751.786
#: CO, 13CO and C18O J=1-0 rest frequencies (Hz): the Galactic ladder.
MOLECULAR_REST_HZ = (115.2712018e9, 110.2013543e9, 109.7821734e9)

#: Thresholds of the rules (spec §2.1). N_res is the number of independent
#: resolution elements; the reliability cut-offs come from two cubes only
#: (HCG 44 at 1.26e6, WALLABY NGC 3137 at 0.21e6).
N_RES_RELIABILITY = 2e6
N_RES_RELIABILITY_CAVEAT = 5e5
N_RES_SURVEY = 1e7
#: Largest spatial kernel in beams (R2): effective resolution √(1 + 1.12²) ≈ 1.5 beams.
KMAX_BEAMS = 1.12
#: Continuum subtraction (C2) when the band spans this many of the ladder's widest lines.
CONTSUB_BAND_LINES = 20.0
R_SKY_LOCAL = 1.2
R_CHAN_SPECTRAL = 1.1
RHO_CORRELATED = 0.3
CONT_FRACTION = 0.01
ZERO_BLANK_FRACTION = 0.01
HI_MIN_BAND_KMS = 300.0
#: Fewer channels than this is not a spectral-line cube to search.
MIN_CHANNELS = 8
#: A proposed sub-cube narrower than this (pixels a side) is not proposed.
MIN_REGION_PX = 32

#: Voxels the pass reads (both planes of every pair), and pairs at most.
SAMPLE_VOXELS = 4_000_000
MAX_PAIRS = 96

_MAD_TO_SIGMA = 1.4826


class SuggestRefused(ValueError):
    """The dataset is not a cube SoFiA-2's noise model describes; the message says why."""


@dataclass
class Measurements:
    """What the strided pass measured. ``None`` marks what could not be measured."""

    shape: list[int]                       # z, y, x
    sample_voxels: int
    stride_xy: int
    channel_pairs: int
    sigma: float | None                    # noise-side σ, cube units
    noise_side: str                        # "negative" (emission) or "positive" (absorption)
    sigma_negative: float | None
    sigma_positive: float | None
    bunit: str
    beam_major_arcsec: float | None
    beam_minor_arcsec: float | None
    pixel_arcsec: float | None
    b_px: float | None                     # geometric-mean beam FWHM in pixels
    omega_b_px: float | None               # beam area in pixels
    channel_kms: float | None              # Δv_rest
    channel_note: str
    band_kms: float | None
    rest_frequency_hz: float | None
    spectral_ctype: str
    rho: float | None                      # Pearson of adjacent channels, on noise
    l_z: int
    r_chan: float | None
    r_chan_noise: float | None             # what noise alone gives r_chan here
    r_sky: float | None
    r_sky_noise: float | None              # what noise alone gives r_sky here
    sky_blocks: int
    f_nan: float
    f_zero: float
    zero_blanked: bool
    n_res: float | None
    finite_voxels: float
    cont_fraction: float | None
    cont_strong_fraction: float | None
    cont_slope_sigma: float | None         # change of the offset over the band, in σ
    channel_offsets: float                 # fraction of sampled channels off zero as a whole
    tail_high: int                         # sample voxels > +5σ (σ from below zero)
    tail_low: int                          # sample voxels < −5σ (σ from above zero)
    tail_expected: float                   # per side, for Gaussian noise
    cube_bytes: int
    sofia_memory_bytes: int
    node_memory_bytes: int | None

    def as_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass
class _Pass:
    """The raw sample: ``a[k]`` and ``b[k]`` are channels z_k and z_k + 1 at the
    same strided pixels; NaN where the cube is blank (and, for a zero-blanked
    cube, where it is exactly zero)."""

    a: np.ndarray
    b: np.ndarray
    z: np.ndarray
    stride: int
    f_nan: float
    f_zero: float
    zero_blanked: bool


# ── the cube's geometry, from the header ──────────────────────────────────────

def _spectral_fits_axis(ds) -> int:
    """The spectral FITS axis (1-based), after refusing what is not a
    spectral-line cube of Stokes I."""
    from ..fits_dataset import ScientificFitsDataset  # noqa: PLC0415

    active = list(ds.active_axes)
    for axis in range(1, ds.naxis + 1):
        if str(ds.header.get(f"CTYPE{axis}", "")).strip().upper() != "STOKES":
            continue
        if axis in active:
            raise SuggestRefused(
                "The cube has a Stokes axis with several planes; open the Stokes I plane: "
                "SoFiA-2's noise model does not describe polarisation (Q/U go to RM synthesis).")
        crval = float(ds.header.get(f"CRVAL{axis}", 1.0))
        crpix = float(ds.header.get(f"CRPIX{axis}", 1.0))
        cdelt = float(ds.header.get(f"CDELT{axis}", 1.0))
        stokes = int(round(crval + (1.0 - crpix) * cdelt))
        if stokes != 1:
            raise SuggestRefused(
                f"The cube is Stokes {({2: 'Q', 3: 'U', 4: 'V'}).get(stokes, stokes)}, not I: "
                "SoFiA-2's noise model does not describe polarisation cubes.")
    if len(active) < 3:
        raise SuggestRefused("SoFiA-2 parameters are suggested for spectral-line cubes; "
                             "this dataset is not a 3-D cube.")
    if len(active) > 3:
        raise SuggestRefused("The cube has more than three axes with several planes.")
    if ds.naxis > 4:
        raise SuggestRefused(f"The cube has {ds.naxis} axes; SoFiA-2 reads at most four "
                             "(the fourth of one plane).")
    spectral = ds.cube_numpy_spectral_fits_axis()
    ctype = str(ds.header.get(f"CTYPE{spectral}", "")).strip().upper()
    if not ctype.startswith(ScientificFitsDataset._SPECTRAL_CTYPE_PREFIXES):
        raise SuggestRefused(
            "The cube has no spectral axis (no FREQ/VELO/VRAD/VOPT/WAVE CTYPE): SoFiA-2 "
            "searches spectral-line cubes, not simulation grids or other volumes.")
    # SoFiA-2 2.7 reads the primary HDU and takes axes 1-2 as the sky and
    # axis 3 as the spectrum (a 4th axis only when degenerate): a cube laid
    # out otherwise would be measured in one order and searched in another.
    if ds.hdu_index != 0:
        raise SuggestRefused(
            "The cube is in a FITS extension; SoFiA-2 reads the primary HDU only. "
            "Save the cube as a plain FITS file first.")
    if active != [1, 2, 3] or spectral != 3:
        raise SuggestRefused(
            "SoFiA-2 needs the sky on axes 1-2 and the spectrum on axis 3; this cube's spectral "
            f"axis is axis {spectral} (active axes {active}). Reorder the cube first.")
    if int(ds.axis_sizes[2]) < MIN_CHANNELS:
        raise SuggestRefused(
            f"The cube has {int(ds.axis_sizes[2])} channels: too few for a spectral-line search "
            f"(at least {MIN_CHANNELS}).")
    return spectral


def _rest_frequency(header) -> float | None:
    """The line's rest frequency in Hz: RESTFRQ (FITS), RESTFREQ (older AIPS),
    or FREQ0, where some velocity cubes keep it (HCG 44 carries HI only as
    FREQ0 = 1420405758.58)."""
    for key in ("RESTFRQ", "RESTFREQ", "FREQ0"):
        try:
            value = float(header.get(key) or 0.0)
        except (TypeError, ValueError):
            continue
        if value > 0:
            return value
    return None


def velocity_convention(ctype: str, velref: Any = None) -> tuple[str, str]:
    """``radio``, ``optical`` or ``relativistic`` for a velocity CTYPE, and why.

    FITS Paper III: VRAD radio, VOPT/ZOPT optical, VELO (bare, or with an
    algorithm code such as VELO-F2V) the relativistic apparent velocity. The
    older AIPS form names a frame instead (VELO-HEL, FELO-LSR): FELO is
    optical, and VELO is optical too unless VELREF > 256 marks it radio, as
    wcslib reads it (VELO-HEL with VELREF absent, 0, 1 or 2 → VOPT; 258 →
    VRAD) and as the rest of the app reads HCG 44. At v ≪ c the three agree
    to v/c.
    """
    ctype = ctype.upper().strip()
    if ctype.startswith("VRAD"):
        return "radio", "VRAD"
    if ctype.startswith(("VOPT", "ZOPT", "FELO")):
        return "optical", ctype[:4]
    if ctype.startswith("VELO"):
        suffix = ctype[5:] if len(ctype) > 5 else ""
        if not suffix or any(ch.isdigit() for ch in suffix):
            return "relativistic", "VELO, FITS Paper III"
        try:
            ref = int(velref) if velref is not None else None
        except (TypeError, ValueError):
            ref = None
        if ref is not None and ref > 256:
            return "radio", f"AIPS {ctype}, VELREF={ref}"
        return "optical", f"AIPS {ctype}" + (f", VELREF={ref}" if ref is not None else ", no VELREF")
    return "optical", f"{ctype or 'velocity'} taken as optical"


def channel_width_kms(coords: np.ndarray, unit: str, ctype: str,
                      restfreq_hz: float | None,
                      velref: Any = None) -> tuple[float | None, float | None, str]:
    """Rest-frame channel width and band width in km/s, and how they were read.

    Line widths are physical, so the number of channels a line spans is
    W_rest / Δv_rest (spec §2.0): c|Δν|/ν for a frequency axis (at the band
    centre), |Δv|/(1 + v/c) for an optical velocity, |Δv|/(1 − v/c) for a radio
    one, |Δv|/(1 − v²/c²) for a relativistic one (:func:`velocity_convention`),
    c|Δλ|/λ for wavelength. A frequency axis without a rest frequency gives
    ``None``: the spectral rules then keep SoFiA's defaults.
    """
    import astropy.units as u  # noqa: PLC0415

    c = np.asarray(coords, dtype=np.float64).ravel()
    if c.size < 2 or not np.all(np.isfinite(c)):
        return None, None, "the spectral axis has no usable WCS"
    try:
        q = u.Unit(unit) if unit else None
    except Exception:  # noqa: BLE001 — an unparsable unit is "unknown"
        q = None
    centre = float(np.median(c))
    step = float(np.median(np.abs(np.diff(c))))
    span = float(abs(c[-1] - c[0])) + step
    upper = ctype.upper()
    if q is not None and q.is_equivalent(u.Hz):
        if not restfreq_hz:
            return None, None, "frequency axis without RESTFRQ/RESTFREQ: the line is unknown"
        nu = (centre * q).to(u.Hz).value
        d = (step * q).to(u.Hz).value
        s = (span * q).to(u.Hz).value
        return C_KMS * d / nu, C_KMS * s / nu, "c·Δν/ν at the band centre"
    if q is not None and q.is_equivalent(u.km / u.s):
        v = (centre * q).to(u.km / u.s).value
        d = (step * q).to(u.km / u.s).value
        s = (span * q).to(u.km / u.s).value
        convention, how = velocity_convention(upper, velref)
        beta = v / C_KMS
        if convention == "radio":
            f = 1.0 - beta
            return d / f, s / f, f"radio velocity ({how}), Δv/(1 − v/c)"
        if convention == "relativistic":
            f = 1.0 - beta * beta
            return d / f, s / f, f"relativistic velocity ({how}), Δv/(1 − v²/c²)"
        f = 1.0 + beta
        return d / f, s / f, f"optical velocity ({how}), Δv/(1 + v/c)"
    if q is not None and q.is_equivalent(u.m):
        lam = (centre * q).to(u.m).value
        d = (step * q).to(u.m).value
        s = (span * q).to(u.m).value
        return C_KMS * d / lam, C_KMS * s / lam, "c·Δλ/λ at the band centre"
    return None, None, f"spectral unit {unit!r} cannot be turned into km/s"


# ── the strided pass ──────────────────────────────────────────────────────────

def _plan(nz: int, ny: int, nx: int) -> tuple[np.ndarray, int]:
    """First channels of the pairs, evenly over the band, and the spatial stride."""
    pairs = max(1, min(MAX_PAIRS, nz // 2))
    z = np.unique(np.round(np.linspace(0, nz - 2, pairs)).astype(np.int64))
    per_plane = SAMPLE_VOXELS / (2.0 * z.size)
    stride = max(1, int(round(math.sqrt(ny * nx / per_plane))))
    # The stride is set from the area; on an elongated field (a few rows of a
    # long strip) the rows it keeps would hold far more: count them.
    while math.ceil(ny / stride) * math.ceil(nx / stride) > 2.0 * per_plane:
        stride = int(stride * 1.25) + 1
    return z, stride


class _SpectralView:
    """The stored cube seen as (z, y, x): ``view[z, ys, xs]`` reads one strided
    plane from the stored memory map, the degenerate axes (a Stokes I axis of
    one plane) indexed at 0, without squeezing or copying the map."""

    def __init__(self, stored, shape: tuple[int, ...], spectral_fits_axis: int) -> None:
        self.stored = stored
        self.ndim = len(shape)
        self.spectral = self.ndim - spectral_fits_axis         # numpy axis
        spatial = [i for i, n in enumerate(shape) if n > 1 and i != self.spectral]
        if len(spatial) != 2 or not 0 <= self.spectral < self.ndim:
            raise SuggestRefused("The cube needs a spectral axis and two spatial axes.")
        self.spatial = spatial
        self.shape = (int(shape[self.spectral]), int(shape[spatial[0]]), int(shape[spatial[1]]))

    def __getitem__(self, key):
        z, ys, xs = key
        index: list[Any] = [0] * self.ndim
        index[self.spectral] = int(z)
        index[self.spatial[0]], index[self.spatial[1]] = ys, xs
        return self.stored[tuple(index)]


def _open_stored(ds):
    """The primary HDU as stored, memory-mapped, and the function that turns a
    slice of it into values. Astropy cannot memory-map an image it has to
    scale (BSCALE/BZERO) or blank (an integer BLANK), so the stored values
    are read and the scaling and the blanking applied to the sampled planes
    only. (A tile-compressed cube lives in an extension, which SoFiA-2 does
    not read, and is refused before this.)"""
    from ..fits_dataset import _open_fits  # noqa: PLC0415

    hdul = _open_fits(ds.path, memmap=True, mode="readonly", do_not_scale_image_data=True)
    # A gzip/bzip2/zip file cannot be memory-mapped: ``.data`` would inflate
    # the whole cube in memory. SoFiA-2 reads plain FITS only, so it is refused.
    compression = getattr(getattr(hdul, "_file", None), "compression", None)
    if compression:
        hdul.close()
        raise SuggestRefused(f"The file is {compression}-compressed; SoFiA-2 reads plain FITS files. "
                             "Decompress it first.")
    hdu = hdul[ds.hdu_index]
    try:
        stored = hdu.data
    except Exception:
        hdul.close()
        raise
    shape = tuple(stored.shape) if stored is not None else ()
    if stored is None or not shape:
        hdul.close()
        raise SuggestRefused("The file holds no image data.")
    header = hdu.header
    bscale = float(header.get("BSCALE", 1.0) or 1.0)
    bzero = float(header.get("BZERO", 0.0) or 0.0)
    blank = header.get("BLANK") if int(header.get("BITPIX", -32)) > 0 else None

    def values(stored: np.ndarray) -> np.ndarray:
        stored = np.asarray(stored)
        out = stored.astype(np.float64) if (bscale != 1.0 or bzero != 0.0) else stored.astype(np.float32)
        if bscale != 1.0 or bzero != 0.0:
            out = out * bscale + bzero
        out = out.astype(np.float32, copy=False)
        if blank is not None:
            out[stored == blank] = np.nan
        return out

    return hdul, stored, shape, values


def _read_pass(view, values=None) -> _Pass:
    """Read the channel pairs from ``view`` (z, y, x: an array, or a
    :class:`_SpectralView`), plane by plane; ``values`` turns a stored slice
    into data values (scaling, blanks)."""
    nz, ny, nx = view.shape
    z, s = _plan(nz, ny, nx)
    py, px = len(range(0, ny, s)), len(range(0, nx, s))
    a = np.empty((z.size, py, px), dtype=np.float32)
    b = np.empty_like(a)
    convert = values or (lambda x: x)
    for k, z0 in enumerate(z):
        a[k] = convert(view[z0, ::s, ::s])
        b[k] = convert(view[z0 + 1, ::s, ::s])
    n = a.size + b.size
    n_finite = int(np.count_nonzero(np.isfinite(a))) + int(np.count_nonzero(np.isfinite(b)))
    n_zero = int(np.count_nonzero(a == 0.0)) + int(np.count_nonzero(b == 0.0))
    f_zero = n_zero / n_finite if n_finite else 0.0
    f_nan = 1.0 - n_finite / n if n else 1.0
    zero_blanked = f_zero > ZERO_BLANK_FRACTION and not _quantized(a)
    if zero_blanked:
        a[a == 0.0] = np.nan
        b[b == 0.0] = np.nan
    return _Pass(a=a, b=b, z=z, stride=s, f_nan=f_nan, f_zero=f_zero, zero_blanked=zero_blanked)


def _quantized(a: np.ndarray) -> bool:
    from ..noise_threshold import _quantized as q  # noqa: PLC0415

    v = a[np.isfinite(a)]
    return bool(v.size) and q(v)


def _side_sigma(values: np.ndarray, min_lower: int = 50,
                centre: float | None = None) -> tuple[float | None, float | None, float]:
    """(σ from below the centre, σ from above it, centre) of finite ``values``;
    the centre is their median unless given."""
    v = values[np.isfinite(values)]
    if v.size < 2 * min_lower:
        return None, None, float("nan")
    med = float(np.median(v)) if centre is None else float(centre)
    lower = med - v[v < med]
    upper = v[v > med] - med
    lo = _MAD_TO_SIGMA * float(np.median(lower)) if lower.size >= min_lower else None
    hi = _MAD_TO_SIGMA * float(np.median(upper)) if upper.size >= min_lower else None
    return (lo if lo and lo > 0 else None), (hi if hi and hi > 0 else None), med


def _ratio_p95_p5(values: list[float], minimum: int) -> float | None:
    v = np.asarray([x for x in values if x is not None and x > 0], dtype=np.float64)
    if v.size < minimum:
        return None
    lo = float(np.percentile(v, 5.0))
    return float(np.percentile(v, 95.0)) / lo if lo > 0 else None


#: Channels apart beyond which the noise is taken as uncorrelated (Hanning
#: smoothing correlates neighbours only; a lag-1 ρ of 0.4 that decays
#: geometrically is 0.03 here).
_DECORRELATED_LAG = 4

#: Finite differences a channel pair needs for its own σ.
_MIN_PAIR_VALUES = 200


def _pair_sigma(d: np.ndarray) -> float | None:
    """1.4826·MAD of one pair's finite differences, None when too few."""
    d = d[np.isfinite(d)]
    if d.size < _MIN_PAIR_VALUES:
        return None
    return _MAD_TO_SIGMA * float(np.median(np.abs(d - np.median(d))))


def _median_of(values: list[float | None], minimum: int = 2) -> float | None:
    v = [x for x in values if x]
    return float(np.median(v)) if len(v) >= minimum else None


def _lagged_sigma(p: _Pass) -> float | None:
    """The noise σ from differences of channels at least
    ``_DECORRELATED_LAG`` apart at the same pixel (Var = 2σ² once their noise
    is uncorrelated): the median over the sampled channels of each one's
    σ, so that the few pairs straddling the edge of broad emission do not
    weigh. Like the adjacent difference it is set against, such a difference
    cancels a pixel's continuum or baseline level and keeps the same pixels
    and weights. Each value is paired with the first FINITE value far enough
    along at its pixel, so a blanked plane in between costs nothing. None
    when the band holds too few such pairs."""
    planes = np.concatenate([p.a, p.b])
    z = np.concatenate([p.z, p.z + 1])
    order = np.argsort(z, kind="stable")
    flat = planes[order].reshape(z.size, -1)
    z = z[order]
    n = z.size
    finite = np.isfinite(flat)
    # nxt[j, i]: the first plane at or after j that is finite at pixel i (n: none).
    nxt = np.full((n + 1, flat.shape[1]), n, dtype=np.int32)
    for j in range(n - 1, -1, -1):
        nxt[j] = np.where(finite[j], j, nxt[j + 1])
    later = np.searchsorted(z, z + _DECORRELATED_LAG)
    pixels = np.arange(flat.shape[1])
    sigmas = []
    for i in range(n):
        if later[i] >= n:
            break
        partner = nxt[later[i]]
        ok = finite[i] & (partner < n)
        sigmas.append(_pair_sigma(flat[partner[ok], pixels[ok]] - flat[i, ok]))
    s = _median_of(sigmas)
    return s / math.sqrt(2.0) if s else None


def _channel_correlation(p: _Pass, sigma: float | None) -> float | None:
    """Correlation of the noise in adjacent channels, ρ = 1 − σ_Δ²/(2σ²),
    with σ_Δ the median over the sampled pairs of 1.4826·MAD of b − a, and σ
    the noise σ (Var(b − a) = 2σ²(1 − ρ)), clamped to [−1, 1].

    A difference cancels what the two channels share: continuum, a baseline,
    and the smooth part of a line, and its MAD ignores the few pixels where a
    line edge leaves something; the median over pairs ignores the few pairs
    at the edge of emission that fills the field. A Pearson coefficient on
    the values instead let faint emission below any clip into both channels
    and read it as correlated noise (0.068 on the WALLABY cubelet, whose
    noise SoFiA reads at 0.024)."""
    if not sigma or sigma <= 0:
        return None
    s_d = _median_of([_pair_sigma(b - a) for a, b in zip(p.a.reshape(len(p.a), -1),
                                                         p.b.reshape(len(p.b), -1))], minimum=1)
    if s_d is None:
        return None
    return float(min(1.0, max(-1.0, 1.0 - s_d * s_d / (2.0 * sigma * sigma))))


#: |value − its pixel's level|, in that pixel's own σ, beyond which a
#: sampled value is taken as signal and left out of both differences ρ is
#: read from.
_SIGNAL_CLIP = 3.0
#: Finite sampled values a pixel needs for its own level and σ.
_MIN_LEVEL_VALUES = 16


def _signal_masked(p: _Pass) -> _Pass:
    """``p`` with both channels of a pair blanked at a pixel where either
    sits more than ``_SIGNAL_CLIP`` of that pixel's σ from its level (median
    and 1.4826·MAD over the sampled channels, so a primary-beam gain does not
    decide): a line, wherever it lies in the band and however it moves over
    the field, then reaches neither difference."""
    planes = np.concatenate([p.a, p.b])
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", RuntimeWarning)
        level = np.nanmedian(planes, axis=0)
        reach = _SIGNAL_CLIP * _MAD_TO_SIGMA * np.nanmedian(np.abs(planes - level), axis=0)
        # A pixel with a few finite values has no σ to judge by: kept whole.
        reach[np.count_nonzero(np.isfinite(planes), axis=0) < _MIN_LEVEL_VALUES] = np.inf
        signal = (np.abs(p.a - level) > reach) | (np.abs(p.b - level) > reach)
    # A line's wings sit under the clip next to its core: the pairs either
    # side of a flagged one (along the band) go with it.
    grown = signal.copy()
    grown[1:] |= signal[:-1]
    grown[:-1] |= signal[1:]
    signal = grown
    a = np.where(signal, np.nan, p.a).astype(np.float32)
    b = np.where(signal, np.nan, p.b).astype(np.float32)
    return dataclasses.replace(p, a=a, b=b)


def _noise_rho(p: _Pass, s_ref: float | None) -> float | None:
    """ρ of the noise: from the differences of :func:`_channel_correlation`
    and :func:`_lagged_sigma`, both of which cancel a continuum or baseline
    level, on the sample with the signal left out (:func:`_signal_masked`).
    The σ about zero, which keeps a level, stands in only when the band is
    too short for the lagged σ."""
    clean = _signal_masked(p)
    return _channel_correlation(clean, _lagged_sigma(clean) or s_ref)


def _independent(n_samples: float, stride: int, omega_b: float | None, l_z: int = 1) -> float:
    """Independent noise values among ``n_samples`` strided samples: one per
    beam area once the stride is finer than the beam, one per ``l_z`` channels."""
    per_sample = min(1.0, stride * stride / omega_b) if omega_b else 1.0
    return n_samples * per_sample / max(1, l_z)


#: Relative standard error (×1/√n) of the one-side σ about a fixed centre
#: (the median of a half-normal).
_LOWER_SIDE_SE = 1.65


def noise_only_ratio(n_independent: float, relative_se: float = _LOWER_SIDE_SE) -> float | None:
    """P95/P5 that noise ALONE gives to a set of σ estimates, each from
    ``n_independent`` values with a relative error of ``relative_se``/√n:
    P95/P5 of log-normal estimates spans ±1.645 of it. A small cube reads as
    "noise varying by 1.15×" from this alone (a 96² field of 4-pixel beams:
    ~500 beams per channel), so a variation is real only above it."""
    if not n_independent or n_independent <= 0:
        return None
    return math.exp(2.0 * 1.645 * relative_se / math.sqrt(n_independent))


def _sky_blocks(p: _Pass, shape: tuple[int, int, int], b_px: float | None,
                centre: float) -> tuple[float | None, int, float]:
    """P95/P5 of the noise-side σ in blocks over the field: about four beams a
    side, between 2 and 8 blocks per axis (6 without a beam). The spread is
    read about ``centre`` (zero, as SoFiA reads it), not the block's median,
    which a galaxy filling the block would move. Also returns the blocks
    measured and the median number of sample values in one."""
    _, ny, nx = shape

    def blocks(n: int) -> int:
        if not b_px:
            return 6
        return int(min(8, max(2, round(n / (4.0 * b_px)))))

    nby, nbx = blocks(ny), blocks(nx)
    yy = (np.arange(p.a.shape[1]) * p.stride * nby) // ny
    xx = (np.arange(p.a.shape[2]) * p.stride * nbx) // nx
    label = (yy[:, None] * nbx + xx[None, :]).ravel()
    stack = np.concatenate([p.a.reshape(p.a.shape[0], -1), p.b.reshape(p.b.shape[0], -1)], axis=0)
    sigmas, counts = [], []
    for k in range(nby * nbx):
        cols = np.flatnonzero(label == k)
        if cols.size == 0:
            continue
        values = stack[:, cols].ravel()
        lo, _, _ = _side_sigma(values, min_lower=200, centre=centre)
        sigmas.append(lo)
        if lo is not None:
            # Twice the noise side: the values the estimate is read from.
            counts.append(2 * int(np.count_nonzero(values < centre)))
    # The least-sampled blocks are the noisiest, and set P95/P5.
    n = float(np.percentile(counts, 10.0)) if counts else 0.0
    return _ratio_p95_p5(sigmas, 4), len(counts), n


def _continuum(p: _Pass, sigma: float) -> tuple[float | None, float | None, float | None, np.ndarray | None]:
    """Fraction of sampled pixels whose spectrum sits off zero, the fraction
    with an offset above 5σ (strong, discrete residuals), the change of the
    offset over the band in units of σ, and which sampled pixels carry it.

    Continuum is told from line emission by where it is: under the whole
    band, so at BOTH ends of it, where a line rarely is (a galaxy filling 60 %
    of a cubelet's band, NGC 3137 in WALLABY's, moves the median and the
    lower percentiles of its pixels' spectra, but leaves the band's ends at
    zero). A pixel is flagged when the medians of its first and last fifth of
    the sampled channels (the first channel of each pair) are both off zero,
    with the same sign, by more than 3 standard errors each (1.2533 σ_pix/√n).
    σ_pix is the pixel's own noise, from the differences between successive
    sampled channels, which a continuum or a broad line cancels: noise that
    is higher in part of the field is then not read as an offset. Those
    channels are at least two apart, so Hanning-smoothed noise is nearly
    independent between them (correlation 1/6 at lag 2) and they count as
    independent. Zero is the reference, not the cube's median, which a
    continuum under the whole field moves with it.
    """
    a = p.a.reshape(p.a.shape[0], -1)
    nch = a.shape[0]
    q = max(3, nch // 5)
    if nch < 2 * q or a.shape[1] < 10:
        return None, None, None, None
    with np.errstate(invalid="ignore"), warnings.catch_warnings():
        warnings.simplefilter("ignore", RuntimeWarning)          # all-NaN pixels
        s_pix = _MAD_TO_SIGMA * np.nanmedian(np.abs(np.diff(a, axis=0)), axis=0) / math.sqrt(2.0)
        low = np.nanmedian(a[:q], axis=0)
        high = np.nanmedian(a[-q:], axis=0)
    n_low = np.count_nonzero(np.isfinite(a[:q]), axis=0)
    n_high = np.count_nonzero(np.isfinite(a[-q:]), axis=0)
    usable = (n_low * 2 >= q) & (n_high * 2 >= q) & np.isfinite(s_pix) & (s_pix > 0)
    if np.count_nonzero(usable) < 10:
        return None, None, None, None
    with np.errstate(invalid="ignore", divide="ignore"):
        se_low = 1.2533 * s_pix / np.sqrt(n_low)
        se_high = 1.2533 * s_pix / np.sqrt(n_high)
        flagged = (usable & (np.abs(low) > 3.0 * se_low) & (np.abs(high) > 3.0 * se_high)
                   & (np.sign(low) == np.sign(high)))
    offset = np.where(flagged, 0.5 * (low + high), 0.0)
    n_usable = float(np.count_nonzero(usable))
    frac = float(np.count_nonzero(flagged)) / n_usable
    strong = float(np.count_nonzero(np.abs(offset) > 5.0 * sigma)) / n_usable
    slope = None
    if np.count_nonzero(flagged) >= 10:
        # The change between the two ends, carried over the whole band.
        span = float(np.median(p.z[-q:]) - np.median(p.z[:q]))
        band = float(p.z[-1] - p.z[0] + 2)
        if span > 0:
            slope = float(abs(np.median(high[flagged] - low[flagged])) / span * band / sigma)
    return frac, strong, slope, flagged


# ── measure ──────────────────────────────────────────────────────────────────

def measure(path: str, data_type: str | None = None) -> tuple[Measurements, dict[str, Any]]:
    """Measure ``path`` in one strided pass; returns the measurements and the
    data type (deduced unless ``data_type`` is given)."""
    from ..fits_dataset import ScientificFitsDataset  # noqa: PLC0415
    from ..node_status import memory_limit_bytes  # noqa: PLC0415
    from ._common import beam_area_pixels  # noqa: PLC0415
    from .finders.sofia2 import MEMORY_FACTOR, MEMORY_FLOOR_BYTES  # noqa: PLC0415

    ds = ScientificFitsDataset(path)
    spectral_fits_axis = _spectral_fits_axis(ds)
    hdul, stored, stored_shape, values = _open_stored(ds)
    try:
        view = _SpectralView(stored, stored_shape, spectral_fits_axis)
        if view.shape[0] < 2:
            raise SuggestRefused("The cube needs at least two channels.")
        shape = view.shape
        p = _read_pass(view, values)
    finally:
        hdul.close()
    nz, ny, nx = shape

    # The noise is read as SoFiA reads it (scfind.statistic mad, fluxRange
    # negative): from the values on one side of ZERO, not of the median.
    # Bright emission adds no value below zero, however much of a channel or
    # of the band it fills, while the median moves with it and takes
    # signal-free values for absorption (faint emission over most of the
    # field does pull the negative side in: see the module docstring). The
    # positive side, by the same token, is clean of absorption. Each tail
    # is counted against the OTHER side's σ, the one its own signal does not
    # inflate; they decide the data type, before the sign is chosen.
    pooled = np.concatenate([p.a.ravel(), p.b.ravel()])
    s_lo, s_hi, _ = _side_sigma(pooled, min_lower=500, centre=0.0)
    if s_lo is None and s_hi is None:
        raise SuggestRefused("Too few finite voxels on both sides of zero to measure the noise.")
    s_ref = min(x for x in (s_lo, s_hi) if x is not None)
    v = pooled[np.isfinite(pooled)]
    tail_high = int(np.count_nonzero(v > 5.0 * (s_lo or s_ref)))
    tail_low = int(np.count_nonzero(v < -5.0 * (s_hi or s_ref)))
    tail_expected = 2.867e-7 * v.size

    # The beam, in pixels.
    pix = ds.spatial_pixel_scale_arcsec()
    pix = float(pix) if pix and np.isfinite(pix) and pix > 0 else None
    beam = ds.beam
    if (not beam.major or not beam.minor) and ds.has_per_channel_beam:
        beam = ds.beam_for_channel(nz // 2)
    bmaj = float(beam.major) * 3600.0 if beam.major else None
    bmin = float(beam.minor) * 3600.0 if beam.minor else None
    b_px = math.sqrt(bmaj * bmin) / pix if (bmaj and bmin and pix) else None
    omega = beam_area_pixels(beam.major, beam.minor, pix, ds.spatial_pixel_area_arcsec2()) if b_px else None

    # The channel width in the rest frame.
    spectral = ds.spectral_axis_metadata(0, nz - 1)
    restfreq = _rest_frequency(ds.header)
    ctype = str(spectral.get("axis_type", "") or "")
    dv, band, note = channel_width_kms(np.asarray(spectral.get("coordinates", []), dtype=np.float64),
                                       str(spectral.get("axis_unit", "") or ""), ctype, restfreq,
                                       ds.header.get("VELREF"))
    dv = float(dv) if dv is not None and math.isfinite(dv) and dv > 0 else None
    band = float(band) if band is not None and math.isfinite(band) else None

    # The memory SoFiA-2 needs here, against what this node has.
    # SoFiA holds the data as 32-bit floats at least: a scaled 8- or 16-bit
    # cube takes 4 bytes a voxel once read, not what it takes on disk.
    bitpix = abs(int(ds.header.get("BITPIX", -32)))
    cube_bytes = nz * ny * nx * max(4, bitpix // 8)
    need = int(MEMORY_FACTOR * cube_bytes + MEMORY_FLOOR_BYTES)

    # What does not depend on the sign comes first, so that the data type is
    # final before the sample is inverted for absorption: the correlation
    # (from |b − a| and σ, unchanged when both channels change sign; s_ref is
    # the clean side's σ either way) and the continuum test (offsets either
    # way, against zero).
    cont, strong, slope, _ = _continuum(p, s_ref)
    rho = _noise_rho(p, s_ref)
    l_z = 2 if (rho is not None and rho > RHO_CORRELATED) else 1
    dtype = deduce_data_type(tail_high, tail_low, tail_expected, data_type)
    if dtype["value"] != "continuum" and data_type is None and cont is not None and cont > CONT_FRACTION:
        dtype = _continuum_type(cont, dtype)
    if dtype["value"] == "absorption":
        # Measure the cube SoFiA will search: inverted.
        np.negative(p.a, out=p.a)
        np.negative(p.b, out=p.b)
        sigma = s_hi or s_lo
    else:
        sigma = s_lo or s_hi
    noise_side = "positive" if dtype["value"] == "absorption" else "negative"

    # Each channel's σ from its noise side about zero, as above; a channel
    # whose noise side holds too few values (one sitting far off zero) gives
    # none, and the channel-offset warning below says why.
    centre = 0.0
    planes = np.concatenate([p.a, p.b])
    flat = planes.reshape(planes.shape[0], -1)
    per_channel = [_side_sigma(plane, centre=centre)[0] for plane in flat]
    r_chan = _ratio_p95_p5(per_channel, 8)
    n_plane = float(np.median(np.count_nonzero(np.isfinite(flat), axis=1)))
    # The floor from the values each σ is actually read from (twice the noise
    # side), at the 10th percentile over the channels: emission over part of
    # the field leaves those channels fewer, and their estimates the
    # noisiest, which is what sets P95/P5.
    n_side = [2.0 * np.count_nonzero(plane < centre) for plane, s in zip(flat, per_channel) if s]
    r_chan_noise = (noise_only_ratio(_independent(float(np.percentile(n_side, 10.0)), p.stride, omega))
                    if n_side else None)
    # Channels sitting off zero as a whole: a baseline or bandpass problem,
    # which SoFiA's σ about zero does not survive either.
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", RuntimeWarning)
        chan_med = np.nanmedian(flat, axis=1)
    se_med = 1.2533 * (sigma or 0.0) / math.sqrt(max(1.0, _independent(n_plane, p.stride, omega)))
    off = np.abs(chan_med) > np.maximum(0.5 * (sigma or 0.0), 5.0 * se_med)
    channel_offsets = float(np.count_nonzero(off & np.isfinite(chan_med))) / max(1, flat.shape[0])
    r_sky, n_blocks, n_block = _sky_blocks(p, shape, b_px, centre)
    # A block pools the pairs' two channels, correlated when l_z = 2.
    r_sky_noise = noise_only_ratio(_independent(n_block, p.stride, omega, l_z))

    valid_fraction = float(np.count_nonzero(np.isfinite(pooled))) / pooled.size
    finite_voxels = valid_fraction * nz * ny * nx
    n_res = finite_voxels / (omega * l_z) if omega else None

    m = Measurements(
        shape=[nz, ny, nx], sample_voxels=int(pooled.size), stride_xy=p.stride,
        channel_pairs=int(p.z.size), sigma=sigma, noise_side=noise_side,
        sigma_negative=s_lo, sigma_positive=s_hi, bunit=ds.bunit,
        beam_major_arcsec=bmaj, beam_minor_arcsec=bmin, pixel_arcsec=pix, b_px=b_px,
        omega_b_px=omega, channel_kms=dv, channel_note=note, band_kms=band,
        rest_frequency_hz=restfreq, spectral_ctype=ctype, rho=rho, l_z=l_z,
        r_chan=r_chan, r_chan_noise=r_chan_noise, r_sky=r_sky, r_sky_noise=r_sky_noise,
        sky_blocks=n_blocks, f_nan=p.f_nan, f_zero=p.f_zero,
        zero_blanked=p.zero_blanked, n_res=n_res, finite_voxels=finite_voxels,
        cont_fraction=cont, cont_strong_fraction=strong, cont_slope_sigma=slope,
        channel_offsets=channel_offsets,
        tail_high=tail_high, tail_low=tail_low, tail_expected=tail_expected,
        cube_bytes=cube_bytes, sofia_memory_bytes=need, node_memory_bytes=memory_limit_bytes(),
    )
    return m, dtype


def deduce_data_type(tail_high: int, tail_low: int,
                     tail_expected: float, override: str | None) -> dict[str, Any]:
    """Emission or absorption, from which tail beyond 5σ holds the signal.

    Gaussian noise puts ``tail_expected`` sample voxels beyond 5σ on each
    side; a side is significant above that plus five Poisson deviations and
    ten voxels. One significant side three times the other names the type;
    both, or neither, is ambiguous and the user is asked. (Continuum is
    decided later, from the spectra's offsets: see :func:`_continuum_type`.)
    """
    options = list(DATA_TYPES)
    if override:
        return {"value": override, "deduced": False, "confidence": "user", "ambiguous": False,
                "reason": f"Set by the user ({override}).", "options": options}
    floor = tail_expected + 5.0 * math.sqrt(max(tail_expected, 0.0)) + 10.0
    high, low = tail_high > floor, tail_low > floor
    counts = f"{tail_high} sample voxels above +5σ, {tail_low} below −5σ (noise alone: {tail_expected:.1f})"
    if high and tail_high >= 3 * tail_low:
        conf = "high" if tail_high >= 10 * max(tail_low, 1) else "medium"
        return {"value": "emission", "deduced": True, "confidence": conf, "ambiguous": False,
                "reason": f"Signal is positive: {counts}.", "options": options}
    if low and tail_low >= 3 * tail_high:
        conf = "high" if tail_low >= 10 * max(tail_high, 1) else "medium"
        return {"value": "absorption", "deduced": True, "confidence": conf, "ambiguous": False,
                "reason": f"Signal is negative: {counts}. SoFiA will search the inverted cube.",
                "options": options}
    if high or low:
        return {"value": "emission", "deduced": True, "confidence": "low", "ambiguous": True,
                "reason": f"Both tails carry signal ({counts}): emission and absorption, or "
                          "artefacts (sidelobes, heavy-tailed noise). Please choose.",
                "options": options}
    return {"value": "emission", "deduced": True, "confidence": "low", "ambiguous": True,
            "reason": f"No signal beyond 5σ in the sample ({counts}); assuming emission, "
                      "please confirm.", "options": options}


def _continuum_type(cont: float, previous: dict[str, Any]) -> dict[str, Any]:
    conf = "high" if cont > 0.05 else "medium"
    reason = (f"{100 * cont:.1f} % of the sampled spectra sit off zero at both ends of the band by "
              "more than the noise allows: the continuum is not (fully) subtracted, or a line "
              "wider than the band in those pixels.")
    ambiguous = previous["value"] == "absorption"
    if ambiguous:
        reason += " The negative tail also suggests absorption: please choose."
    return {"value": "continuum", "deduced": True, "confidence": conf, "ambiguous": ambiguous,
            "reason": reason, "options": list(DATA_TYPES)}


# ── rules ────────────────────────────────────────────────────────────────────

def _round_half_up(x: float) -> int:
    """floor(x + 0.5): halves round up (Python's round() rounds them to even)."""
    return int(math.floor(x + 0.5))


def spatial_kernels(b_px: float) -> list[int]:
    """Spec rule R2 (§7.1, rev. 2026-11-05b): 0, rnd(k_max/2), k_max with
    k_max = rnd(1.12·b), rounding halves up; the middle kernel is dropped
    below 2 px or when it equals k_max; k_max < 2 gives SoFiA's smallest
    default set, 0 and 3. No extra kernel for survey-sized cubes."""
    k_max = _round_half_up(KMAX_BEAMS * b_px)
    if k_max < 2:
        return [0, 3]
    k_mid = _round_half_up(k_max / 2.0)
    return [0, k_mid, k_max] if 2 <= k_mid < k_max else [0, k_max]


def sky_variation(m: Measurements) -> bool:
    """Whether the noise varies across the field: above the rule's ratio and
    above what the estimator's own noise gives here."""
    return m.r_sky is not None and m.r_sky > max(R_SKY_LOCAL, m.r_sky_noise or 0.0)


def _odd_nearest(x: float) -> int:
    return 2 * int(math.floor(x / 2.0)) + 1


def _fmt_list(values: list[int]) -> str:
    return ", ".join(str(v) for v in values)


def _fmt(x: float) -> str:
    return f"{x:.3g}"


def choose_line_ladder(m: Measurements, override: str | None) -> dict[str, Any]:
    """Extragalactic HI or Galactic/molecular line widths (spec §2.1 notes)."""
    options = list(LINE_LADDERS)
    if override:
        return {"value": override, "deduced": False, "ambiguous": False,
                "reason": f"Set by the user ({override}).", "options": options}
    rf = m.rest_frequency_hz
    if rf and any(abs(rf - f) / f < 1e-4 for f in MOLECULAR_REST_HZ):
        return {"value": "galactic", "deduced": True, "ambiguous": False,
                "reason": "Rest frequency of CO/13CO/C18O: narrow Galactic line widths.", "options": options}
    if m.channel_kms is not None and m.channel_kms < 1.0:
        return {"value": "galactic", "deduced": True, "ambiguous": False,
                "reason": f"Channels of {_fmt(m.channel_kms)} km/s resolve narrow Galactic lines.",
                "options": options}
    if rf and abs(rf - HI_REST_HZ) / HI_REST_HZ < 1e-4 and (m.band_kms or 0) > HI_MIN_BAND_KMS:
        return {"value": "hi", "deduced": True, "ambiguous": False,
                "reason": f"HI rest frequency and a {m.band_kms:.0f} km/s band: extragalactic HI line widths.",
                "options": options}
    why = ("no rest frequency in the header" if not rf else
           "the rest frequency is not HI or CO" if abs(rf - HI_REST_HZ) / HI_REST_HZ >= 1e-4 else
           f"the band is only {(m.band_kms or 0):.0f} km/s")
    return {"value": "hi", "deduced": True, "ambiguous": True,
            "reason": f"The expected line width cannot be read from the cube ({why}); "
                      "assuming extragalactic HI, please confirm.", "options": options}


def _param(out: list[dict[str, Any]], key: str, value: Any, reason: str) -> None:
    if isinstance(value, bool):
        text = "true" if value else "false"
    elif isinstance(value, float):
        text = f"{value:g}" if value != int(value) else f"{value:.1f}"
    else:
        text = str(value)
    default = SOFIA_DEFAULTS.get(key) or None
    out.append({"key": key, "value": text, "reason": reason, "sofia_default": default,
                "changed": default is None or text != default})


def suggest(path: str, data_type: str | None = None, line: str | None = None) -> dict[str, Any]:
    """Measure ``path`` and return the suggested SoFiA-2 parameters (see the
    module docstring); the response the route sends."""
    if data_type is not None and data_type not in DATA_TYPES:
        raise ValueError(f"data_type must be one of {', '.join(DATA_TYPES)}")
    if line is not None and line not in LINE_LADDERS:
        raise ValueError(f"line must be one of {', '.join(LINE_LADDERS)}")
    m, dtype = measure(path, data_type)
    ladder = choose_line_ladder(m, line)
    params, warnings = rules(m, dtype["value"], ladder["value"])
    return {
        "valid": True, "error": "", "sofia_version": SOFIA_VERSION,
        "data_type": dtype, "line": ladder, "parameters": params,
        "params": {p["key"]: p["value"] for p in params},
        "sofia_defaults": dict(SOFIA_DEFAULTS),
        "measurements": m.as_dict(), "warnings": warnings,
    }


def rules(m: Measurements, data_type: str, ladder: str) -> tuple[list[dict[str, Any]], list[dict[str, str]]]:
    """The spec's §2.1 table, applied to the measurements. Pure: testable on
    hand-made :class:`Measurements`."""
    out: list[dict[str, Any]] = []
    warnings: list[dict[str, str]] = []
    b, dv, l_z, n_res = m.b_px, m.channel_kms, m.l_z, m.n_res

    def warn(code: str, message: str) -> None:
        warnings.append({"code": code, "message": message})

    # Input: absorption is searched inverted.
    _param(out, "input.invert", data_type == "absorption",
           "Absorption is negative signal: SoFiA searches the inverted cube." if data_type == "absorption"
           else "Emission is positive signal: the cube is searched as it is.")

    # Memory: a sub-cube is proposed, never applied silently.
    if m.node_memory_bytes and m.sofia_memory_bytes > m.node_memory_bytes:
        from .finders.sofia2 import MEMORY_FACTOR, MEMORY_FLOOR_BYTES  # noqa: PLC0415

        # The share of the cube whose run fits: (memory − floor) / factor bytes
        # of data, as a centred square of the field over the whole band. No
        # floor on it: a region that does not fit is not proposed at all.
        fits_bytes = (m.node_memory_bytes - MEMORY_FLOOR_BYTES) / MEMORY_FACTOR
        side = min(1.0, math.sqrt(max(fits_bytes, 0.0) / m.cube_bytes)) if m.cube_bytes else 0.0
        _, ny, nx = m.shape
        wy, wx = int(ny * side), int(nx * side)
        need = (f"SoFiA-2 needs ~{MEMORY_FACTOR:g}× the cube's size ({m.sofia_memory_bytes / 2**30:.1f} GB); "
                f"this node has {m.node_memory_bytes / 2**30:.1f} GB.")
        if min(wy, wx) >= MIN_REGION_PX:
            y0, x0 = (ny - wy) // 2, (nx - wx) // 2
            region_fraction = (wy * wx) / float(ny * nx)
            _param(out, "input.region", f"{x0}, {x0 + wx - 1}, {y0}, {y0 + wy - 1}, 0, {m.shape[0] - 1}",
                   f"{need} Proposed: the central {wx}×{wy} pixels ({100 * region_fraction:.0f} % of the "
                   "field) over the whole band; move or split it as needed.")
            # What follows is for the sub-cube SoFiA will search.
            if n_res is not None:
                n_res = n_res * region_fraction
            warn("memory", "The cube does not fit SoFiA-2 in this node's memory: a sub-cube (input.region) "
                           "is proposed, and the rules below are for that sub-cube; review it before running.")
        else:
            warn("memory", f"{need} Even a sub-cube of the whole band would be under {MIN_REGION_PX} pixels "
                           "a side: run on a node with more memory, or cut the band first.")

    # Continuum residuals.
    cont = m.cont_fraction or 0.0
    w_max = max(LADDER_KMS[ladder])
    # C2 (§7.2): on a band many line widths wide a per-spectrum constant
    # cannot remove a line, and removes local continuum residuals that cover
    # too few pixels for the cube-wide 1 % test.
    wide_band = m.band_kms is not None and m.band_kms >= CONTSUB_BAND_LINES * w_max
    cube_wide = data_type == "continuum" or cont > CONT_FRACTION
    if cube_wide or wide_band:
        order = 1 if cube_wide and (m.cont_slope_sigma or 0.0) > 3.0 else 0
        if cont > CONT_FRACTION:
            reason = (f"{100 * cont:.1f} % of pixels carry an offset at both ends of the band (continuum, "
                      "or a line wider than the band in those pixels); residual continuum mimics line "
                      "emission and breaks reliability symmetry.")
        elif data_type == "continuum":
            reason = "The user says the continuum is not subtracted."
        else:
            reason = (f"The band ({m.band_kms:.0f} km/s) is {m.band_kms / w_max:.0f}× wider than the widest "
                      f"expected line ({w_max:g} km/s): a per-spectrum constant cannot remove a line, and "
                      "removes local continuum residuals the global test cannot see.")
        _param(out, "contsub.enable", True, reason)
        _param(out, "contsub.order", order,
               f"The offset changes by {_fmt(m.cont_slope_sigma)}σ over the band: a slope is fitted."
               if order == 1 else
               ("The offset is flat over the band: a constant is subtracted." if cube_wide else
                "A constant per spectrum: the least that removes an offset."))
        if cube_wide and (m.cont_strong_fraction or 0.0) > 0.0:
            _param(out, "flag.auto", True,
                   f"{100 * m.cont_strong_fraction:.2f} % of pixels carry residuals above 5σ per channel: "
                   "SoFiA flags the pixels and channels with excess noise.")
            _param(out, "flag.threshold", 5.0, "Flag what is noisier than 5σ.")
    elif m.cont_fraction is not None:
        band = (f"; the band ({m.band_kms:.0f} km/s) is under {CONTSUB_BAND_LINES:g}× the widest expected "
                f"line ({w_max:g} km/s)" if m.band_kms is not None else "")
        _param(out, "contsub.enable", False,
               f"{100 * cont:.2f} % of the sampled spectra sit off zero at both ends of the band "
               f"(the test fires above {100 * CONT_FRACTION:g} %){band}.")
    else:
        _param(out, "contsub.enable", False, "The continuum test could not run (too few spectra).")

    # Noise scaling: a variation counts above the rule's ratio AND above what
    # the estimator's own noise gives on a cube this small.
    sky_varies = sky_variation(m)
    chan_varies = m.r_chan is not None and m.r_chan > max(R_CHAN_SPECTRAL, m.r_chan_noise or 0.0)
    if sky_varies:
        scale_mode = "local"
        _param(out, "scaleNoise.enable", True,
               f"Noise varies by {m.r_sky:.2f}× across the field: a single σ would over-detect in the noisy part.")
        _param(out, "scaleNoise.mode", "local", "The variation is across the field (e.g. primary beam).")
    elif chan_varies:
        scale_mode = "spectral"
        _param(out, "scaleNoise.enable", True,
               f"Noise varies by {m.r_chan:.2f}× across the band: a single σ would over-detect in the noisy channels.")
        _param(out, "scaleNoise.mode", "spectral", "The variation is from channel to channel.")
    else:
        scale_mode = None

        def said(r: float | None, floor: float | None, where: str) -> str | None:
            if r is None:
                return None
            text = f"{r:.2f} across the {where}"
            return text + (f", within the {floor:.2f} noise alone gives" if floor and floor > 1.0 + 1e-3 and r <= floor
                           else "")
        parts = [said(m.r_sky, m.r_sky_noise, "field"), said(m.r_chan, m.r_chan_noise, "band")]
        text = "; ".join(x for x in parts if x) or "not measurable"
        _param(out, "scaleNoise.enable", False, f"Noise is uniform ({text}): no scaling.")

    # Smooth-and-clip kernels.
    if b:
        xy = spatial_kernels(b)
        k_max = xy[-1]
        _param(out, "scfind.kernelsXY", _fmt_list(xy),
               f"Beam is {b:.1f} px. Unsmoothed is the matched filter for point sources; the largest "
               f"kernel, {k_max} px (~1.12 beams), keeps the effective resolution near 1.5 beams "
               f"(√(b² + k²) = {math.hypot(b, k_max) / b:.2f} b here, against 2.24 b for a 2-beam "
               "kernel), so nearby sources and faint bridges between them are not blended into one; "
               "it gives ≥ 92 % of the best smoothing S/N for sources of observed size up to ~2.5 "
               "beams, less for larger ones (83 % at 3, 67 % at 4 beams)."
               if k_max >= 2 else
               f"Beam is {b:.1f} px: under two pixels, so SoFiA's smallest default kernel set.")
    else:
        xy = [0, 3, 6]
        _param(out, "scfind.kernelsXY", "0, 3, 6", "No beam in the header (BMAJ/BMIN): SoFiA's default.")
        warn("no_beam", "The header has no beam (BMAJ/BMIN): spatial kernels, linker sizes and the "
                        "resolution-element count keep SoFiA's defaults.")
    if dv:
        widths = LADDER_KMS[ladder]
        zs = [0]
        dropped = []
        for w in widths:
            k = _odd_nearest(w / dv)
            if k < 3:
                dropped.append(f"the {w:g} km/s step ({k} channel{'s' if k != 1 else ''}) is dropped: "
                               "a boxcar under 3 channels is the unsmoothed cube")
            elif k > m.shape[0] / 4.0:
                dropped.append(f"the {w:g} km/s step ({k} channels) is dropped: more than a quarter "
                               f"of the {m.shape[0]} channels")
            elif k not in zs:
                zs.append(k)
        corr = f", correlated: ρ={m.rho:.2f}" if l_z == 2 and m.rho is not None else ""
        what = ("~15-120 km/s match dwarf to massive-spiral HI line widths" if ladder == "hi"
                else "~2-20 km/s match Galactic and molecular line widths")
        _param(out, "scfind.kernelsZ", _fmt_list(zs),
               f"Channels are {dv:.2f} km/s (rest frame{corr}); boxcars of {what}"
               + ("; " + "; ".join(dropped) + "." if dropped else "."))
        window_z = max(zs)
    else:
        # SoFiA's default set, less what the band cannot hold: SoFiA refuses
        # a kernel as long as the cube, and the rule above drops one over a
        # quarter of the channels.
        zs = [0] + [k for k in (3, 7, 15) if k <= m.shape[0] / 4.0]
        dropped = [k for k in (3, 7, 15) if k not in zs]
        _param(out, "scfind.kernelsZ", _fmt_list(zs),
               f"Channel width in km/s unknown ({m.channel_note}): SoFiA's default"
               + (f", without the {', '.join(map(str, dropped))}-channel boxcar"
                  f"{'s' if len(dropped) > 1 else ''} (more than a quarter of the "
                  f"{m.shape[0]} channels)." if dropped else "."))
        warn("no_channel_width", f"Channel width in km/s unknown ({m.channel_note}): spectral kernels "
                                 "and the minimum spectral size keep SoFiA's defaults.")
        window_z = max(zs)
    if scale_mode == "local":
        wxy = max(25, int(math.ceil(5 * b)) if b else 25)
        wxy += 1 - wxy % 2
        wz = max(15, 2 * window_z)
        wz += 1 - wz % 2
        _param(out, "scaleNoise.windowXY", wxy,
               "Window large compared with a source, so the source does not inflate its own noise.")
        _param(out, "scaleNoise.windowZ", wz,
               "Twice the widest spectral kernel, so a line does not inflate its own noise.")

    # Reliability, from the number of resolution elements.
    reasons_off = []
    if data_type == "absorption":
        reasons_off.append("absorption (too few independent negatives to model)")
    if cont > CONT_FRACTION or data_type == "continuum":
        reasons_off.append("continuum residuals break the symmetry of positive and negative noise")
    if sky_varies and scale_mode != "local":
        reasons_off.append("the noise varies across the field without noise scaling")
    caveat = False
    if n_res is None:
        rel = False
        rel_reason = "Resolution elements cannot be counted without a beam: reliability stays off."
    elif reasons_off:
        rel = False
        rel_reason = "Reliability off: " + "; ".join(reasons_off) + "."
    elif n_res >= N_RES_RELIABILITY:
        rel = True
        rel_reason = (f"{n_res:.3g} resolution elements: expect enough negative detections to model "
                      "the noise.")
    elif n_res >= N_RES_RELIABILITY_CAVEAT:
        rel, caveat = True, True
        rel_reason = (f"{n_res:.3g} resolution elements: probably enough negative detections, but "
                      "SoFiA may warn that there are few; check the count after the run.")
    else:
        rel = False
        rel_reason = (f"{n_res:.3g} resolution elements: too few negative detections to model the "
                      "noise (SoFiA warns below its minimum); a 5σ clip keeps chance detections rare.")
    _param(out, "reliability.enable", rel, rel_reason)
    if rel:
        _param(out, "reliability.threshold", 0.9, "SoFiA default; lowering trades purity for completeness.")
        _param(out, "linker.positivity", False, "Reliability needs the negative detections.")
    if caveat:
        warn("reliability_caveat", "Few resolution elements for the reliability filter: read the "
                                   "negative-detection count in the run summary.")

    # Threshold.
    if rel:
        k = 3.5 if (n_res or 0) >= N_RES_SURVEY else 4.0
        t_reason = f"{k}σ: with reliability filtering a lower clip is safe (Serra+2012)."
    else:
        k = 5.0
        t_reason = (f"5σ: without reliability filtering it keeps chance detections rare in "
                    f"{n_res:.3g} resolution elements." if n_res else
                    "5σ: without reliability filtering it keeps chance detections rare.")
    if m.zero_blanked:
        t_reason += (f" Caution: {100 * m.f_zero:.1f} % exact zeros; SoFiA counts them as data and "
                     "under-estimates σ, so the clip is lower than it reads.")
        warn("zero_blanks", f"{100 * m.f_zero:.1f} % of the finite voxels are exactly zero: convert "
                            "blanks to NaN before running, or the threshold is not what it says.")
    _param(out, "scfind.threshold", k, t_reason)
    _param(out, "scfind.statistic", "mad", "Robust to the sources in the cube.")
    _param(out, "scfind.fluxRange", "negative",
           "Noise from the negative side, which bright emission does not reach (faint emission over "
           "most of the field still pulls it low).")
    _param(out, "scfind.replacement", 2.0, "SoFiA default.")

    # Linker.
    if b:
        _param(out, "linker.radiusXY", max(1, int(round(b / 2.5))),
               "Merge fragments closer than ~half a beam.")
        _param(out, "linker.minSizeXY", min(10, max(3, int(round(b)))), "A real source is at least one beam across.")
    else:
        _param(out, "linker.radiusXY", 1, "No beam: SoFiA's default.")
        _param(out, "linker.minSizeXY", 5, "No beam: SoFiA's default.")
    if dv:
        rz = 2 if (l_z == 2 or dv < 5.0) else 1
        why = [f"correlated (ρ={m.rho:.2f})" if l_z == 2 and m.rho is not None else
               "correlated" if l_z == 2 else None,
               f"narrow ({dv:.2f} < 5 km/s)" if dv < 5.0 else None]
        fired = " and ".join(x for x in why if x)
        _param(out, "linker.radiusZ", rz,
               f"Channels are {fired}: merge fragments one channel apart." if rz == 2
               else f"Channels are independent and wide ({dv:.2f} km/s): adjacent ones only.")
        narrow = NARROWEST_LINE_KMS[ladder]
        mz = max(int(round(narrow / dv)), 2 * l_z + 1)
        reason = ((f"Narrowest HI lines are ~20 km/s (thermal FWHM at 8000 K ≈ 19 km/s)" if ladder == "hi"
                   else f"Narrowest Galactic lines are taken as ~{narrow:g} km/s (not validated)")
                  + (f" and longer than the channel correlation ({l_z} channels)." if l_z == 2 else "."))
    else:
        _param(out, "linker.radiusZ", 2 if l_z == 2 else 1,
               (f"Channels are correlated (ρ={m.rho:.2f}): merge across one." if m.rho is not None
                else "Channels are correlated: merge across one.") if l_z == 2 else "SoFiA's default.")
        mz, reason = max(5, 2 * l_z + 1), "Channel width unknown: SoFiA's default."
    half = max(1, m.shape[0] // 2)
    if mz > half:
        mz, reason = half, reason + f" Capped at half the cube's {m.shape[0]} channels."
    _param(out, "linker.minSizeZ", mz, reason)

    # Filter.
    _param(out, "filter.minSNR", 3.0, "Drops detections whose integrated S/N < 3.")
    _param(out, "filter.discardNegative", True, "Negative detections are noise (or, inverted, emission).")
    # No dilation: v1 suggests masks at the clip (the "flux" goal was left
    # out). A user who wants dilated masks sets dilation.* by hand.

    if m.f_nan > 0.5:
        warn("blank_footprint", f"{100 * m.f_nan:.0f} % of the cube is blank (NaN): the counts above use "
                                "the finite voxels only.")
    if m.channel_offsets > 0.1:
        warn("channel_offsets", f"{100 * m.channel_offsets:.0f} % of the sampled channels sit off zero as a "
                                "whole (a baseline or bandpass problem, or emission over the whole field): "
                                "SoFiA measures the noise about zero, so check the baseline before running.")
    return out, warnings
