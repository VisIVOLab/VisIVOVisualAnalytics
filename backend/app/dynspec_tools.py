"""Dynamic-spectrum / beamformed analysis workers.

Three tools, all operating on a 2-D FITS file produced by
:mod:`hdf5_dynspec` (or any FITS classified by
:meth:`ScientificFitsDataset.is_dynamic_spectrum`) with axis convention
NAXIS1 = TIME, NAXIS2 = FREQUENCY:

* :func:`worker_dynspec_dedisperse` — incoherent dedispersion at a user
  supplied dispersion measure (DM). For each frequency channel the time
  series is shifted by an integer number of samples corresponding to the
  cold-plasma dispersion delay. Result is a new 2-D FITS that drops into
  the workspace and can be opened as a normal dynspec dataset.

* :func:`worker_dynspec_pulse_profile` — folds the dedispersed,
  frequency-averaged time series at a user-supplied pulse period and
  returns a 1-D phase profile.

* :func:`worker_dynspec_rfi_mask` — sigma-clipped flagging of pixels
  that deviate from each channel's robust median by more than ``n_sigma``
  median absolute deviations. The mask is written as a 1-bit FITS file
  with the same WCS as the source.

No coherent dedispersion is offered — that requires voltage / complex
data that the dynspec viewer does not expose.
"""
from __future__ import annotations

import logging
from pathlib import Path
from typing import Any

import numpy as np
from astropy.io import fits

logger = logging.getLogger("visivo.dynspec")


# ── Common helpers ──────────────────────────────────────────────────────────

def _open_dynspec(src_path: str) -> tuple[np.ndarray, fits.Header]:
    """Read a 2-D dynspec FITS into a (freq, time) float32 array."""
    with fits.open(src_path, memmap=True, mode="readonly") as hdul:
        data = np.asarray(hdul[0].data, dtype=np.float32)
        header = hdul[0].header.copy()
    if data.ndim > 2:
        # Squeeze any degenerate leading axes (Stokes/Pol/etc.) so the result
        # is purely (freq, time). Last two dims are kept as (NAXIS2, NAXIS1).
        while data.ndim > 2:
            data = data[0]
    if data.ndim != 2:
        raise ValueError(
            "Dynspec FITS must reduce to a 2-D array (freq, time), got "
            f"shape {data.shape}.")
    # FITS stores rows as NAXIS2 (frequency) and columns as NAXIS1 (time).
    # numpy reads it as (NAXIS2, NAXIS1) = (freq, time) — already correct.
    return data, header


def _axis_metadata(header: fits.Header) -> dict[str, float]:
    """Return the per-axis sampling/origin used for dispersion math."""
    def _f(key: str, default: float) -> float:
        try:
            return float(header.get(key, default))
        except (TypeError, ValueError):
            return default
    return {
        "n_time":   int(header.get("NAXIS1", 0)),
        "n_freq":   int(header.get("NAXIS2", 0)),
        "t_ref":    _f("CRVAL1", 0.0),
        "t_pix":    _f("CRPIX1", 1.0),
        "t_step":   _f("CDELT1", 1.0),  # seconds per sample
        "f_ref":    _f("CRVAL2", 0.0),
        "f_pix":    _f("CRPIX2", 1.0),
        "f_step":   _f("CDELT2", 1.0),  # Hz per channel
    }


def _channel_frequencies_mhz(meta: dict[str, float]) -> np.ndarray:
    """Return the central frequency (MHz) of every channel."""
    idx = np.arange(meta["n_freq"], dtype=np.float64)
    freq_hz = meta["f_ref"] + (idx + 1.0 - meta["f_pix"]) * meta["f_step"]
    return freq_hz / 1e6  # → MHz


def _finite_range(arr: np.ndarray) -> tuple[float, float]:
    finite = np.isfinite(arr)
    if not finite.any():
        return 0.0, 0.0
    return float(arr[finite].min()), float(arr[finite].max())


