from __future__ import annotations

import base64
import concurrent.futures as _cf  # noqa: F401
import logging  # noqa: F401
import os as _os  # noqa: F401
import shutil  # noqa: F401
import subprocess  # noqa: F401
import warnings  # noqa: F401
import zlib  # noqa: F401
from pathlib import Path  # noqa: F401
from typing import Any  # noqa: F401

import numpy as np

from ..fits_dataset import ScientificFitsDataset  # noqa: F401
from ._common import (  # noqa: F401
    logger,
    _qt_zlib_compress,
    _b64f32,
    _b64i32,
    _b64f32_compressed,
    _b64i32_compressed,
    _finite_range,
    _integrated_spacing,
)


# ── Stokes sources: one file with a STOKES axis, or one file per plane ───────
#
# Two layouts are in the wild and both have to work.
#
#   * One FITS carrying a STOKES axis of length ≥ 2. Everything below reads the
#     planes out of that single array. This is what the original workers
#     assumed, and it is still the fast path.
#
#   * One FITS PER PLANE, each with a degenerate STOKES axis of length 1. This
#     is how MeerKAT (MGCLS), ASKAP/POSSUM and anything produced by Obit
#     MFImage ship full-Stokes data: ``FIELD_I.fits``, ``FIELD_Q.fits``, … The
#     caller names the companion files and says which role each one plays.
#
# Three things measured on real MGCLS cubes shape the rules here:
#
#   1. A companion's own STOKES header may be WRONG. In the MGCLS release the
#     V cube carries CRVAL4 = 1, which decodes as Stokes I. When the file has a
#     single-plane Stokes axis the caller's role is therefore authoritative and
#     the header disagreement is only logged — refusing would reject a real,
#     published dataset over a typo in a keyword we do not need.
#
#   2. The I cube's frequency grid is NOT the Q/U grid (CRVAL3 1.3426 GHz vs
#     1.2838 GHz for the same field). λ² must come from the file that supplied
#     Q, never from whichever file the user happened to open. Getting this
#     wrong silently biases every rotation measure.
#
#   3. These cubes are 7500 × 7500 × 16 per plane. Materialising one as float64
#     costs 7 GB, two 14 GB, and RM synthesis needs both in full. Every read
#     below slices the memmap FIRST — channel, then region — and only then
#     converts, so the cost is set by what was asked for and not by the file.


def _stokes_axis_length(dataset: ScientificFitsDataset) -> int | None:
    """Length of the STOKES axis, or None when the dataset has no such axis."""
    for axis in range(1, dataset.naxis + 1):
        ctype = str(dataset.header.get(f"CTYPE{axis}", "")).strip().upper()
        if ctype == "STOKES":
            return int(dataset.axis_sizes[axis - 1])
    return None


def _select_stokes_index(dataset: ScientificFitsDataset, role: str, *, authoritative: bool) -> int | None:
    """0-based index of `role` along the Stokes axis, or None when there is no such axis.

    `authoritative` says the caller knows this file IS that role (it named it as
    the companion for it). That only overrides the header when the Stokes axis
    holds a single plane, so there is no ambiguity about which one is meant.
    """
    planes = dataset.stokes_planes
    if role in planes:
        return planes[role]
    length = _stokes_axis_length(dataset)
    if length is None:
        return None                      # no Stokes axis at all — whole file is the plane
    if authoritative and length == 1:
        logger.warning(
            "[stokes] %s: header declares %s but the file was supplied as Stokes %s; "
            "trusting the caller (single-plane Stokes axis).",
            dataset.path, sorted(planes) or "no known plane", role)
        return 0
    raise ValueError(
        f"Stokes plane '{role}' not present in {dataset.path}. "
        f"Available: {sorted(planes)}.")


