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


def worker_stokes_plane(
    path: str,
    plane: str,
    channel: int | None,
) -> dict[str, Any]:
    """Extract a single Stokes plane (I, Q, U or V) from a FITS dataset.

    The dataset must have a Stokes axis (CTYPE == 'STOKES').  If a spectral
    (channel) axis is also present, ``channel`` selects the spectral plane;
    None → channel 0.

    Returns a float32 2-D image encoded in base64, plus shape / range metadata.
    """
    plane = plane.upper()
    valid_planes = {"I", "Q", "U", "V"}
    if plane not in valid_planes:
        raise ValueError(f"Invalid Stokes plane '{plane}'. Must be one of {valid_planes}.")

    dataset = ScientificFitsDataset(path)
    stokes_map = dataset.stokes_planes
    if not stokes_map:
        raise ValueError("This dataset has no Stokes axis (CTYPE == STOKES not found).")
    if plane not in stokes_map:
        available = sorted(stokes_map)
        raise ValueError(
            f"Stokes plane '{plane}' not present in this dataset. "
            f"Available: {available}."
        )

    hdul, raw = dataset.open_memmap()
    try:
        data = np.asarray(raw)
    finally:
        hdul.close()

    stokes_numpy_ax = dataset.stokes_numpy_axis()
    if stokes_numpy_ax is None:
        raise RuntimeError("Stokes axis disappeared after detection — internal error.")

    stokes_idx = stokes_map[plane]
    # Select Stokes plane along its numpy axis
    slices_stokes: list[Any] = [slice(None)] * data.ndim
    slices_stokes[stokes_numpy_ax] = stokes_idx
    data = data[tuple(slices_stokes)]  # now ndim - 1

    # If a spectral axis remains, select channel
    if data.ndim == 3:
        ch = 0 if channel is None else int(channel)
        ch = max(0, min(ch, data.shape[0] - 1))
        data = data[ch]
    elif data.ndim > 3:
        # Squeeze any remaining degenerate axes
        data = np.squeeze(data)
        if data.ndim == 3:
            ch = 0 if channel is None else int(channel)
            ch = max(0, min(ch, data.shape[0] - 1))
            data = data[ch]

    image = np.asarray(data, dtype=np.float32)
    range_min, range_max = _finite_range(image)
    return {
        "plane": plane,
        "channel": channel,
        "width": int(image.shape[1]) if image.ndim == 2 else image.size,
        "height": int(image.shape[0]) if image.ndim == 2 else 1,
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
) -> dict[str, Any]:
    """Compute bias-corrected polarised intensity (PI) and position angle (PA).

    PI_corr = sqrt(max(Q² + U² − σ_QU², 0))
    PA      = 0.5 × atan2(U, Q)  [degrees, range −90..+90]

    σ_QU² is estimated from line-free channels (if provided) as the mean
    of the per-pixel variance of Q and U over those channels; if not provided
    or if the dataset is 2-D, σ_QU² = 0 (no bias correction).

    Both output maps are float32, returned as base64 payloads.
    """
    dataset = ScientificFitsDataset(path)
    stokes_map = dataset.stokes_planes
    for plane in ("Q", "U"):
        if plane not in stokes_map:
            raise ValueError(
                f"Stokes plane '{plane}' required for PI/PA computation but not found. "
                f"Available: {sorted(stokes_map)}."
            )

    hdul, raw = dataset.open_memmap()
    try:
        data = np.asarray(raw, dtype=np.float64)
    finally:
        hdul.close()

    stokes_numpy_ax = dataset.stokes_numpy_axis()
    assert stokes_numpy_ax is not None

    def _extract_plane(arr: np.ndarray, stokes_idx: int, ch: int | None) -> np.ndarray:
        slc: list[Any] = [slice(None)] * arr.ndim
        slc[stokes_numpy_ax] = stokes_idx
        plane_arr = arr[tuple(slc)]
        if plane_arr.ndim == 3:
            c = 0 if ch is None else int(ch)
            c = max(0, min(c, plane_arr.shape[0] - 1))
            return plane_arr[c]
        return np.squeeze(plane_arr) if plane_arr.ndim != 2 else plane_arr

    ch = 0 if channel is None else int(channel)
    Q = _extract_plane(data, stokes_map["Q"], ch)
    U = _extract_plane(data, stokes_map["U"], ch)

    # Bias correction: σ²_QU from line-free channels
    sigma_qu2 = 0.0
    if line_free_channels and len(line_free_channels) >= 2:
        slc_q: list[Any] = [slice(None)] * data.ndim
        slc_q[stokes_numpy_ax] = stokes_map["Q"]
        q_cube = data[tuple(slc_q)]  # (nchans, ny, nx) or (ny, nx)
        slc_u: list[Any] = [slice(None)] * data.ndim
        slc_u[stokes_numpy_ax] = stokes_map["U"]
        u_cube = data[tuple(slc_u)]
        if q_cube.ndim == 3:
            lfc = np.array([i for i in line_free_channels if 0 <= i < q_cube.shape[0]])
            if len(lfc) >= 2:
                q_lf = q_cube[lfc]
                u_lf = u_cube[lfc]
                sigma_qu2 = float(
                    np.nanmean(np.nanvar(q_lf, axis=0))
                    + np.nanmean(np.nanvar(u_lf, axis=0))
                ) / 2.0

    pi2 = Q ** 2 + U ** 2 - sigma_qu2
    pi_corr = np.sqrt(np.where(pi2 > 0.0, pi2, 0.0)).astype(np.float32)
    pa_deg  = (0.5 * np.degrees(np.arctan2(U, Q))).astype(np.float32)

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


