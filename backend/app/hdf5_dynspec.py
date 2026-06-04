"""HDF5 beamformed / dynamic-spectrum reader.

Recognises the LOFAR BFData layout (SUB_ARRAY_POINTING / BEAM / STOKES
hierarchy) and a generic fall-back for files that simply contain a 2-D
dataset of float32 / float64 values.

On open we **convert the chosen 2-D plane to a sidecar FITS file** in the
Workspace Exports directory and return its path. The rest of the backend
(geometry_metadata, image preview / full readers, region stats, …) then
treats it as an ordinary FITS image — no other module needs to know
HDF5 exists. The conversion is one-shot: subsequent opens of the same
``.h5`` file re-use the sidecar FITS as long as it is newer than the
HDF5 file.

The header written to the sidecar uses ``CTYPE1='TIME'`` and
``CTYPE2='FREQ'`` plus ``CDELT1/CRVAL1`` (in seconds) and
``CDELT2/CRVAL2`` (in Hz) so the FITS classifier promotes the dataset to
``kind='dynspec'`` automatically.

Limitations of this MVP:
- Only writes the first sub-array pointing / beam / Stokes plane.
- Time axis is assumed regularly sampled (``TSAMP`` or ``CADENCE``).
- Frequency axis is assumed regularly spaced; no SUBBAND irregular
  spacing handling.
- No flagging mask is carried over.
"""
from __future__ import annotations

import logging
from pathlib import Path
from typing import Any

import numpy as np
from astropy.io import fits

logger = logging.getLogger("visivo.hdf5")

_HDF5_SUFFIXES = (".h5", ".hdf5", ".hdf", ".bfdata")


def is_hdf5_path(path: str | Path) -> bool:
    """True when ``path``'s suffix looks like an HDF5 file."""
    return Path(path).suffix.lower() in _HDF5_SUFFIXES


def _open_h5(path: Path):
    try:
        import h5py  # noqa: PLC0415
    except ImportError as exc:  # pragma: no cover - dependency required
        raise RuntimeError(
            "h5py is required to read HDF5 beamformed / dynamic-spectrum "
            "files. Install it via `pip install h5py`."
        ) from exc
    return h5py.File(str(path), "r")


def _walk_datasets(group, prefix: str = "") -> list[tuple[str, Any]]:
    """Depth-first walk yielding (full_path, dataset) for every leaf."""
    out: list[tuple[str, Any]] = []
    for name, item in group.items():
        full = f"{prefix}/{name}" if prefix else name
        if hasattr(item, "shape") and hasattr(item, "dtype"):
            out.append((full, item))
        else:
            out.extend(_walk_datasets(item, full))
    return out


def _choose_dynspec_dataset(h5: Any) -> tuple[str, Any]:
    """Pick the most likely 2-D dynamic-spectrum dataset.

    Strategy:
      1. LOFAR BFData: look for the first STOKES plane via the
         SUB_ARRAY_POINTING_xxx / BEAM_yyy / STOKES_z path.
      2. Generic fall-back: pick the largest 2-D float dataset in the file.
    """
    # LOFAR BFData fast-path
    for name in h5.keys():
        if name.startswith("SUB_ARRAY_POINTING"):
            sap = h5[name]
            for beam_name in sap.keys():
                if beam_name.startswith("BEAM"):
                    beam = sap[beam_name]
                    for stokes_name in beam.keys():
                        if stokes_name.startswith("STOKES"):
                            ds = beam[stokes_name]
                            if hasattr(ds, "shape") and len(ds.shape) >= 2:
                                full = f"{name}/{beam_name}/{stokes_name}"
                                logger.info(
                                    "[hdf5] LOFAR BFData detected: %s shape=%s",
                                    full, ds.shape,
                                )
                                return full, ds

    # Generic: largest 2-D float dataset
    candidates: list[tuple[int, str, Any]] = []
    for full_path, ds in _walk_datasets(h5):
        if len(ds.shape) >= 2 and np.issubdtype(ds.dtype, np.floating):
            size = int(np.prod(ds.shape))
            candidates.append((size, full_path, ds))
    if not candidates:
        raise ValueError(
            "HDF5 file does not contain a 2-D float dataset suitable as a "
            "dynamic spectrum. Supported layouts: LOFAR BFData, or any file "
            "with a generic 2-D float dataset.")
    candidates.sort(key=lambda entry: entry[0], reverse=True)
    _, path, ds = candidates[0]
    logger.info("[hdf5] generic 2-D dataset chosen: %s shape=%s", path, ds.shape)
    return path, ds