def _read_stokes_cube(
    dataset: ScientificFitsDataset,
    role: str,
    *,
    authoritative: bool,
    channel: int | None = None,
    region: tuple[int, int, int, int] | None = None,
    planes: tuple[int, int] | None = None,
) -> np.ndarray:
    """Pixels of one Stokes plane as (n_freq, ny, nx), or (1, ny, nx) for an image.

    `channel` keeps a single spectral channel; `region` is (x0, y0, w, h) in
    pixels. Both are applied to the memmap before the data is read, which is
    what keeps a 7500² × 16 cube affordable.
    """
    hdul, raw = dataset.open_memmap()
    try:
        arr = raw
        stokes_idx = _select_stokes_index(dataset, role, authoritative=authoritative)
        if stokes_idx is not None:
            stokes_numpy_ax = dataset.stokes_numpy_axis()
            if stokes_numpy_ax is None:      # pragma: no cover — detection just said it exists
                raise RuntimeError("Stokes axis disappeared after detection, internal error.")
            slc: list[Any] = [slice(None)] * arr.ndim
            slc[stokes_numpy_ax] = stokes_idx
            arr = arr[tuple(slc)]

        # Drop degenerate axes, but only LEADING ones. Squeezing every axis of
        # length 1 also eats a spatial axis: a (16, 1, 100) cube — sixteen
        # channels of a one-row image — became (16, 100), which then reads as a
        # 16-row image and combines frequency with sky. The last two axes are y
        # and x by construction and are never dropped.
        while arr.ndim > 3 and arr.shape[0] == 1:
            arr = arr[0]
        if arr.ndim == 3 and arr.shape[0] == 1 and arr.shape[1] == 1:
            arr = arr[0]          # (1, 1, nx) → (1, nx): a one-row image
        if arr.ndim == 2:
            arr = arr[np.newaxis, :, :]
        if arr.ndim != 3:
            raise ValueError(
                f"Expected a 2-D image or a 3-D (freq, y, x) cube for Stokes {role} "
                f"in {dataset.path}, got shape {tuple(arr.shape)}.")

        if planes is not None:
            start, count = planes
            arr = arr[start:start + count]
            if arr.shape[0] != count:
                raise ValueError(
                    f"{dataset.path} has {arr.shape[0]} of the {count} planes expected at "
                    f"offset {start}.")
        if channel is not None:
            ch = max(0, min(int(channel), arr.shape[0] - 1))
            arr = arr[ch:ch + 1]
        if region is not None:
            x0, y0, w, h = region
            arr = arr[:, y0:y0 + h, x0:x0 + w]
            if arr.shape[1] == 0 or arr.shape[2] == 0:
                raise ValueError(f"Region {region} does not overlap the image.")
        # Read now, while the file is still open: everything above is a view.
        return np.array(arr, dtype=np.float32)
    finally:
        hdul.close()


def _resolve_stokes_sources(
    path: str,
    roles: tuple[str, ...],
    companions: dict[str, str] | None,
) -> dict[str, tuple[ScientificFitsDataset, bool]]:
    """Pick, for each role, the dataset to read it from and whether the role is authoritative.

    A role present in the primary file's own Stokes axis is taken from there;
    otherwise the companion named for it is used. Raises with a message naming
    what is missing, because "no polarisation" is otherwise indistinguishable
    from "you forgot to load the companions".
    """
    primary = ScientificFitsDataset(path)
    companions = companions or {}
    sources: dict[str, tuple[ScientificFitsDataset, bool]] = {}
    missing: list[str] = []
    for role in roles:
        if role in primary.stokes_planes:
            sources[role] = (primary, False)
        elif companions.get(role):
            sources[role] = (ScientificFitsDataset(companions[role]), True)
        else:
            missing.append(role)
    if missing:
        raise ValueError(
            f"Stokes {', '.join(missing)} required but not available: the dataset carries "
            f"{sorted(primary.stokes_planes) or 'no Stokes axis'} and no companion file was "
            f"supplied for {', '.join(missing)}. Load the Stokes companions first.")
    return sources