# ── Incoherent dedispersion ─────────────────────────────────────────────────

# Pulsar dispersion constant in MHz²·pc⁻¹·cm³·s — value adopted by Lorimer
# & Kramer (2005); matches PRESTO and sigproc within the rounding of the
# physical constants involved.
_DISP_CONST_MHZ2_S = 4.148808e3  # such that delay_s = K * DM / freq_MHz²


def worker_dynspec_dedisperse(
    src_path: str,
    output_path: str,
    dm_pc_cm3: float,
    f_ref_mhz: float = 0.0,
) -> dict[str, Any]:
    """Incoherently dedisperse a 2-D dynamic spectrum at ``dm_pc_cm3``.

    Each frequency channel is rolled by ``-round(Δt/dt)`` samples along
    the time axis so that all channels align in time at the reference
    frequency ``f_ref_mhz`` (defaults to the highest channel — common
    convention so pulses arrive simultaneously at the top of the band).

    Edges that roll past the end of the time axis are wrapped via
    ``np.roll`` — they will appear at the opposite edge. Document the
    caveat: if you want padded edges instead of wrap-around, post-process
    by NaN-masking the wrapped region.

    Returns metadata (shape, range, frequency span, ref freq used).
    """
    data, header = _open_dynspec(src_path)
    meta = _axis_metadata(header)
    if meta["n_freq"] <= 1 or meta["n_time"] <= 1:
        raise ValueError("Dynspec is too small to dedisperse.")
    freqs_mhz = _channel_frequencies_mhz(meta)
    if np.any(freqs_mhz <= 0.0):
        raise ValueError(
            "Some channel frequencies are <= 0 MHz; check CDELT2/CRVAL2 in "
            "the source header.")

    if f_ref_mhz <= 0.0:
        f_ref_mhz = float(freqs_mhz.max())

    dt_s = abs(meta["t_step"])
    if dt_s <= 0.0:
        raise ValueError("Time sampling CDELT1 is zero or invalid.")

    delays_s = (_DISP_CONST_MHZ2_S * float(dm_pc_cm3)
                * (1.0 / freqs_mhz ** 2 - 1.0 / f_ref_mhz ** 2))
    shifts = np.round(delays_s / dt_s).astype(np.int64)

    dedispersed = np.empty_like(data)
    for i in range(meta["n_freq"]):
        dedispersed[i, :] = np.roll(data[i, :], -int(shifts[i]))

    new_header = header.copy()
    new_header.add_history(
        f"VisIVO incoherent dedispersion @ DM={dm_pc_cm3:.4f} pc cm^-3, "
        f"reference freq {f_ref_mhz:.3f} MHz.")
    new_header["DM"] = (float(dm_pc_cm3), "Dispersion measure applied")
    new_header["DMREFMHZ"] = (float(f_ref_mhz), "Reference freq used for DM correction")

    fits.PrimaryHDU(data=dedispersed, header=new_header).writeto(
            output_path, overwrite=True)
    range_min, range_max = _finite_range(dedispersed)
    logger.info(
        "[dedisperse] DM=%s refFreq=%s shape=%s shifts_min=%s shifts_max=%s "
        "range=(%s, %s) out=%s",
        dm_pc_cm3, f_ref_mhz, data.shape,
        int(shifts.min()) if shifts.size else 0,
        int(shifts.max()) if shifts.size else 0,
        range_min, range_max, output_path,
    )
    return {
        "width":  int(meta["n_time"]),
        "height": int(meta["n_freq"]),
        "range_min": float(range_min),
        "range_max": float(range_max),
        "dm_pc_cm3": float(dm_pc_cm3),
        "ref_freq_mhz": float(f_ref_mhz),
        "max_shift_samples": int(abs(shifts).max()) if shifts.size else 0,
        "output_path": output_path,
    }


# ── Pulse profile folding ───────────────────────────────────────────────────

