"""PSRFITS (pulsar FITS) reader -> dynamic-spectrum sidecar.

PSRFITS is a multi-HDU binary-table FITS layout used by most modern
pulsar back-ends (PSRCHIVE/DSPSR, PRESTO, SKA PST, ...). The payload lives
in a ``SUBINT`` binary-table HDU with a ``DATA`` column. The layout of a
row is fixed by the definition, not by the lengths of its axes, so it is
read from the headers:

* ``OBS_MODE`` in the primary header says which layout it is: ``PSR`` or
  ``CAL`` are **folded** (each row is one sub-integration holding a
  profile of ``NBIN`` phase bins per channel and polarisation, stored in
  polarisation, channel, bin order -> numpy ``(NPOL, NCHAN, NBIN)``);
  ``SEARCH`` rows hold ``NSBLK`` consecutive time samples of ``TBIN``
  seconds each, stored in sample, polarisation, channel order -> numpy
  ``(NSBLK, NPOL, NCHAN)``. Guessing from lengths fails as soon as two of
  them are equal (a search row of 2 samples, 1 pol, 2 channels looks just
  like a folded row of 2 pols, 1 channel, 2 bins), so the row is
  flattened and reshaped from ``NBIN``/``NCHAN``/``NPOL``/``NSBLK``.
* Stored values are scaled integers. The physical value is
  ``DATA * DAT_SCL + DAT_OFFS`` (folded) or
  ``(DATA - ZERO_OFF) * DAT_SCL + DAT_OFFS`` (search), with ``DAT_SCL``
  and ``DAT_OFFS`` given per row as ``NCHAN * NPOL`` values, channel
  varying fastest. Using ``DATA`` raw puts every channel on its own
  arbitrary scale, which is not a spectrum.
* Search data with ``NBITS`` 1, 2 or 4 are packed several samples per
  byte, earlier samples in the higher-order bits, and are unpacked here.
  Other widths below 8 are refused.
* ``DAT_WTS`` holds one weight per channel and row. A weight of zero is
  how PSRFITS flags a channel, so those samples become NaN, which is how
  the rest of the dynspec pipeline treats flagged data (the RFI mask flags
  NaN, the pulse profile and averages skip it). Non-zero weights are not
  applied: they weight sums across channels, they do not rescale a
  channel's values.

The sidecar is Stokes I: ``AA + BB`` when ``POL_TYPE`` is a pair of
feed products (``AABB``, ``AABBCRCI``), otherwise the first polarisation
(``IQUV``, ``INTEN``, ``AA+BB``). Folded data are averaged across phase so
the sidecar is time vs frequency, one column per sub-integration of
``TSUBINT`` seconds; search data give ``NSUB * NSBLK`` columns of ``TBIN``
seconds. The output FITS is 2-D with ``CTYPE1='TIME'`` /
``CTYPE2='FREQ'`` so the dynspec classifier downstream promotes it to
``kind='dynspec'``.

The conversion is cached as a sidecar next to the source file
(``foo.fits`` -> ``foo.fits.dynspec.fits``) and reused on subsequent opens
as long as the source mtime hasn't moved forward.
"""
from __future__ import annotations

import logging
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from astropy.io import fits

logger = logging.getLogger("visivo.psrfits")

_FOLD_MODES = ("PSR", "CAL")
_SEARCH_MODES = ("SEARCH", "SRCH")
_PACKED_NBITS = (1, 2, 4)
_SEARCH_NBITS = (1, 2, 4, 8, 16, 32)
# A row may be placed after a gap in OFFS_SUB (dropped sub-integrations
# become NaN columns) as long as the image stays within this many times
# its contiguous length; beyond that the rows are concatenated instead.
_MAX_GAP_EXPANSION = 4
# POL_TYPE values whose first two products are the two feeds' powers, so
# Stokes I is their sum rather than the first product alone.
_FEED_PAIR_POL_TYPES = ("AABB", "AABBCRCI", "XXYY", "RRLL")