def _mfimage_subbands(dataset: ScientificFitsDataset) -> tuple[int, np.ndarray] | None:
    """(first sub-band plane, its frequencies in Hz) for an Obit MFImage cube.

    MFImage does not write a linear spectral axis. It writes NTERM fitted
    planes first — total flux, spectral index, … — then NSPEC sub-band planes,
    and gives each sub-band's frequency in its own FREQnnnn keyword. The WCS
    keywords on axis 3 describe none of that.
    
    Measured on the real MGCLS release (G000.0+2.5_Q.fits.gz): NAXIS3 = 16 with
    NTERM = 2 and NSPEC = 14, sub-bands running 908 → 1656 MHz, while CRVAL3 /
    CDELT3 describe 1284 → 2147 MHz. Taking the WCS grid would put λ² in the
    wrong place for every channel AND feed two fitted planes, which are not
    sub-bands at all, into the transform — silently, with a plausible-looking
    RM map coming out the other end.

    Returns None when the header does not describe this layout, and the caller
    falls back to the WCS axis.
    """
    header = dataset.header
    try:
        n_spec = int(header.get("NSPEC", 0))
        n_term = int(header.get("NTERM", 0))
    except (TypeError, ValueError):
        return None
    if n_spec <= 0:
        return None

    freqs: list[float] = []
    for i in range(1, n_spec + 1):
        value = header.get(f"FREQ{i:04d}")
        if value is None:
            return None            # incomplete: not this layout, or unusable
        freqs.append(float(value))

    # The planes have to fit: NTERM + NSPEC is the whole spectral axis for a
    # well-formed MFImage cube. Anything else means the file is not what these
    # keywords claim, and guessing an offset would be worse than the WCS.
    spectral_axis = dataset.cube_numpy_spectral_fits_axis()
    n_planes = int(dataset.axis_sizes[spectral_axis - 1])
    offset = max(0, n_term)
    if offset + n_spec != n_planes:
        logger.warning(
            "[rmsynth] %s: NTERM=%s + NSPEC=%s does not match the %s planes on the "
            "spectral axis; using the WCS axis instead.",
            dataset.path, n_term, n_spec, n_planes)
        return None

    freq = np.asarray(freqs, dtype=np.float64)
    if not np.all(np.isfinite(freq)) or np.any(freq <= 0.0):
        return None
    return offset, freq


def _require_same_sky_grid(a: ScientificFitsDataset, b: ScientificFitsDataset) -> None:
    """Refuse two files that are the same SHAPE but not the same place on the sky.

    Equal pixel dimensions prove nothing: two cutouts of the same survey are
    routinely 7500 × 7500 and centred hundreds of pixels apart. Combining them
    per-pixel would give a polarisation map of two different patches of sky,
    with nothing in the numbers to show for it.

    The comparison is in PIXELS, not in header values. Sibling files written by
    the same pipeline can carry CDELT to different precision — -4.166667E-04
    against -4.1667E-04 — and demanding equal floats would reject a perfectly
    co-registered pair over how a number was printed. What matters is whether
    the two grids land on the same sky to well within a pixel.
    """
    if a.path == b.path:
        return
    tolerance_pixels = 0.01
    for axis in (1, 2):
        ctype_a = str(a.header.get(f"CTYPE{axis}", "")).strip().upper()
        ctype_b = str(b.header.get(f"CTYPE{axis}", "")).strip().upper()
        if ctype_a != ctype_b:
            raise ValueError(
                f"The Stokes files use different projections on axis {axis} "
                f"({ctype_a or 'none'} vs {ctype_b or 'none'}); they cannot be combined "
                f"pixel by pixel.")
        try:
            cdelt_a = float(a.header.get(f"CDELT{axis}", 0.0))
            cdelt_b = float(b.header.get(f"CDELT{axis}", 0.0))
            crval_a = float(a.header.get(f"CRVAL{axis}", 0.0))
            crval_b = float(b.header.get(f"CRVAL{axis}", 0.0))
            crpix_a = float(a.header.get(f"CRPIX{axis}", 1.0))
            crpix_b = float(b.header.get(f"CRPIX{axis}", 1.0))
        except (TypeError, ValueError) as exc:
            raise ValueError(
                f"The Stokes files have unreadable WCS on axis {axis}: {exc}") from exc

        scale = abs(cdelt_a) or abs(cdelt_b)
        if scale == 0.0:
            raise ValueError(f"The Stokes files have no pixel scale on axis {axis}.")
        if abs(cdelt_a - cdelt_b) > 1e-6 * scale:
            raise ValueError(
                f"The Stokes files have different pixel scales on axis {axis} "
                f"(CDELT {cdelt_a:g} vs {cdelt_b:g}); they are not on the same grid.")
        # Offset of b's reference point from a's, measured in pixels of a.
        offset_pixels = abs((crval_a - crval_b) / scale) + abs(crpix_a - crpix_b)
        if offset_pixels > tolerance_pixels:
            raise ValueError(
                f"The Stokes files are not on the same sky grid: their reference points "
                f"differ by {offset_pixels:.3g} pixels on axis {axis} "
                f"({a.path} vs {b.path}). Combining them per pixel would mix different "
                f"positions on the sky.")