def worker_dynspec_pulse_profile(
    src_path: str,
    period_s: float,
    n_bins: int = 64,
    dm_pc_cm3: float = 0.0,
    freq_start_mhz: float = 0.0,
    freq_end_mhz: float = 0.0,
) -> dict[str, Any]:
    """Fold the frequency-averaged time series at ``period_s`` into
    ``n_bins`` phase bins.

    If ``dm_pc_cm3 > 0`` the data is first dedispersed (in memory, no
    sidecar produced) before being summed across frequency.

    If ``freq_start_mhz < freq_end_mhz`` only channels falling within that
    range contribute to the sum — handy for excluding RFI-dominated
    sub-bands.

    Returns a dict with ``phase`` (cycle 0…1) and ``profile`` (mean
    intensity per phase bin) plus useful statistics (peak, FWHM estimate,
    SNR via MAD).
    """
    if period_s <= 0.0:
        raise ValueError("period_s must be > 0.")
    if n_bins < 4:
        raise ValueError("n_bins must be ≥ 4.")
    data, header = _open_dynspec(src_path)
    meta = _axis_metadata(header)
    freqs_mhz = _channel_frequencies_mhz(meta)
    dt_s = abs(meta["t_step"])
    if dt_s <= 0.0:
        raise ValueError("Time sampling CDELT1 is zero or invalid.")

    # Optional in-memory dedispersion
    if dm_pc_cm3 != 0.0:
        f_ref_mhz = float(freqs_mhz.max())
        delays_s = (_DISP_CONST_MHZ2_S * float(dm_pc_cm3)
                    * (1.0 / freqs_mhz ** 2 - 1.0 / f_ref_mhz ** 2))
        shifts = np.round(delays_s / dt_s).astype(np.int64)
        dedispersed = np.empty_like(data)
        for i in range(meta["n_freq"]):
            dedispersed[i, :] = np.roll(data[i, :], -int(shifts[i]))
        data = dedispersed

    # Frequency band selection
    if freq_start_mhz > 0.0 and freq_end_mhz > 0.0 and freq_end_mhz > freq_start_mhz:
        mask = (freqs_mhz >= freq_start_mhz) & (freqs_mhz <= freq_end_mhz)
        if mask.sum() == 0:
            raise ValueError("Selected frequency band contains no channels.")
        data = data[mask, :]

    # Mean across frequency, ignoring NaNs
    time_series = np.nanmean(data, axis=0)
    if not np.any(np.isfinite(time_series)):
        raise ValueError("No finite samples after frequency averaging.")

    # Fold: assign each sample to a phase bin
    n_samples = time_series.size
    times_s = meta["t_ref"] + (np.arange(n_samples, dtype=np.float64) + 1.0
                                - meta["t_pix"]) * dt_s
    phase = np.mod(times_s, period_s) / period_s
    bin_idx = np.clip((phase * n_bins).astype(np.int64), 0, n_bins - 1)

    profile = np.zeros(n_bins, dtype=np.float64)
    counts = np.zeros(n_bins, dtype=np.int64)
    for i in range(n_samples):
        v = time_series[i]
        if np.isfinite(v):
            b = bin_idx[i]
            profile[b] += v
            counts[b] += 1
    valid = counts > 0
    profile[valid] /= counts[valid]
    profile[~valid] = np.nan

    # SNR via MAD on the off-peak region
    finite_profile = profile[np.isfinite(profile)]
    if finite_profile.size > 4:
        med = np.median(finite_profile)
        mad = np.median(np.abs(finite_profile - med))
        sigma_mad = 1.4826 * mad if mad > 0 else 0.0
        peak = float(np.nanmax(profile))
        snr_peak = (peak - med) / sigma_mad if sigma_mad > 0 else 0.0
    else:
        sigma_mad = 0.0
        snr_peak = 0.0
        peak = float(np.nanmax(profile)) if finite_profile.size else 0.0

    phase_centres = (np.arange(n_bins, dtype=np.float64) + 0.5) / n_bins
    logger.info(
        "[pulse_profile] period=%s s nbins=%s dm=%s peak=%s snr=%s",
        period_s, n_bins, dm_pc_cm3, peak, snr_peak,
    )
    return {
        "phase":       phase_centres.tolist(),
        "profile":     profile.tolist(),
        "counts":      counts.tolist(),
        "peak":        float(peak),
        "sigma_mad":   float(sigma_mad),
        "snr_peak":    float(snr_peak),
        "period_s":    float(period_s),
        "dm_pc_cm3":   float(dm_pc_cm3),
        "n_bins":      int(n_bins),
        "n_samples":   int(n_samples),
    }