def _extract_sampling(h5: Any, ds_path: str, ds: Any) -> dict[str, float]:
    """Best-effort extraction of time / frequency sampling metadata.

    Returns a dict with keys ``tsamp_s``, ``freq_lo_hz``, ``freq_step_hz``.
    Missing values default to 1.0 so the file still opens — the user can
    correct the header keywords manually afterwards if needed.
    """
    result: dict[str, float] = {
        "tsamp_s":      1.0,
        "freq_lo_hz":   0.0,
        "freq_step_hz": 1.0,
    }
    # File-level attrs
    file_attrs = {k.upper(): v for k, v in h5.attrs.items()}
    for key in ("TSAMP", "TSAMPLING", "DT", "DT_S", "CADENCE_S"):
        if key in file_attrs:
            try:
                result["tsamp_s"] = float(file_attrs[key])
                break
            except (TypeError, ValueError):
                continue
    for key in ("FREQ_START_HZ", "FREQ_LOW_HZ", "FCH1", "FREQ_LOW"):
        if key in file_attrs:
            try:
                result["freq_lo_hz"] = float(file_attrs[key])
                break
            except (TypeError, ValueError):
                continue
    for key in ("DELTA_FREQ_HZ", "DF_HZ", "FOFF", "CHANNEL_BW_HZ"):
        if key in file_attrs:
            try:
                result["freq_step_hz"] = float(file_attrs[key])
                break
            except (TypeError, ValueError):
                continue
    # Dataset-level attrs (override file-level if present)
    ds_attrs = {k.upper(): v for k, v in ds.attrs.items()}
    for key in ("TSAMP", "TSAMPLING", "DT", "DT_S"):
        if key in ds_attrs:
            try:
                result["tsamp_s"] = float(ds_attrs[key])
                break
            except (TypeError, ValueError):
                continue
    return result


def _sidecar_fits_path(hdf5_path: Path) -> Path:
    """``foo.h5`` → ``foo.h5.dynspec.fits`` (kept next to the source so it
    is easy to find / remove)."""
    return hdf5_path.with_suffix(hdf5_path.suffix + ".dynspec.fits")


def convert_hdf5_to_sidecar_fits(hdf5_path: str | Path) -> Path:
    """Convert the first 2-D dynamic-spectrum plane of ``hdf5_path`` to a
    sidecar FITS file. Cached: re-uses an existing sidecar that is newer
    than the source HDF5.

    Returns the absolute path of the FITS file.
    """
    hdf5_path = Path(hdf5_path).resolve()
    sidecar = _sidecar_fits_path(hdf5_path).resolve()
    if sidecar.exists() and sidecar.stat().st_mtime >= hdf5_path.stat().st_mtime:
        logger.info(
            "[hdf5] sidecar FITS already up to date: %s", sidecar,
        )
        return sidecar

    with _open_h5(hdf5_path) as h5:
        ds_path, ds = _choose_dynspec_dataset(h5)
        sampling = _extract_sampling(h5, ds_path, ds)

        # If the dataset has > 2 dims, collapse the leading dims to a single
        # 2-D plane: pick the first index along each extra axis.
        data = ds[...]
        while data.ndim > 2:
            data = data[0]
        data = np.asarray(data, dtype=np.float32)
        # Convention: NAXIS1 = time, NAXIS2 = frequency. Many HDF5 layouts
        # have (time, freq) row-major already; if shape suggests otherwise
        # (height > width by a large factor) we leave it as-is and let the
        # client display.
        height, width = data.shape

        header = fits.Header()
        header["NAXIS"]  = 2
        header["NAXIS1"] = int(width)
        header["NAXIS2"] = int(height)
        header["CTYPE1"] = "TIME"
        header["CTYPE2"] = "FREQ"
        header["CUNIT1"] = "s"
        header["CUNIT2"] = "Hz"
        header["CRPIX1"] = 1.0
        header["CRPIX2"] = 1.0
        header["CRVAL1"] = 0.0
        header["CRVAL2"] = float(sampling["freq_lo_hz"])
        header["CDELT1"] = float(sampling["tsamp_s"])
        header["CDELT2"] = float(sampling["freq_step_hz"])
        header["BUNIT"]  = "arb"
        header["ORIGIN"] = "VisIVO HDF5→FITS dynspec converter"
        header.add_history(
            f"Converted from HDF5 source {hdf5_path.name} dataset path "
            f"{ds_path} on demand."
        )

        sidecar.parent.mkdir(parents=True, exist_ok=True)
        fits.PrimaryHDU(data=data, header=header).writeto(
                sidecar, overwrite=True)
        logger.info(
            "[hdf5] wrote sidecar FITS path=%s shape=%s tsamp=%s freq0=%s df=%s",
            sidecar, data.shape,
            sampling["tsamp_s"], sampling["freq_lo_hz"],
            sampling["freq_step_hz"],
        )
        return sidecar