def _frequencies_hz(dataset: ScientificFitsDataset, n_freq: int) -> np.ndarray:
    """Spectral axis of `dataset` in Hz, or a ValueError explaining why it cannot be.

    The unit is taken from CUNIT when it is there and inferred from the
    magnitude when it is not: MGCLS cubes carry CTYPE3 = 'SPECLNMF' with no
    CUNIT at all, and silently treating GHz as Hz would scale λ² by 10^18.
    """
    spectral = dataset.spectral_axis_metadata(0, n_freq - 1)
    coords = np.asarray(spectral["coordinates"], dtype=np.float64)
    axis_type = str(spectral.get("axis_type", "")).strip().upper()
    unit = str(spectral.get("axis_unit", "")).strip().lower()

    if axis_type.startswith(("VELO", "VRAD", "VOPT", "FELO", "ZOPT")):
        raise ValueError(
            f"RM synthesis needs a frequency axis; {dataset.path} has a velocity axis "
            f"({axis_type}). Convert the cube to frequency first.")

    scale = {"hz": 1.0, "khz": 1e3, "mhz": 1e6, "ghz": 1e9}.get(unit)
    if scale is None:
        if unit:
            raise ValueError(
                f"Unsupported spectral unit '{unit}' on {dataset.path}; expected Hz, kHz, MHz or GHz.")
        # No CUNIT. Infer from the magnitude, but only for an axis that CLAIMS
        # to be a frequency: an axis of some other kind that happens to hold
        # values in the radio range would otherwise be read as frequencies.
        if not axis_type.startswith(("FREQ", "SPECLNMF")):
            raise ValueError(
                f"{dataset.path} has no CUNIT on its spectral axis and its CTYPE "
                f"('{axis_type or 'none'}') does not name a frequency, so the unit cannot "
                f"be established.")
        # Radio frequencies land in Hz between 10 MHz and 1 THz; a value far
        # outside that says the header is in some other unit, and guessing
        # would be worse than stopping.
        median = float(np.nanmedian(np.abs(coords)))
        scale = 1.0 if 1e7 <= median <= 1e12 else 0.0
        if scale == 0.0:
            raise ValueError(
                f"{dataset.path} has no CUNIT on its spectral axis and its values "
                f"(median {median:g}) are not plausible frequencies in Hz.")
    freq = coords * scale
    if not np.all(np.isfinite(freq)) or np.any(freq <= 0.0):
        raise ValueError(f"{dataset.path} has non-positive or non-finite frequencies.")
    return freq


def worker_stokes_plane(
    path: str,
    plane: str,
    channel: int | None,
    companions: dict[str, str] | None = None,
) -> dict[str, Any]:
    """Extract a single Stokes plane (I, Q, U or V) as a 2-D float32 image.

    The plane comes from `path` when that file has a Stokes axis carrying it,
    and otherwise from ``companions[plane]`` — the one-file-per-plane layout.
    If a spectral axis is present, `channel` selects the plane; None → 0.

    Returns a base64 float32 image plus shape / range metadata.
    """
    plane = plane.upper()
    valid_planes = {"I", "Q", "U", "V"}
    if plane not in valid_planes:
        raise ValueError(f"Invalid Stokes plane '{plane}'. Must be one of {valid_planes}.")

    dataset, authoritative = _resolve_stokes_sources(path, (plane,), companions)[plane]
    cube = _read_stokes_cube(dataset, plane, authoritative=authoritative, channel=channel)
    image = cube[0]

    range_min, range_max = _finite_range(image)
    return {
        "plane": plane,
        "channel": channel,
        "width": int(image.shape[1]),
        "height": int(image.shape[0]),
        "scalar_type": "float32",
        "range_min": range_min,
        "range_max": range_max,
        "data_base64": _b64f32(image),
    }