def is_psrfits(path: str | Path) -> bool:
    """Quick check: open the FITS file headers only and look for a SUBINT
    binary-table HDU. Errors are swallowed (the caller will fall through
    to the standard FITS classifier)."""
    try:
        with fits.open(str(path), memmap=True, mode="readonly") as hdul:
            for hdu in hdul:
                if (hdu.name or "").upper() == "SUBINT":
                    return True
            return False
    except Exception as exc:
        logger.debug("[psrfits] probe failed for %s: %s", path, exc)
        return False


def _sidecar_path(src: Path) -> Path:
    return src.with_suffix(src.suffix + ".dynspec.fits")


@dataclass(frozen=True)
class _Layout:
    """How one SUBINT row is laid out, as the headers declare it."""
    folded: bool
    nbin: int
    nchan: int
    npol: int
    nsblk: int
    nbits: int
    signed: bool = False

    @property
    def values_per_row(self) -> int:
        if self.folded:
            return self.npol * self.nchan * self.nbin
        return self.nsblk * self.npol * self.nchan


def _header_int(header: fits.Header, key: str, default: int | None) -> int:
    """An integer keyword; ``default`` when absent, ``ValueError`` when
    required (``default is None``) and missing or not an integer."""
    value = header.get(key)
    if value is None or (isinstance(value, str) and value.strip() in ("", "*")):
        if default is None:
            raise ValueError(f"PSRFITS SUBINT header has no {key}.")
        return default
    try:
        return int(value)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"PSRFITS {key} is not an integer: {value!r}.") from exc


def _header_float(header: fits.Header, key: str) -> float | None:
    """A float keyword, or None when absent or ``'*'`` (PSRFITS's "not
    applicable"), so callers choose their own fallback."""
    value = header.get(key)
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def _read_layout(primary_hdr: fits.Header, subint_hdr: fits.Header) -> _Layout:
    """Fold or search, and the row dimensions, from OBS_MODE and SUBINT.

    Without ``OBS_MODE`` the mode follows from ``NBIN`` / ``NSBLK`` (only
    one of them can exceed 1); a file where both do is refused rather
    than read in a guessed order.
    """
    nbin = _header_int(subint_hdr, "NBIN", 1)
    nchan = _header_int(subint_hdr, "NCHAN", None)
    npol = _header_int(subint_hdr, "NPOL", None)
    nsblk = _header_int(subint_hdr, "NSBLK", 1)
    nbits = _header_int(subint_hdr, "NBITS", 8)
    if min(nbin, nchan, npol, nsblk) < 1:
        raise ValueError(
            f"PSRFITS dimensions must be positive: NBIN={nbin}, NCHAN={nchan}, "
            f"NPOL={npol}, NSBLK={nsblk}.")

    mode = str(primary_hdr.get("OBS_MODE", "")).strip().upper()
    if mode in _FOLD_MODES:
        folded = True
    elif mode in _SEARCH_MODES:
        folded = False
        if nbin != 1:
            raise ValueError(
                f"PSRFITS OBS_MODE=SEARCH but NBIN={nbin}; search data "
                "have one bin per sample.")
    elif nbin > 1 and nsblk > 1:
        raise ValueError(
            f"PSRFITS without a recognised OBS_MODE ({mode or 'missing'}) has "
            f"both NBIN={nbin} and NSBLK={nsblk} > 1; cannot tell folded "
            "from search data.")
    else:
        folded = nsblk == 1
    signed = _header_int(subint_hdr, "SIGNINT", 0) == 1
    if folded:
        # NSBLK is 1 by definition for folded data, and NBITS describes
        # search samples only (fold writers often leave it at 1): the
        # stored type of DATA is what counts.
        nsblk = 1
    elif nbits not in _SEARCH_NBITS:
        raise ValueError(
            f"PSRFITS NBITS={nbits} is not supported; search data must have "
            "NBITS 1, 2, 4 (packed), 8, 16 or 32.")
    return _Layout(folded, nbin, nchan, npol, nsblk, nbits, signed)


def _to_signed(values: np.ndarray, nbits: int) -> np.ndarray:
    """Two's-complement reading of unsigned ``nbits``-wide integers
    (``SIGNINT = 1``): 255 -> -1 at 8 bits, 15 -> -1 at 4 bits."""
    values = values.astype(np.int16)
    half = 1 << (nbits - 1)
    return np.where(values >= half, values - (1 << nbits), values)


