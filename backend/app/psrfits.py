"""PSRFITS (pulsar FITS) reader → dynamic-spectrum sidecar.

PSRFITS is a multi-HDU binary-table FITS layout used by most modern
pulsar back-ends (PRESTO, DSPSR, …). The pulsar payload lives in a
``SUBINT`` binary-table HDU with a ``DATA`` column whose row shape is
``(npol, nchan, nbin)``. Two flavours exist:

* **Search-mode**: many short subintegrations, ``nbin == 1`` and the
  time series is implicit in the row index. To produce a 2-D
  dynamic-spectrum (time vs frequency) we concatenate the rows along
  time.
* **Folded**: each row already represents a folded profile per
  subintegration; ``nbin`` is the number of phase bins (typically 64–
  1024). The natural 2-D product here is **time vs frequency** averaged
  across phase, which is the most useful view in the dynspec viewer
  (the phase profile is recovered later via the pulse-profile tool).

We pick Stokes I (npol index 0 — the standard convention) for the
sidecar. The output FITS is 2-D with ``CTYPE1='TIME'`` /
``CTYPE2='FREQ'`` so the dynspec classifier downstream promotes it to
``kind='dynspec'``.

The conversion is cached as a sidecar next to the source file
(``foo.fits`` → ``foo.dynspec.fits``) and reused on subsequent opens
as long as the source mtime hasn't moved forward.
"""
from __future__ import annotations

import logging
from pathlib import Path
from typing import Any

import numpy as np
from astropy.io import fits

logger = logging.getLogger("visivo.psrfits")


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


def convert_psrfits_to_sidecar_fits(src_path: str | Path) -> Path:
    """Read a PSRFITS file's SUBINT HDU and produce a 2-D dynspec sidecar.

    Returns the absolute path of the sidecar FITS. Cached via mtime.
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

        # DATA column: shape (n_subint, npol, nchan, nbin) — sometimes
        # transposed depending on the dialect. We tolerate the two common
        # layouts ((npol, nchan, nbin) per row, and (nbin, nchan, npol)).
        data = np.asarray(subint.data["DATA"])
        # Expected shape: (n_subint, npol, nchan, nbin) — DSPSR/PRESTO convention
        if data.ndim == 4 and data.shape[1] in (1, 2, 4):
            n_subint, n_pol, n_chan, n_bin = data.shape
        elif data.ndim == 4 and data.shape[3] in (1, 2, 4):
            # alternate: (n_subint, nbin, nchan, npol)
            data = np.transpose(data, (0, 3, 2, 1))
            n_subint, n_pol, n_chan, n_bin = data.shape
        else:
            raise ValueError(
                f"Unrecognised PSRFITS DATA shape: {data.shape}.")
        logger.info(
            "[psrfits] %s SUBINT data shape=%s (n_subint=%s, npol=%s, nchan=%s, nbin=%s)",
            src_path.name, data.shape, n_subint, n_pol, n_chan, n_bin,
        )

        # Pick Stokes I (pol index 0). The PSRFITS specification is
        # consistent on this: the first polarisation column is always I.
        stokes_i = data[:, 0, :, :].astype(np.float32, copy=False)

        if n_bin > 1:
            # Folded data → average across phase bins so the sidecar is
            # purely time vs frequency. The phase profile remains
            # reconstructable via the pulse-profile tool on the original
            # PSRFITS file (future hook), but for the dynspec viewer this
            # is the most useful 2-D product.
            dynspec = stokes_i.mean(axis=2)  # shape (n_subint, n_chan)
        else:
            dynspec = stokes_i[:, :, 0]      # shape (n_subint, n_chan)

        # Switch to (n_chan, n_subint) so NAXIS1=time / NAXIS2=frequency.
        dynspec = np.ascontiguousarray(dynspec.T).astype(np.float32)
        n_freq, n_time = dynspec.shape

        # Time sampling: PSRFITS SUBINT has TBIN (sample length in s) and
        # NSBLK (samples per subintegration). Time step per "time sample"
        # in our sidecar is TBIN × NSBLK when search-mode (n_bin==1) or
        # the subintegration length (TBIN × NBIN) when folded.
        primary_hdr = hdul[0].header
        subint_hdr = subint.header
        tbin = float(subint_hdr.get("TBIN", 1.0))
        nsblk = float(subint_hdr.get("NSBLK", 1.0))
        if n_bin > 1:
            t_step_s = float(tbin * n_bin)
        else:
            t_step_s = float(tbin * nsblk)

        # Frequency sampling: SUBINT has DAT_FREQ (per-channel centres in MHz).
        try:
            dat_freq = np.asarray(subint.data["DAT_FREQ"][0], dtype=np.float64)
            f_lo_hz = float(dat_freq[0]) * 1e6
            f_step_hz = float((dat_freq[-1] - dat_freq[0])
                              / max(1, len(dat_freq) - 1)) * 1e6
        except Exception as exc:
            logger.warning(
                "[psrfits] DAT_FREQ unavailable (%s); falling back to OBSFREQ/OBSBW.",
                exc,
            )
            obsfreq = float(primary_hdr.get("OBSFREQ", 0.0)) * 1e6
            obsbw = float(primary_hdr.get("OBSBW", 0.0)) * 1e6
            f_step_hz = obsbw / max(1, n_freq)
            f_lo_hz = obsfreq - 0.5 * obsbw

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
        header["CRVAL1"] = 0.0
        header["CRVAL2"] = float(f_lo_hz)
        header["CDELT1"] = float(t_step_s)
        header["CDELT2"] = float(f_step_hz)
        header["BUNIT"]  = "arb"
        header["ORIGIN"] = "VisIVO PSRFITS→dynspec converter"
        for key in ("SOURCE", "RA", "DEC", "TELESCOP", "BACKEND", "OBS_MODE"):
            if key in primary_hdr:
                header[key] = primary_hdr[key]
        header.add_history(
            f"Converted from PSRFITS source {src_path.name}: "
            f"n_subint={n_subint}, npol={n_pol}, nchan={n_chan}, nbin={n_bin}. "
            f"Stokes I selected; phase-averaged when n_bin>1.")

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