# ── P-02: Polarised intensity and position-angle maps ────────────────────────


def worker_polarisation_pi_pa(
    path: str,
    channel: int | None,
    line_free_channels: list[int] | None,
    companions: dict[str, str] | None = None,
) -> dict[str, Any]:
    """Compute bias-corrected polarised intensity (PI) and position angle (PA).

    PI_corr = sqrt(max(Q² + U² − σ_QU², 0))
    PA      = 0.5 × atan2(U, Q)  [degrees, range −90..+90]

    σ_QU² is estimated from line-free channels (if provided) as the mean of the
    per-pixel variance of Q and U over those channels; if not provided, or if
    the data is 2-D, σ_QU² = 0 (no bias correction).

    Q and U come from `path` when it carries them, else from `companions`.
    Both output maps are float32, returned as base64 payloads.
    """
    sources = _resolve_stokes_sources(path, ("Q", "U"), companions)
    ch = 0 if channel is None else int(channel)
    Q = _read_stokes_cube(sources["Q"][0], "Q", authoritative=sources["Q"][1], channel=ch)[0]
    U = _read_stokes_cube(sources["U"][0], "U", authoritative=sources["U"][1], channel=ch)[0]
    if Q.shape != U.shape:
        raise ValueError(
            f"Stokes Q and U have different shapes ({Q.shape} vs {U.shape}); they must be "
            f"on the same grid.")
    _require_same_sky_grid(sources["Q"][0], sources["U"][0])

    # Bias correction: σ²_QU from line-free channels. Read separately (and only
    # when asked for), so the common case costs one channel per plane.
    sigma_qu2 = 0.0
    if line_free_channels and len(line_free_channels) >= 2:
        q_cube = _read_stokes_cube(sources["Q"][0], "Q", authoritative=sources["Q"][1])
        u_cube = _read_stokes_cube(sources["U"][0], "U", authoritative=sources["U"][1])
        if q_cube.shape[0] > 1:
            lfc = np.array([i for i in line_free_channels if 0 <= i < q_cube.shape[0]])
            if len(lfc) >= 2:
                sigma_qu2 = float(
                    np.nanmean(np.nanvar(q_cube[lfc], axis=0))
                    + np.nanmean(np.nanvar(u_cube[lfc], axis=0))
                ) / 2.0

    Qd = Q.astype(np.float64)
    Ud = U.astype(np.float64)
    pi2 = Qd ** 2 + Ud ** 2 - sigma_qu2
    pi_corr = np.sqrt(np.where(pi2 > 0.0, pi2, 0.0)).astype(np.float32)
    pa_deg  = (0.5 * np.degrees(np.arctan2(Ud, Qd))).astype(np.float32)

    pi_range = _finite_range(pi_corr)
    pa_range = _finite_range(pa_deg)
    ny, nx = pi_corr.shape
    return {
        "width": nx,
        "height": ny,
        "channel": channel,
        "sigma_qu2": sigma_qu2,
        "scalar_type": "float32",
        "pi_range_min": pi_range[0],
        "pi_range_max": pi_range[1],
        "pa_range_min": pa_range[0],
        "pa_range_max": pa_range[1],
        "pi_base64": _b64f32(pi_corr),
        "pa_base64": _b64f32(pa_deg),
    }


# ── P-03: Rotation Measure synthesis ─────────────────────────────────────────

# An FDF cube is n_phi × ny × nx float32. At MGCLS size (7500² pixels) even a
# hundred φ samples is 22 GB, so the full cube cannot be the only answer the
# endpoint knows how to give. `reduce="peak"` returns the two maps that are
# actually looked at — peak |F| and the φ at which it peaks — and a region
# keeps the cube variant usable on a cut-out.
_FDF_CUBE_LIMIT_BYTES = 512 * 1024 * 1024
#: Working-memory budget for one φ block of the complex intermediate.
_FDF_BLOCK_BYTES = 256 * 1024 * 1024