def _unpack_bits(raw: np.ndarray, nbits: int, count: int) -> np.ndarray:
    """Unpack ``count`` unsigned ``nbits``-wide samples from bytes, the
    earliest sample in the highest-order bits (the PSRFITS convention)."""
    raw = np.ascontiguousarray(raw, dtype=np.uint8).ravel()
    shifts = np.arange(8 - nbits, -1, -nbits, dtype=np.uint8)  # high bits first
    mask = np.uint8((1 << nbits) - 1)
    values = (raw[:, None] >> shifts[None, :]) & mask
    values = values.reshape(-1)
    if values.size < count:
        raise ValueError(
            f"PSRFITS row has {raw.size} bytes, fewer than the "
            f"{count} samples x {nbits} bits it declares.")
    return values[:count]


def _row_values(raw_row: np.ndarray, layout: _Layout) -> np.ndarray:
    """One DATA row as float64 in its declared shape: ``(NPOL, NCHAN,
    NBIN)`` folded, ``(NSBLK, NPOL, NCHAN)`` search. The flat order of a
    row is the FITS storage order whatever ``TDIM`` astropy applied."""
    count = layout.values_per_row
    if not layout.folded and layout.nbits < 8:
        expected_bytes = count * layout.nbits // 8
        if np.asarray(raw_row).size != expected_bytes:
            raise ValueError(
                f"PSRFITS DATA row has {np.asarray(raw_row).size} bytes; "
                f"NSBLK={layout.nsblk} x NPOL={layout.npol} x "
                f"NCHAN={layout.nchan} at NBITS={layout.nbits} needs "
                f"{expected_bytes}.")
        unpacked = _unpack_bits(raw_row, layout.nbits, count)
        if layout.signed:
            unpacked = _to_signed(unpacked, layout.nbits)
        flat = unpacked.astype(np.float64)
    else:
        raw = np.asarray(raw_row)
        if not layout.folded and layout.signed and raw.dtype == np.uint8:
            raw = _to_signed(raw, 8)
        flat = raw.astype(np.float64).ravel()
        if flat.size != count:
            what = (f"NPOL={layout.npol} x NCHAN={layout.nchan} x NBIN={layout.nbin}"
                    if layout.folded else
                    f"NSBLK={layout.nsblk} x NPOL={layout.npol} x NCHAN={layout.nchan}")
            raise ValueError(
                f"PSRFITS DATA row has {flat.size} values; {what} "
                f"needs {count}.")
    if layout.folded:
        return flat.reshape(layout.npol, layout.nchan, layout.nbin)
    return flat.reshape(layout.nsblk, layout.npol, layout.nchan)


def _per_pol_chan(table, column: str, row: int, layout: _Layout,
                  default: float) -> np.ndarray:
    """``DAT_SCL`` / ``DAT_OFFS`` of one row as ``(NPOL, NCHAN)``.

    The definition gives ``NCHAN * NPOL`` values with the channel varying
    fastest; a column of only ``NCHAN`` values (some writers) applies to
    every polarisation. Absent -> ``default`` everywhere.
    """
    if column not in table.columns.names:
        return np.full((layout.npol, layout.nchan), default, dtype=np.float64)
    values = np.asarray(table.data[column][row], dtype=np.float64).ravel()
    if values.size == layout.npol * layout.nchan:
        return values.reshape(layout.npol, layout.nchan)
    if values.size == layout.nchan:
        return np.broadcast_to(values, (layout.npol, layout.nchan))
    raise ValueError(
        f"PSRFITS {column} has {values.size} values per row; expected "
        f"NCHAN*NPOL={layout.nchan * layout.npol}.")


def _channel_weights(table, row: int, nchan: int) -> np.ndarray:
    if "DAT_WTS" not in table.columns.names:
        return np.ones(nchan, dtype=np.float64)
    weights = np.asarray(table.data["DAT_WTS"][row], dtype=np.float64).ravel()
    if weights.size != nchan:
        raise ValueError(
            f"PSRFITS DAT_WTS has {weights.size} values per row; expected "
            f"NCHAN={nchan}.")
    return weights