# ── RFI mask generation ─────────────────────────────────────────────────────

def worker_dynspec_rfi_mask(
    src_path: str,
    output_path: str,
    n_sigma: float = 5.0,
    axis: str = "channel",
) -> dict[str, Any]:
    """Generate a binary RFI mask by σ-clipping each channel (or each
    time sample, depending on ``axis``) against its robust median.

    ``n_sigma`` of MAD: pixels whose deviation from the row/column median
    exceeds ``n_sigma × 1.4826 × MAD`` are flagged. Median and MAD are
    computed ignoring NaNs.

    The output FITS is a uint8 array (0 = good, 1 = flagged) sharing the
    source's WCS, so it can be loaded as a transparent overlay on the
    dynspec viewer or used by other tools.
    """
    if n_sigma <= 0:
        raise ValueError("n_sigma must be > 0.")
    if axis not in ("channel", "time"):
        raise ValueError("axis must be 'channel' or 'time'.")
    data, header = _open_dynspec(src_path)

    # Convert to float64 for the percentile computation; keep NaNs as bad.
    arr = np.asarray(data, dtype=np.float64)
    mask = np.zeros_like(arr, dtype=np.uint8)
    finite = np.isfinite(arr)
    # Sentinel for the np.median calls (treated below).
    work = np.where(finite, arr, np.nan)

    axis_along = 1 if axis == "channel" else 0  # σ-clip across time per channel
    medians = np.nanmedian(work, axis=axis_along, keepdims=True)
    abs_dev = np.abs(work - medians)
    mads = np.nanmedian(abs_dev, axis=axis_along, keepdims=True)
    sigmas = 1.4826 * mads
    threshold = n_sigma * sigmas
    # Avoid division by 0 — zero σ means a constant row/column (likely flat
    # spectrum or already-flagged data), skip the comparison there.
    valid_thr = (sigmas > 0)
    flagged = np.zeros_like(arr, dtype=bool)
    np.greater(abs_dev, threshold, out=flagged, where=valid_thr)
    flagged |= ~finite  # NaNs are always bad
    mask = flagged.astype(np.uint8)

    new_header = header.copy()
    # Keep the spatial / spectral WCS so the mask overlays correctly. Switch
    # BUNIT to a boolean-style label and add an explanatory HISTORY card.
    new_header["BUNIT"] = ("flag", "0 = good, 1 = flagged")
    new_header["BITPIX"] = 8
    new_header.add_history(
        f"VisIVO RFI sigma-clip mask along {axis} axis, n_sigma={n_sigma}.")
    new_header["NSIGMA"] = (float(n_sigma), "Sigma threshold used for flagging")
    new_header["AXIS"] = (axis, "Axis used for median/MAD computation")

    fits.PrimaryHDU(data=mask, header=new_header).writeto(
            output_path, overwrite=True)
    flagged_fraction = float(mask.mean()) if mask.size else 0.0
    logger.info(
        "[rfi_mask] axis=%s n_sigma=%s flagged_fraction=%.4f out=%s",
        axis, n_sigma, flagged_fraction, output_path,
    )
    return {
        "width":  int(mask.shape[1]),
        "height": int(mask.shape[0]),
        "flagged_fraction": flagged_fraction,
        "n_sigma": float(n_sigma),
        "axis": axis,
        "output_path": output_path,
    }