def worker_rm_synthesis(
    path: str,
    phi_min: float,
    phi_max: float,
    d_phi: float,
    companions: dict[str, str] | None = None,
    reduce: str = "cube",
    region: tuple[int, int, int, int] | None = None,
) -> dict[str, Any]:
    """Rotation Measure synthesis (Brentjens & de Bruyn 2005).

    Computes the Faraday Dispersion Function (FDF):
      F(φ) = (1/N) Σ_i  P(λ²_i) × exp(−2i φ λ²_i)

    where P = Q + iU and λ² comes from the frequency axis of the file that
    supplied Q — not from `path`, whose grid may differ (it does in MGCLS).

    Q and U come from `path` when it carries them, else from `companions`.
    `region` is (x0, y0, w, h) in pixels. `reduce` is "cube" for the whole FDF
    or "peak" for the peak-|F| and peak-φ maps.

    Returns |F(φ)| (or the two peak maps) as base64 float32, the φ grid and the
    RMSF.
    """
    C_LIGHT = 2.99792458e8  # m/s

    if reduce not in ("cube", "peak"):
        raise ValueError(f"reduce must be 'cube' or 'peak', got '{reduce}'.")

    sources = _resolve_stokes_sources(path, ("Q", "U"), companions)
    q_dataset, q_auth = sources["Q"]
    u_dataset, u_auth = sources["U"]
    _require_same_sky_grid(q_dataset, u_dataset)

    # Which planes carry the sub-bands, and at what frequencies. An Obit
    # MFImage cube answers both from its own keywords; anything else from the
    # WCS axis, over every plane.
    q_layout = _mfimage_subbands(q_dataset)
    planes: tuple[int, int] | None = None
    if q_layout is not None:
        offset, freq_hz = q_layout
        planes = (offset, len(freq_hz))
        if u_dataset.path != q_dataset.path:
            u_layout = _mfimage_subbands(u_dataset)
            if u_layout is None:
                raise ValueError(
                    f"{q_dataset.path} is an MFImage cube (NSPEC sub-bands) but "
                    f"{u_dataset.path} is not; their planes do not correspond.")
            if u_layout[0] != offset or not np.allclose(u_layout[1], freq_hz, rtol=1e-6):
                raise ValueError(
                    "Stokes Q and U declare different sub-band frequencies; RM synthesis "
                    "would combine them at frequencies they were not measured at.")

    Q_cube = _read_stokes_cube(q_dataset, "Q", authoritative=q_auth, region=region,
                               planes=planes)
    U_cube = _read_stokes_cube(u_dataset, "U", authoritative=u_auth, region=region,
                               planes=planes)
    if Q_cube.shape != U_cube.shape:
        raise ValueError(
            f"Stokes Q and U have different shapes ({Q_cube.shape} vs {U_cube.shape}); "
            f"they must be on the same grid.")

    n_freq, ny, nx = Q_cube.shape

    if q_layout is None:
        # λ² from the Q file's own spectral axis — never the primary's, whose
        # grid may differ. When U came from a different file its grid has to
        # agree, or the two are being combined at frequencies they were not
        # measured at.
        freq_hz = _frequencies_hz(q_dataset, n_freq)
        if u_dataset.path != q_dataset.path:
            u_freq = _frequencies_hz(u_dataset, n_freq)
            if not np.allclose(freq_hz, u_freq, rtol=1e-6):
                raise ValueError(
                    "Stokes Q and U are on different frequency grids "
                    f"({freq_hz[0]:.6g}…{freq_hz[-1]:.6g} Hz vs "
                    f"{u_freq[0]:.6g}…{u_freq[-1]:.6g} Hz); RM synthesis would combine "
                    "them at frequencies they were not measured at.")
    if len(freq_hz) != n_freq:                      # pragma: no cover — guarded above
        raise ValueError(
            f"{n_freq} planes were read but {len(freq_hz)} frequencies are known.")
    lambda2 = (C_LIGHT / freq_hz) ** 2   # (n_freq,)

    # φ grid
    phi_grid = np.arange(phi_min, phi_max, d_phi, dtype=np.float64)
    n_phi = len(phi_grid)
    if n_phi == 0:
        raise ValueError("Empty φ grid - check phi_min, phi_max, d_phi.")

    if reduce == "cube":
        needed = n_phi * ny * nx * 4
        if needed > _FDF_CUBE_LIMIT_BYTES:
            raise ValueError(
                f"The requested FDF cube is {needed / (1 << 20):.0f} MiB "
                f"({n_phi} φ × {ny} × {nx}), over the {_FDF_CUBE_LIMIT_BYTES >> 20} MiB limit. "
                f"Ask for reduce='peak', a smaller region, or a coarser φ grid.")

    # Complex polarisation cube
    P_cube = (Q_cube.astype(np.float64) + 1j * U_cube.astype(np.float64))

    # FDF: F[phi, y, x] = (1/N) Σ_f P[f,y,x] * exp(-2i*phi*lambda2[f])
    #
    # Evaluated in blocks of φ, and in peak mode never assembled: holding the
    # whole thing would make "peak" cost exactly what the cube costs — the
    # complex intermediate alone is 16 bytes a sample — and the size guard
    # above, which peak mode deliberately skips, would protect nothing. The
    # running maximum is all the peak maps need.
    block = max(1, int(_FDF_BLOCK_BYTES // max(1, ny * nx * 16)))

    def kernel_for(lo: int, hi: int) -> np.ndarray:
        return np.exp(-2j * np.outer(phi_grid[lo:hi], lambda2))   # (hi-lo, n_freq)

    FDF: np.ndarray | None = None
    peak_value: np.ndarray | None = None
    peak_index: np.ndarray | None = None
    if reduce == "cube":
        FDF = np.empty((n_phi, ny, nx), dtype=np.float32)
    for lo in range(0, n_phi, block):
        hi = min(lo + block, n_phi)
        chunk = np.abs(np.einsum("pf,fyx->pyx", kernel_for(lo, hi), P_cube)
                       / float(n_freq)).astype(np.float32)
        if FDF is not None:
            FDF[lo:hi] = chunk
            continue
        local_index = np.argmax(chunk, axis=0)
        local_value = np.take_along_axis(chunk, local_index[np.newaxis], axis=0)[0]
        if peak_value is None:
            peak_value, peak_index = local_value, local_index + lo
        else:
            better = local_value > peak_value
            peak_value = np.where(better, local_value, peak_value)
            peak_index = np.where(better, local_index + lo, peak_index)

    # RMSF: theoretical RM spread function as a 1-D array in the same φ space
    # R(φ) = (1/N) Σ_f exp(-2i*φ*λ²_f)
    rmsf = np.abs(np.concatenate([np.einsum("pf->p", kernel_for(lo, min(lo + block, n_phi)))
                                  for lo in range(0, n_phi, block)])
                  / float(n_freq)).tolist()

    result: dict[str, Any] = {
        "n_phi": n_phi,
        "n_freq": n_freq,
        "width": nx,
        "height": ny,
        "scalar_type": "float32",
        "phi_min": float(phi_min),
        "phi_max": float(phi_max),
        "d_phi": float(d_phi),
        "phi_grid": phi_grid.tolist(),
        "rmsf": rmsf,
        "reduce": reduce,
    }
    if reduce == "peak":
        assert peak_value is not None and peak_index is not None
        peak_phi = phi_grid[peak_index].astype(np.float32)
        # A pixel whose F(φ) is NaN everywhere — one NaN channel is enough to
        # poison it — has no peak. argmax still answers 0, which would publish
        # φ_min as that pixel's rotation measure: a finite, plausible, entirely
        # invented number. NaN says "no measurement", which is what it is.
        undefined = ~np.isfinite(peak_value)
        peak_phi[undefined] = np.nan
        peak_value = np.where(undefined, np.nan, peak_value)
        result["range_min"], result["range_max"] = _finite_range(peak_value)
        result["phi_peak_min"], result["phi_peak_max"] = _finite_range(peak_phi)
        result["data_base64"] = _b64f32(np.asarray(peak_value, dtype=np.float32))
        result["phi_peak_base64"] = _b64f32(peak_phi)
        return result
    assert FDF is not None

    range_min, range_max = _finite_range(FDF)
    result["range_min"] = float(range_min)
    result["range_max"] = float(range_max)
    result["data_base64"] = _b64f32(FDF)
    return result


# ── C-01: Angular power spectrum ──────────────────────────────────────────────