def _stokes_i(values: np.ndarray, pol_axis: int, pol_type: str) -> np.ndarray:
    """Total intensity from the polarisation products along ``pol_axis``."""
    if pol_type in _FEED_PAIR_POL_TYPES and values.shape[pol_axis] >= 2:
        return (np.take(values, 0, axis=pol_axis)
                + np.take(values, 1, axis=pol_axis))
    return np.take(values, 0, axis=pol_axis)


def _column_floats(table, column: str) -> np.ndarray | None:
    if column not in table.columns.names:
        return None
    try:
        return np.asarray(table.data[column], dtype=np.float64).ravel()
    except (TypeError, ValueError):
        return None


@dataclass(frozen=True)
class _TimeGrid:
    """Where each SUBINT row lands on the sidecar's linear time axis."""
    start_s: float          # centre of the first column, s from obs start
    step_s: float           # signed seconds per column (CDELT1)
    row_columns: np.ndarray  # first column of each row
    n_time: int
    note: str               # how the grid was derived, for HISTORY


def _time_grid(table, subint_hdr: fits.Header, layout: _Layout,
               n_rows: int) -> _TimeGrid:
    """The sidecar time axis: one column per search sample (``TBIN``) or
    per folded sub-integration (``TSUBINT``).

    Rows are placed by ``OFFS_SUB`` (each row's centre) so a dropped
    sub-integration leaves NaN columns instead of shifting every later
    timestamp. When ``OFFS_SUB`` is absent, not on a grid of whole
    columns, overlapping, or would blow the image up by more than
    ``_MAX_GAP_EXPANSION``, the rows are concatenated and the note says
    the times assume contiguous rows.
    """
    per_row = 1 if layout.folded else layout.nsblk
    if layout.folded:
        tsubint = _column_floats(table, "TSUBINT")
        if tsubint is not None and tsubint.size and tsubint[0] > 0:
            step, source = float(tsubint[0]), "TSUBINT"
        else:
            offs = _column_floats(table, "OFFS_SUB")
            if offs is not None and offs.size > 1 and offs[1] != offs[0]:
                step, source = float(offs[1] - offs[0]), "OFFS_SUB spacing"
            else:
                step, source = 1.0, "no TSUBINT/OFFS_SUB, 1 s per sub-integration assumed"
    else:
        tbin = _header_float(subint_hdr, "TBIN")
        if tbin is None or tbin <= 0:
            raise ValueError("PSRFITS search data need a positive TBIN.")
        step, source = float(tbin), "TBIN"

    contiguous = np.arange(n_rows, dtype=np.int64) * per_row
    centres = _column_floats(table, "OFFS_SUB")
    if centres is None or centres.size != n_rows or not np.isfinite(centres).all():
        return _TimeGrid(0.5 * step, step, contiguous, n_rows * per_row,
                         f"time step from {source}; no OFFS_SUB, rows assumed contiguous")
    start = float(centres[0]) - 0.5 * per_row * step + 0.5 * step
    rel = (centres - centres[0]) / step
    columns = np.round(rel).astype(np.int64)
    n_time = int(columns[-1]) + per_row if n_rows else 0
    on_grid = np.all(np.abs(rel - columns) <= 0.01)
    ordered = np.all(np.diff(columns) >= per_row)
    limit = _MAX_GAP_EXPANSION * n_rows * per_row
    if on_grid and ordered and n_time <= limit:
        gaps = n_time - n_rows * per_row
        note = f"time step from {source}; rows placed by OFFS_SUB"
        if gaps:
            note += f", {gaps} missing column(s) left NaN"
        return _TimeGrid(start, step, columns, n_time, note)
    if on_grid and ordered:
        reason = (f"OFFS_SUB gaps would need {n_time} columns for "
                  f"{n_rows * per_row} samples (limit {_MAX_GAP_EXPANSION}x)")
    else:
        reason = "OFFS_SUB irregular"
    logger.warning(
        "[psrfits] %s; rows are concatenated and timestamps after a gap "
        "are approximate.", reason)
    return _TimeGrid(start, step, contiguous, n_rows * per_row,
                     f"time step from {source}; {reason}, rows concatenated "
                     "so times after a gap are approximate")