def worker_rm_synthesis(
    path: str,
    phi_min: float,
    phi_max: float,
    d_phi: float,
) -> dict[str, Any]:
    """Rotation Measure synthesis (Brentjens & de Bruyn 2005).

    Computes the Faraday Dispersion Function (FDF):
      F(φ) = (1/N) Σ_i  P(λ²_i) × exp(−2i φ λ²_i)

    where P = Q + iU and λ² is derived from the frequency axis via WCS.

    The dataset must have both Q and U Stokes planes and a frequency spectral
    axis.  φ is sampled uniformly in [phi_min, phi_max) with step d_phi.

    Returns:
      - |F(φ)| as a 3-D float32 array (n_phi × ny × nx), base64-encoded
      - RMSF (RM spread function) as a 1-D list of floats
      - phi_grid: the φ sample points (rad/m²)
    """
    C_LIGHT = 2.99792458e8  # m/s

    dataset = ScientificFitsDataset(path)
    stokes_map = dataset.stokes_planes
    for plane in ("Q", "U"):
        if plane not in stokes_map:
            raise ValueError(
                f"Stokes plane '{plane}' required for RM synthesis but not found. "
                f"Available: {sorted(stokes_map)}."
            )

    hdul, raw = dataset.open_memmap()
    try:
        data = np.asarray(raw, dtype=np.float64)
    finally:
        hdul.close()

    stokes_numpy_ax = dataset.stokes_numpy_axis()
    assert stokes_numpy_ax is not None

    def _get_stokes_cube(arr: np.ndarray, stokes_idx: int) -> np.ndarray:
        slc: list[Any] = [slice(None)] * arr.ndim
        slc[stokes_numpy_ax] = stokes_idx
        cube = arr[tuple(slc)]
        return np.squeeze(cube)   # should be (nchans, ny, nx) or (ny, nx)

    Q_cube = _get_stokes_cube(data, stokes_map["Q"])
    U_cube = _get_stokes_cube(data, stokes_map["U"])

    if Q_cube.ndim == 2:
        Q_cube = Q_cube[np.newaxis, :, :]
        U_cube = U_cube[np.newaxis, :, :]

    if Q_cube.ndim != 3:
        raise ValueError(f"Expected 3-D (freq, ny, nx) Q/U cube, got shape {Q_cube.shape}.")

    n_freq, ny, nx = Q_cube.shape

    # Derive λ² from frequency axis via WCS spectral axis
    spectral = dataset.spectral_axis_metadata(0, n_freq - 1)
    freq_hz = np.asarray(spectral["coordinates"], dtype=np.float64)
    # Convert to wavelength squared: λ² = (c/f)²
    lambda2 = (C_LIGHT / freq_hz) ** 2   # (n_freq,)

    # φ grid
    phi_grid = np.arange(phi_min, phi_max, d_phi, dtype=np.float64)
    n_phi = len(phi_grid)
    if n_phi == 0:
        raise ValueError("Empty φ grid — check phi_min, phi_max, d_phi.")

    # Complex polarisation cube
    P_cube = (Q_cube + 1j * U_cube).astype(np.complex128)   # (n_freq, ny, nx)

    # FDF: F[phi, y, x] = (1/N) Σ_f P[f,y,x] * exp(-2i*phi*lambda2[f])
    # Shape: (n_phi, n_freq) phase kernel → (n_phi, ny, nx) FDF
    kernel = np.exp(-2j * np.outer(phi_grid, lambda2))  # (n_phi, n_freq)
    # Reshape for broadcasting: (n_phi, n_freq, 1, 1) × (n_freq, ny, nx)
    F_complex = np.einsum("pf,fyx->pyx", kernel, P_cube) / float(n_freq)
    FDF = np.abs(F_complex).astype(np.float32)           # (n_phi, ny, nx)

    # RMSF: theoretical RM spread function as a 1-D array in the same φ space
    # R(φ) = (1/N) Σ_f exp(-2i*φ*λ²_f)
    rmsf_complex = np.einsum("pf->p", kernel) / float(n_freq)
    rmsf = np.abs(rmsf_complex).tolist()

    range_min, range_max = _finite_range(FDF)
    return {
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
        "range_min": float(range_min),
        "range_max": float(range_max),
        "data_base64": _b64f32(FDF),
    }


# ── C-01: Angular power spectrum ──────────────────────────────────────────────