def _frequency_axis(table, primary_hdr: fits.Header, subint_hdr: fits.Header,
                    nchan: int) -> tuple[float, float]:
    """``(first channel centre, signed channel step)`` in Hz."""
    dat_freq = None
    if "DAT_FREQ" in table.columns.names:
        try:
            dat_freq = np.asarray(table.data["DAT_FREQ"][0], dtype=np.float64).ravel()
        except (TypeError, ValueError, IndexError):
            dat_freq = None
    chan_bw = _header_float(subint_hdr, "CHAN_BW")
    if dat_freq is not None and dat_freq.size == nchan:
        if nchan > 1:
            step = float(dat_freq[-1] - dat_freq[0]) / (nchan - 1)
        else:
            step = chan_bw if chan_bw else float(primary_hdr.get("OBSBW", 0.0) or 0.0)
        return float(dat_freq[0]) * 1e6, step * 1e6
    logger.warning(
        "[psrfits] DAT_FREQ unavailable; falling back to OBSFREQ/OBSBW.")
    obsfreq = float(primary_hdr.get("OBSFREQ", 0.0) or 0.0) * 1e6
    obsbw = float(primary_hdr.get("OBSBW", 0.0) or 0.0) * 1e6
    step = obsbw / max(1, nchan)
    return obsfreq - 0.5 * obsbw + 0.5 * step, step


def _check_sample_width(data_col: np.ndarray, layout: _Layout) -> None:
    """Search ``NBITS`` must match how ``DATA`` is stored: bytes for packed
    and 8-bit samples, 2- or 4-byte elements for 16 and 32 bits."""
    if layout.folded:
        return
    stored_bits = 8 * np.asarray(data_col).dtype.itemsize
    expected = 8 if layout.nbits < 8 else layout.nbits
    if stored_bits != expected:
        raise ValueError(
            f"PSRFITS NBITS={layout.nbits} but DATA is stored as "
            f"{stored_bits}-bit elements.")


def _dynspec_from_subint(subint, layout: _Layout, pol_type: str,
                         grid: _TimeGrid) -> np.ndarray:
    """``(NCHAN, n_time)`` float32 Stokes I in physical units, NaN where a
    channel's weight is zero or no row covers the column. Read row by row
    so a search file is never held in memory at its full polarisation
    width."""
    n_rows = len(subint.data)
    per_row = 1 if layout.folded else layout.nsblk
    out = np.full((layout.nchan, grid.n_time), np.nan, dtype=np.float32)
    zero_off = 0.0
    if not layout.folded:
        zero_off = _header_float(subint.header, "ZERO_OFF") or 0.0
    data_col = subint.data["DATA"]
    _check_sample_width(data_col, layout)
    for row in range(n_rows):
        col0 = int(grid.row_columns[row])
        values = _row_values(data_col[row], layout)
        scl = _per_pol_chan(subint, "DAT_SCL", row, layout, 1.0)
        offs = _per_pol_chan(subint, "DAT_OFFS", row, layout, 0.0)
        flagged = _channel_weights(subint, row, layout.nchan) == 0.0
        if layout.folded:
            physical = values * scl[:, :, None] + offs[:, :, None]
            intensity = _stokes_i(physical, 0, pol_type).mean(axis=1)  # (nchan,)
            intensity[flagged] = np.nan
            out[:, col0] = intensity
        else:
            physical = (values - zero_off) * scl[None] + offs[None]
            intensity = _stokes_i(physical, 1, pol_type)  # (nsblk, nchan)
            intensity[:, flagged] = np.nan
            out[:, col0:col0 + per_row] = intensity.T
    return out


def convert_psrfits_to_sidecar_fits(src_path: str | Path) -> Path:
    """Read a PSRFITS file's SUBINT HDU and produce a 2-D dynspec sidecar.

    Returns the absolute path of the sidecar FITS. Cached via mtime.
    Raises ``ValueError`` with the reason when the layout the headers
    declare does not match the data, or is not supported.
    """
    src_path = Path(src_path).resolve()
    sidecar = _sidecar_path(src_path).resolve()
    if sidecar.exists() and sidecar.stat().st_mtime >= src_path.stat().st_mtime:
        logger.info("[psrfits] sidecar already up to date: %s", sidecar)
        return sidecar

    with fits.open(str(src_path), memmap=True, mode="readonly") as hdul:
        try:
            subint = hdul["SUBINT"]
        except KeyError as exc:
            raise ValueError(
                "PSRFITS file does not contain a SUBINT HDU.") from exc
        if subint.data is None or len(subint.data) == 0:
            raise ValueError("PSRFITS SUBINT table has no rows.")
        primary_hdr = hdul[0].header
        subint_hdr = subint.header
        layout = _read_layout(primary_hdr, subint_hdr)
        pol_type = str(subint_hdr.get("POL_TYPE", "")).strip().upper()
        n_rows = len(subint.data)
        logger.info(
            "[psrfits] %s %s rows=%s npol=%s nchan=%s nbin=%s nsblk=%s nbits=%s pol_type=%s",
            src_path.name, "fold" if layout.folded else "search", n_rows,
            layout.npol, layout.nchan, layout.nbin, layout.nsblk,
            layout.nbits, pol_type or "?",
        )

        grid = _time_grid(subint, subint_hdr, layout, n_rows)
        dynspec = _dynspec_from_subint(subint, layout, pol_type, grid)
        n_freq, n_time = dynspec.shape
        t_start_s, t_step_s = grid.start_s, grid.step_s
        f_lo_hz, f_step_hz = _frequency_axis(subint, primary_hdr, subint_hdr, n_freq)

        header = fits.Header()
        header["NAXIS"]  = 2
        header["NAXIS1"] = int(n_time)
        header["NAXIS2"] = int(n_freq)
        header["CTYPE1"] = "TIME"
        header["CTYPE2"] = "FREQ"
        header["CUNIT1"] = "s"
        header["CUNIT2"] = "Hz"
        header["CRPIX1"] = 1.0
        header["CRPIX2"] = 1.0
        header["CRVAL1"] = float(t_start_s)
        header["CRVAL2"] = float(f_lo_hz)
        header["CDELT1"] = float(t_step_s)
        header["CDELT2"] = float(f_step_hz)
        header["BUNIT"]  = "arb"
        header["ORIGIN"] = "VisIVO PSRFITS to dynspec converter"
        for key in ("SOURCE", "RA", "DEC", "TELESCOP", "BACKEND", "OBS_MODE"):
            if key in primary_hdr:
                header[key] = primary_hdr[key]
        n_flagged = int(np.count_nonzero(np.all(np.isnan(dynspec), axis=1))) if n_time else 0
        stokes = ("AA+BB" if pol_type in _FEED_PAIR_POL_TYPES and layout.npol >= 2
                  else "first polarisation")
        header.add_history(
            f"Converted from PSRFITS source {src_path.name}: "
            f"{'fold' if layout.folded else 'search'} mode, rows={n_rows}, "
            f"npol={layout.npol}, nchan={layout.nchan}, nbin={layout.nbin}, "
            f"nsblk={layout.nsblk}, nbits={layout.nbits}.")
        header.add_history(
            f"Stokes I = {stokes}; DATA scaled by DAT_SCL/DAT_OFFS; "
            f"zero-weight samples set to NaN"
            f"{'; phase-averaged' if layout.folded else ''}.")
        header.add_history(grid.note + ".")
        if n_flagged:
            header.add_history(
                f"{n_flagged} channel(s) have zero weight in every row.")

        sidecar.parent.mkdir(parents=True, exist_ok=True)
        fits.PrimaryHDU(data=dynspec, header=header).writeto(
                sidecar, overwrite=True)
        logger.info(
            "[psrfits] wrote sidecar shape=%s t_step=%s s freq=%s..%s Hz out=%s",
            dynspec.shape, t_step_s,
            f_lo_hz, f_lo_hz + f_step_hz * (n_freq - 1),
            sidecar,
        )
        return sidecar
