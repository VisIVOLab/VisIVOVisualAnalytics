from __future__ import annotations

import logging
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np
from astropy.io import fits
from astropy import units as u
from astropy.wcs import WCS

logger = logging.getLogger("visivo.fits")


@dataclass(frozen=True)
class BeamMetadata:
    major: float | None
    minor: float | None
    pa: float | None


# Descriptive keywords an archive puts in the (empty) primary header of a
# multi-extension product; copied onto the image header when it lacks them.
# Spellings of one quantity are one entry, so a primary RESTFRQ never lands
# beside the extension's own RESTFREQ (the reader prefers the first spelling).
_PRIMARY_DESCRIPTIVE_KEYS = (("OBJECT",), ("TELESCOP",), ("INSTRUME",), ("OBSERVER",),
                             ("DATE-OBS",), ("BUNIT",), ("RESTFRQ", "RESTFREQ"), ("SPECSYS",))


def image_hdu_index(hdul) -> int:
    """Index of the HDU that holds the image.

    The primary when it has at least a 2-D array, otherwise the first image
    extension with one: ESO phase-3 and APEX products keep the primary empty
    (``NAXIS = 0``) and the cube in ``DATA_EXT``, with the variance cube in
    ``STAT_EXT`` beside it, and reading ``hdul[0]`` on those gave an image of
    shape ``()``. Headers only — no data is touched. 0 when nothing qualifies,
    so callers fail where they always did.
    """
    for index, hdu in enumerate(hdul):
        if not getattr(hdu, "is_image", False):
            continue
        header = hdu.header
        naxis = int(header.get("NAXIS", 0))
        if naxis >= 2 and sum(int(header.get(f"NAXIS{k}", 1)) > 1 for k in range(1, naxis + 1)) >= 2:
            return index
    return 0


class ScientificFitsDataset:
    """
    Minimal backend-side scientific FITS dataset helper.

    Centralizes:
    - FITS header access
    - WCS
    - active/degenerate axis handling
    - BUNIT
    - beam metadata
    - spectral axis identification for the current cube semantics
    - provenance-friendly metadata
    """

    def __init__(self, path: str | Path) -> None:
        self.path = str(Path(path))
        with fits.open(self.path, memmap=True, mode="readonly") as hdul:
            self.hdu_index = image_hdu_index(hdul)
            header = hdul[self.hdu_index].header.copy()
            shape = hdul[self.hdu_index].shape
            if self.hdu_index != 0:
                primary = hdul[0].header
                for spellings in _PRIMARY_DESCRIPTIVE_KEYS:
                    if any(key in header for key in spellings):
                        continue
                    for key in spellings:
                        if key in primary:
                            header[key] = primary[key]
                            break
        self.header = header
        self.shape = tuple(shape) if shape is not None else ()
        self.naxis = int(header.get("NAXIS", 0))
        self.axis_sizes = [int(header.get(f"NAXIS{i}", 1)) for i in range(1, self.naxis + 1)]
        self.active_axes = [axis for axis, size in enumerate(self.axis_sizes, start=1) if size > 1]
        self.degenerate_axes = [axis for axis, size in enumerate(self.axis_sizes, start=1) if size == 1]
        self.client_axis_to_fits_axis = {idx + 1: axis for idx, axis in enumerate(self.active_axes)}
        self.sanitized_header, self.wcs_sanitized_axes = self._sanitize_header_for_wcs(header)
        self.wcs = self._build_wcs(self.sanitized_header)
        self.wcs_status = self._determine_wcs_status()
        self.wcs_warning_message = self._build_wcs_warning_message()
        self.bunit = str(header.get("BUNIT", "")).strip()
        self.beam = BeamMetadata(
            major=float(header["BMAJ"]) if "BMAJ" in header else None,
            minor=float(header["BMIN"]) if "BMIN" in header else None,
            pa=float(header["BPA"]) if "BPA" in header else None,
        )
        # R-05: per-channel beam table (CASA multi-beam cubes carry a binary-table
        # HDU named "BEAMS" with per-plane BMAJ/BMIN/BPA). Read lazily-safe here so
        # flux/beam-area is correct channel-to-channel on wideband ALMA/SKA cubes.
        self._per_channel_beam: dict[int, BeamMetadata] | None = self._read_beams_table()
        self._stokes_planes: dict[str, int] | None = None  # lazy
        self._log_axis_debug()

    def _read_beams_table(self) -> dict[int, BeamMetadata] | None:
        """Read a CASA per-plane beam table (EXTNAME='BEAMS') if present.

        The table has one row per (channel, pol) with columns BMAJ, BMIN (arcsec)
        and BPA (deg), plus a CHAN column. We index by channel (collapsing over
        pol by taking the first row per channel) and store BMAJ/BMIN in **degrees**
        to match the primary-header convention used everywhere else. Returns None
        when there is no such table (the common single-beam case).
        """
        try:
            with fits.open(self.path, memmap=True, mode="readonly") as hdul:
                beams_hdu = None
                for hdu in hdul[1:]:
                    if str(hdu.header.get("EXTNAME", "")).strip().upper() == "BEAMS":
                        beams_hdu = hdu
                        break
                if beams_hdu is None:
                    return None
                cols = {c.upper(): c for c in beams_hdu.columns.names}
                if not {"BMAJ", "BMIN"} <= set(cols):
                    return None
                data = beams_hdu.data
                bmaj = np.asarray(data[cols["BMAJ"]], dtype=np.float64)
                bmin = np.asarray(data[cols["BMIN"]], dtype=np.float64)
                bpa = (np.asarray(data[cols["BPA"]], dtype=np.float64)
                       if "BPA" in cols else np.zeros_like(bmaj))
                chan = (np.asarray(data[cols["CHAN"]], dtype=np.int64)
                        if "CHAN" in cols else np.arange(bmaj.size, dtype=np.int64))
                # Convert each size column to degrees using ITS OWN unit (BMAJ and
                # BMIN can in principle carry different TUNITs). Default arcsec.
                def _col_to_deg(colname: str) -> float:
                    raw = beams_hdu.columns[cols[colname]].unit or "arcsec"
                    try:
                        return float(u.Unit(raw).to(u.deg))
                    except Exception:  # noqa: BLE001
                        return 1.0 / 3600.0  # assume arcsec on an unparsable unit
                bmaj_to_deg = _col_to_deg("BMAJ")
                bmin_to_deg = _col_to_deg("BMIN")
                out: dict[int, BeamMetadata] = {}
                for i in range(bmaj.size):
                    ch = int(chan[i])
                    if ch in out:
                        continue  # first pol wins
                    out[ch] = BeamMetadata(
                        major=float(bmaj[i]) * bmaj_to_deg,
                        minor=float(bmin[i]) * bmin_to_deg,
                        pa=float(bpa[i]),
                    )
                return out or None
        except Exception as exc:  # noqa: BLE001
            logger.warning("[fits] BEAMS table read failed path=%s: %s", self.path, exc)
            return None

    def beam_for_channel(self, channel: int | None) -> BeamMetadata:
        """Beam for a given channel — the per-plane beam when a BEAMS table is
        present, otherwise the single primary-header beam. R-05."""
        if channel is not None and self._per_channel_beam:
            b = self._per_channel_beam.get(int(channel))
            if b is not None:
                return b
        return self.beam

    @property
    def has_per_channel_beam(self) -> bool:
        return bool(self._per_channel_beam)

    @staticmethod
    def _is_celestial_ctype(ctype: str) -> bool:
        token = (ctype or "").strip().upper()
        if not token:
            return False
        celestial_prefixes = ("RA--", "DEC-", "GLON", "GLAT", "ELON", "ELAT", "HLON", "HLAT", "SLON", "SLAT", "TLON", "TLAT")
        return token.startswith(celestial_prefixes)

    @staticmethod
    def _is_angular_unit(unit: str) -> bool:
        raw = (unit or "").strip()
        if not raw:
            return True
        try:
            parsed = u.Unit(raw)
        except Exception:
            return False
        physical_type = str(parsed.physical_type)
        return physical_type == "angle"

    def _sanitize_header_for_wcs(self, header: fits.Header) -> tuple[fits.Header, list[int]]:
        sanitized = header.copy()
        sanitized_axes: list[int] = []
        for axis in range(1, self.naxis + 1):
            ctype = str(header.get(f"CTYPE{axis}", "")).strip()
            cunit_key = f"CUNIT{axis}"
            cunit = str(header.get(cunit_key, "")).strip()
            if self._is_celestial_ctype(ctype) and not self._is_angular_unit(cunit):
                logger.warning(
                    "Malformed FITS WCS: CTYPE%s='%s' with CUNIT%s='%s'. Overriding to 'deg' for compatibility.",
                    axis,
                    ctype,
                    axis,
                    cunit,
                )
                sanitized[cunit_key] = "deg"
                sanitized_axes.append(axis)
        return sanitized, sanitized_axes

    def _build_wcs(self, header: fits.Header) -> WCS | None:
        try:
            return WCS(header, relax=True, fix=True)
        except Exception as exc:
            logger.warning("[fits] WCS build failed for path=%s: %s. Falling back to degraded metadata-only mode.", self.path, exc)
            return None

    def _determine_wcs_status(self) -> str:
        if self.wcs is None:
            return "degraded"
        if self.wcs_sanitized_axes:
            return "sanitized"
        return "ok"

    def _build_wcs_warning_message(self) -> str:
        if self.wcs_status == "ok":
            return ""
        if self.wcs_status == "sanitized":
            axes = ", ".join(str(axis) for axis in self.wcs_sanitized_axes)
            return (
                f"WCS metadata was repaired for compatibility. "
                f"Celestial axis units were corrected on FITS axes: {axes}."
            )
        if self.wcs_sanitized_axes:
            axes = ", ".join(str(axis) for axis in self.wcs_sanitized_axes)
            return (
                f"WCS metadata was repaired on FITS axes {axes}, but full WCS construction still failed. "
                f"Using degraded linear axis metadata."
            )
        return "WCS construction failed. Using degraded linear axis metadata."

    def _log_axis_debug(self) -> None:
        ctype = [str(self.header.get(f"CTYPE{i}", "")).strip() for i in range(1, self.naxis + 1)]
        cunit = [str(self.header.get(f"CUNIT{i}", "")).strip() for i in range(1, self.naxis + 1)]
        logger.info(
            "[fits] path=%s ctype=%s cunit=%s active_axes=%s client_axis_to_fits_axis=%s wcs_status=%s sanitized_axes=%s",
            self.path,
            ctype,
            cunit,
            self.active_axes,
            self.client_axis_to_fits_axis,
            self.wcs_status,
            self.wcs_sanitized_axes,
        )

    def open_memmap(self):
        hdul = fits.open(self.path, memmap=True, mode="readonly")
        data = hdul[self.hdu_index].data
        if data is None:
            hdul.close()
            raise ValueError("FITS file contains no image data.")
        return hdul, data

    @staticmethod
    def squeeze_to_3d(data: np.ndarray) -> np.ndarray:
        arr = np.squeeze(data)
        # np.squeeze already dropped all degenerate (size-1) axes. If we still have
        # >3 dims, a *non-degenerate* extra axis remains (e.g. a real multi-plane
        # STOKES/FREQ axis): taking arr[0] keeps only plane 0 with no guarantee it
        # is Stokes I (R-10). Warn loudly rather than silently picking a plane.
        while arr.ndim > 3:
            logger.warning(
                "squeeze_to_3d: dropping a non-degenerate leading axis of size %d "
                "(keeping index 0 only), remaining shape %s; if this is a STOKES "
                "axis the plane may not be Stokes I.",
                arr.shape[0], arr.shape,
            )
            arr = arr[0]
        return arr

    def degenerate_axes_summary(self) -> str:
        entries: list[str] = []
        for axis in self.degenerate_axes:
            name = str(self.header.get(f"CTYPE{axis}", f"AXIS{axis}")).strip()
            val = float(self.header.get(f"CRVAL{axis}", 0.0))
            unit = str(self.header.get(f"CUNIT{axis}", "")).strip()
            label = f"{name}={val:g}"
            if unit and name.upper() != "STOKES":
                label += f" {unit}"
            label += " (1)"
            entries.append(label)
        return f"Collapsed axes: {', '.join(entries)}" if entries else ""

    def _rest_freq_hz(self) -> float | None:
        """RESTFRQ / RESTFREQ in Hz, or None when the header carries neither.

        Both spellings are in the wild (RESTFRQ is the FITS standard, RESTFREQ
        the older AIPS form). A non-positive value means "not set" as surely as
        a missing card, so it is reported as None rather than passed on to a
        client that would divide by it.
        """
        for key in ("RESTFRQ", "RESTFREQ"):
            value = self.header.get(key)
            if value is None:
                continue
            try:
                hz = float(value)
            except (TypeError, ValueError):
                continue
            if hz > 0.0:
                return hz
        return None

    def _axis_float(self, key: str, default: float = 0.0) -> float:
        return float(self.header.get(key, default))

    def _cdelt(self, axis: int) -> float:
        """Degrees (or native units) per pixel along `axis`, CD matrix included.

        ``CDELT`` is optional in FITS: a header may express the whole linear
        transformation as a ``CD`` matrix instead, and GLIMPSE mosaics — and so
        every VLKB cutout of one — do exactly that. Reading only ``CDELT`` gave
        those images a scale of 1.0 **degree** per pixel, which is not a small
        error: a 0.6° cutout was placed across 1800° of longitude, the axes were
        labelled with numbers near -3861, and every catalogue query built from
        those corners asked the VLKB about a region that does not exist. That is
        the "the coordinates are wrong and compact sources find nothing" pair;
        they are one defect.

        For the celestial axes the scale of axis *j* is the norm of **column j**
        of the matrix, not its diagonal element — a 90°-rotated field has zeros
        on the diagonal and perfectly square pixels. The sign is the orientation
        and has to be carried: a negative determinant means the longitude axis
        runs backwards, which is the usual case for sky images.
        """
        def keyword(name: str) -> float | None:
            raw = self.header.get(name)
            if raw is None:
                return None
            try:
                value = float(raw)
            except (TypeError, ValueError):
                return None
            return value if np.isfinite(value) and value != 0.0 else None

        def cd(i: int, j: int) -> float | None:
            raw = self.header.get(f"CD{i}_{j}")
            if raw is None:
                return None
            try:
                value = float(raw)
            except (TypeError, ValueError):
                return None
            return value if np.isfinite(value) else None

        # When both are written the CD matrix wins, which is what astropy does
        # and so what the rest of this class already assumes. A header carrying
        # both is malformed — the standard makes them exclusive — but they exist,
        # and agreeing with the WCS object beside us matters more than agreeing
        # with the keyword that happens to be read first.
        if axis in (1, 2):
            matrix = [[cd(i, j) for j in (1, 2)] for i in (1, 2)]
            if any(element is not None for row in matrix for element in row):
                m = [[element or 0.0 for element in row] for row in matrix]
                scale = float(np.hypot(m[0][axis - 1], m[1][axis - 1]))
                if scale > 0.0:
                    determinant = m[0][0] * m[1][1] - m[0][1] * m[1][0]
                    # Convention: the handedness flip is attributed to axis 1,
                    # which is how a negative CDELT1 is written when CDELT is.
                    if axis == 1 and determinant < 0.0:
                        return -scale
                    return scale

        written = keyword(f"CDELT{axis}")
        if written is not None:
            return written

        diagonal = cd(axis, axis)
        if diagonal is not None and diagonal != 0.0:
            return diagonal
        return 1.0

    def _wcs_ref_pixel_sky(self, pix_x: float, pix_y: float) -> list[float] | None:
        """Return [lon, lat] in degrees (native WCS frame) for a 0-indexed pixel via astropy."""
        if self.wcs is None:
            return None
        try:
            celestial = self.wcs.celestial
            if celestial is None or celestial.pixel_n_dim < 2:
                return None
            coords = celestial.all_pix2world([[pix_x, pix_y]], 0)
            lon, lat = float(coords[0][0]), float(coords[0][1])
            if not (np.isfinite(lon) and np.isfinite(lat)):
                return None
            return [lon, lat]
        except Exception as exc:
            logger.warning("[fits] WCS ref pixel (%.0f,%.0f) failed for path=%s: %s",
                           pix_x, pix_y, self.path, exc)
            return None

    def effective_cdelt(self) -> list[float] | None:
        """Return [cdelt1_eff, cdelt2_eff] in deg/pix with CORRECT SIGN via astropy WCS.

        Unlike the raw CDELT header keyword (which defaults to 1.0 for CD-matrix FITS),
        this uses a 1-pixel perturbation at the reference pixel to derive the effective
        per-axis pixel scale including the correct algebraic sign.  The sign encodes
        image orientation (e.g. RA-axis is typically negative).
        Returns None when WCS is unavailable.
        """
        if self.wcs is None:
            return None
        try:
            celestial = self.wcs.celestial
            if celestial is None or celestial.pixel_n_dim < 2:
                return None
            crpix1_0 = float(self.header.get("CRPIX1", 1.0)) - 1.0
            crpix2_0 = float(self.header.get("CRPIX2", 1.0)) - 1.0
            # sky at CRPIX (should equal CRVAL)
            sky_ref  = celestial.all_pix2world([[crpix1_0,       crpix2_0      ]], 0)[0]
            # sky at CRPIX + (1,0)  →  cdelt1 direction
            sky_p1x  = celestial.all_pix2world([[crpix1_0+ 1.0,  crpix2_0      ]], 0)[0]
            # sky at CRPIX + (0,1)  →  cdelt2 direction
            sky_p1y  = celestial.all_pix2world([[crpix1_0,        crpix2_0+ 1.0]], 0)[0]
            eff1 = float(sky_p1x[0] - sky_ref[0])
            eff2 = float(sky_p1y[1] - sky_ref[1])
            if not (np.isfinite(eff1) and np.isfinite(eff2)):
                return None
            return [eff1, eff2, self._cdelt(3)]
        except Exception as exc:
            logger.warning("[fits] effective_cdelt failed for path=%s: %s", self.path, exc)
            return None

    def is_dynamic_spectrum(self) -> bool:
        """Detect whether the FITS file is a dynamic-spectrum / beamformed
        product, i.e. its axes encode (time, frequency) rather than
        (sky_x, sky_y[, spectral]).

        Recognised CTYPE tokens:
            - TIME-like:  TIME, TIME-OBS, MJD, UTC, T (PSRFITS uses TIME)
            - FREQ-like:  FREQ, FREQUENCY, VRAD, VOPT, WAVE, AWAV
        Beamformed data is matched when the *active* axes contain at least
        one time-like and one frequency-like CTYPE. A pure spectral cube
        (RA, Dec, FREQ) is NOT matched because it has celestial axes too.
        """
        time_tokens = ("TIME", "MJD", "UTC")
        freq_tokens = ("FREQ", "FREQUEN", "WAVE", "AWAV")
        time_axes: list[int] = []
        freq_axes: list[int] = []
        for axis in self.active_axes:
            ctype = str(self.header.get(f"CTYPE{axis}", "")).strip().upper()
            if not ctype:
                continue
            if ctype.startswith(time_tokens):
                time_axes.append(axis)
            elif ctype.startswith(freq_tokens):
                freq_axes.append(axis)
        return bool(time_axes) and bool(freq_axes)

    def _file_size_bytes_safe(self) -> int:
        """Return the dataset file size in bytes, or 0 if unreadable.

        ``self.path`` is a string; we use ``os.path.getsize`` rather than
        a Path method to avoid AttributeError on non-Path inputs and to
        keep the call cheap. Errors (file missing, race with deletion)
        degrade to 0 — the recommended_load_mode falls back to
        ``auto_full`` in that case, matching legacy behaviour.
        """
        import os as _os
        try:
            return int(_os.path.getsize(self.path))
        except (OSError, TypeError):
            return 0

    def _full_res_threshold_bytes(self) -> int:
        """Default 4 GiB; override with VISIVO_FULL_RES_THRESHOLD_BYTES.

        At/above this size the client is told to default to preview-only
        and let the user opt explicitly into full-resolution download
        (or ROI crop). For cubes well below this threshold the legacy
        auto-full-load path is preserved unchanged.
        """
        import os as _os
        return int(_os.environ.get(
            "VISIVO_FULL_RES_THRESHOLD_BYTES",
            str(4 * 1024 ** 3),
        ))

    def geometry_metadata(self) -> dict[str, Any]:
        spacing = [self._cdelt(1), self._cdelt(2), self._cdelt(3)]
        origin = [
            self._axis_float("CRVAL1") - spacing[0] * (self._axis_float("CRPIX1", 1.0) - 1.0),
            self._axis_float("CRVAL2") - spacing[1] * (self._axis_float("CRPIX2", 1.0) - 1.0),
            self._axis_float("CRVAL3") - spacing[2] * (self._axis_float("CRPIX3", 1.0) - 1.0),
        ]
        squeezed = [size for size in self.axis_sizes if size > 1]
        if len(self.active_axes) == 2:
            width, height, depth = squeezed[0], squeezed[1], 1
            kind = "image"
        elif len(self.active_axes) == 3:
            width, height, depth = squeezed[0], squeezed[1], squeezed[2]
            kind = "cube"
        else:
            width = squeezed[0] if squeezed else 1
            height = squeezed[1] if len(squeezed) > 1 else 1
            depth = 1
            kind = "image"

        # Promote to dynamic-spectrum / beamformed kind when the CTYPE
        # signature is (time, frequency). The client routes 'dynspec' to the
        # image viewer with axis labels and tools adapted for waterfall plots
        # (no beam, no celestial WCS, no catalogue overlay).
        if self.is_dynamic_spectrum():
            kind = "dynspec"
        spectral_axis_type = ""
        spectral_axis_unit = ""
        if kind == "cube" and len(self.active_axes) >= 3:
            try:
                spectral = self.spectral_axis_metadata(0, 0)
                spectral_axis_type = str(spectral.get("axis_type", "") or "")
                spectral_axis_unit = str(spectral.get("axis_unit", "") or "")
            except Exception as exc:
                logger.warning("[fits] spectral metadata unavailable for path=%s: %s", self.path, exc)
        return {
            "kind": kind,
            "active_axes": len(self.active_axes),
            "degenerate_axes_summary": self.degenerate_axes_summary(),
            "wcs_status": self.wcs_status,
            "wcs_warning_message": self.wcs_warning_message,
            "wcs_sanitized_axes": list(self.wcs_sanitized_axes),
            "spectral_axis_type": spectral_axis_type,
            "spectral_axis_unit": spectral_axis_unit,
            # Rest frequency of the observed transition, in Hz. Already read from
            # the header by the line-fit path (compute/spectral.py); exposed here
            # so a client can place a spectral-line list on a VELOCITY axis, which
            # is impossible without it.
            "rest_freq_hz": self._rest_freq_hz(),
            "width": width,
            "height": height,
            "depth": depth,
            "spacing": spacing,
            "origin": origin,
            "bunit": self.bunit,
            "ctype": [str(self.header.get(f"CTYPE{i}", "")) for i in range(1, 4)],
            "cunit": [str(self.header.get(f"CUNIT{i}", "")) for i in range(1, 4)],
            "crval": [self._axis_float(f"CRVAL{i}") for i in range(1, 4)],
            "crpix": [self._axis_float(f"CRPIX{i}", 1.0) for i in range(1, 4)],
            "cdelt": spacing,
            # Pixel scale from astropy WCS (handles CD matrix / PC matrix correctly).
            # Clients should prefer this over sqrt(|CDELT1*CDELT2|) * 3600.
            "pixel_scale_arcsec": float(self.spatial_pixel_scale_arcsec()),
            # Effective per-axis CDELT in deg/pix WITH CORRECT SIGN, derived from a
            # 1-pixel perturbation at CRPIX via astropy WCS.  Replaces raw CDELT
            # (which defaults to 1.0 for CD-matrix-only FITS) in TAN sky↔pixel math.
            "effective_cdelt": self.effective_cdelt(),
            # Pre-computed sky coordinates (native frame, degrees) for layer alignment.
            # Computed via astropy WCS pixel_to_world, bypassing CDELT limitations.
            "wcs_ref_sky_00":   self._wcs_ref_pixel_sky(0.0,   0.0),
            "wcs_ref_sky_0100": self._wcs_ref_pixel_sky(0.0, 100.0),
            "beam_major": self.beam.major,
            "beam_minor": self.beam.minor,
            "beam_pa": self.beam.pa,
            # File-size aware loading policy (added for Pattern B / SDC2).
            # Client reads ``recommended_load_mode`` to decide whether to
            # auto-fetch full-resolution data or stay in preview-only mode
            # and ask the user explicitly. Threshold is configurable via
            # VISIVO_FULL_RES_THRESHOLD_BYTES; the default 4 GiB is
            # empirically the size above which a 16 GB-RAM laptop starts
            # thrashing on VTK volume rendering of an HI cube.
            #
            # NOTE: ``self.path`` is a string (``str(Path(path))``), not a
            # Path. We use ``os.path.getsize`` to avoid an AttributeError.
            "file_size_bytes": self._file_size_bytes_safe(),
            "full_res_threshold_bytes": self._full_res_threshold_bytes(),
            "recommended_load_mode": (
                "preview_only"
                if self._file_size_bytes_safe() >= self._full_res_threshold_bytes()
                else "auto_full"
            ),
        }

    # FITS CTYPE prefixes that denote a spectral axis (frequency, velocity,
    # wavelength, energy, …) per FITS WCS Paper III.
    # SPECLNMF is not in Paper III: it is what Obit MFImage writes for the
    # per-sub-band frequency axis of a wide-band image, and it is what every
    # MeerKAT MGCLS cube carries. Without it such a cube only finds its spectral
    # axis through the positional fallback below, which is wrong for any cube
    # whose spectral axis is not last.
    _SPECTRAL_CTYPE_PREFIXES = (
        "FREQ", "ENER", "WAVN", "VRAD", "WAVE", "VOPT", "ZOPT",
        "AWAV", "VELO", "BETA", "FELO", "SPECLNMF",
    )

    def cube_numpy_spectral_fits_axis(self) -> int:
        if len(self.active_axes) < 3:
            raise ValueError("Scientific cube helpers require at least three active FITS axes.")
        # Prefer the axis whose CTYPE actually names a spectral coordinate, so
        # transposed cubes (spectral not in the conventional 3rd slot) are
        # handled correctly. Fall back to the last active axis — the usual
        # position — when no CTYPE matches, preserving prior behaviour.
        for axis in self.active_axes:
            ctype = str(self.header.get(f"CTYPE{axis}", "")).strip().upper()
            if ctype.startswith(self._SPECTRAL_CTYPE_PREFIXES):
                return axis
        return self.active_axes[-1]

    def _linear_axis_coordinates(self, fits_axis: int, start: int, end: int) -> np.ndarray:
        crpix = float(self.sanitized_header.get(f"CRPIX{fits_axis}", 1.0))
        crval = float(self.sanitized_header.get(f"CRVAL{fits_axis}", 0.0))
        cdelt = float(self.sanitized_header.get(f"CDELT{fits_axis}", 1.0))
        pixels = np.arange(start, end + 1, dtype=np.float64)
        return crval + (pixels + 1.0 - crpix) * cdelt

    def spectral_axis_metadata(self, channel_start: int, channel_end: int) -> dict[str, Any]:
        spectral_fits_axis = self.cube_numpy_spectral_fits_axis()
        axis_label = str(self.header.get(f"CTYPE{spectral_fits_axis}", f"AXIS{spectral_fits_axis}")).strip()
        axis_unit = str(self.sanitized_header.get(f"CUNIT{spectral_fits_axis}", self.header.get(f"CUNIT{spectral_fits_axis}", ""))).strip()

        if self.wcs is None:
            logger.warning(
                "[fits] degraded spectral metadata path=%s fits_axis=%s axis_label=%s axis_unit=%s",
                self.path,
                spectral_fits_axis,
                axis_label,
                axis_unit or "-",
            )
            return {
                "coordinates": self._linear_axis_coordinates(spectral_fits_axis, channel_start, channel_end),
                "axis_type": axis_label,
                "axis_unit": axis_unit,
                "axis_label": axis_label,
                "fits_axis": spectral_fits_axis,
                "world_axis": max(0, spectral_fits_axis - 1),
            }

        pixel_naxis = int(self.wcs.pixel_n_dim)
        world_naxis = int(self.wcs.world_n_dim)
        if pixel_naxis <= 0 or world_naxis <= 0:
            return {
                "coordinates": self._linear_axis_coordinates(spectral_fits_axis, channel_start, channel_end),
                "axis_type": axis_label,
                "axis_unit": axis_unit,
                "axis_label": axis_label,
                "fits_axis": spectral_fits_axis,
                "world_axis": max(0, spectral_fits_axis - 1),
            }

        axis_types = [ptype or "" for ptype in (self.wcs.world_axis_physical_types or [])]
        correlation = np.asarray(self.wcs.axis_correlation_matrix, dtype=bool)
        spectral_pixel_index = spectral_fits_axis - 1

        spectral_world_index = None
        preferred_world_indices = [
            idx
            for idx, ptype in enumerate(axis_types)
            if ptype.startswith("em.") or "spect" in ptype or "freq" in ptype or "velo" in ptype
        ]
        for world_index in preferred_world_indices:
            if (
                world_index < correlation.shape[0]
                and spectral_pixel_index < correlation.shape[1]
                and correlation[world_index, spectral_pixel_index]
            ):
                spectral_world_index = world_index
                break
        if spectral_world_index is None:
            for world_index in range(min(correlation.shape[0], world_naxis)):
                if spectral_pixel_index < correlation.shape[1] and correlation[world_index, spectral_pixel_index]:
                    spectral_world_index = world_index
                    break
        if spectral_world_index is None:
            spectral_world_index = min(world_naxis - 1, spectral_pixel_index)

        reference_pixels = []
        for pixel_axis in range(pixel_naxis):
            crpix = float(self.header.get(f"CRPIX{pixel_axis + 1}", 1.0))
            reference_pixels.append(
                np.full(channel_end - channel_start + 1, crpix - 1.0, dtype=np.float64)
            )
        reference_pixels[spectral_pixel_index] = np.arange(
            channel_start, channel_end + 1, dtype=np.float64
        )
        try:
            world_values = self.wcs.pixel_to_world_values(*reference_pixels)
            coordinates = np.asarray(world_values[spectral_world_index], dtype=np.float64)
            if coordinates.ndim != 1 or coordinates.size != (channel_end - channel_start + 1):
                coordinates = np.ravel(coordinates).astype(np.float64, copy=False)
        except Exception as exc:
            logger.warning(
                "[fits] WCS spectral conversion failed path=%s fits_axis=%s error=%s. Falling back to linear header coordinates.",
                self.path,
                spectral_fits_axis,
                exc,
            )
            coordinates = self._linear_axis_coordinates(spectral_fits_axis, channel_start, channel_end)

        # The IVOA physical type (e.g. "em.freq", "spect.dopplerVeloc.radio").
        # Kept as a separate field; ``axis_type`` itself stays the FITS CTYPE so
        # the value is consistent with the WCS-degraded code paths above and
        # with what the desktop client / moment metadata consumers expect.
        axis_physical_type = axis_types[spectral_world_index] if spectral_world_index < len(axis_types) else ""

        # astropy normalises world units to SI (e.g. a "km/s" header axis comes
        # back as "m.s**-1", with pixel_to_world_values in m/s). Radio data is
        # authored and displayed in the header unit (km/s), so keep the header
        # CUNIT and convert the WCS coordinates *into* it — otherwise axis_unit
        # and the coordinate scale would disagree by 1000× and moment M1/M2
        # would be reported in the wrong unit. FREQ cubes (header already Hz)
        # are unaffected: source and target unit match, so nothing is rescaled.
        world_unit = ""
        if hasattr(self.wcs, "world_axis_units") and spectral_world_index < len(self.wcs.world_axis_units):
            world_unit = self.wcs.world_axis_units[spectral_world_index] or ""
        if world_unit and axis_unit and world_unit != axis_unit:
            try:
                import astropy.units as u
                coordinates = (coordinates * u.Unit(world_unit)).to(u.Unit(axis_unit)).value
            except Exception:
                # Header CUNIT not convertible to the WCS unit — trust the WCS.
                axis_unit = world_unit
        elif not axis_unit:
            axis_unit = world_unit
        return {
            "coordinates": coordinates,
            "axis_type": axis_label,
            "axis_physical_type": axis_physical_type,
            "axis_unit": axis_unit,
            "axis_label": axis_label,
            "fits_axis": spectral_fits_axis,
            "world_axis": spectral_world_index,
        }

    def spatial_pixel_scale_arcsec(self) -> float:
        """Representative LINEAR spatial pixel scale in arcseconds (mean of the two
        celestial axis scales) — for lengths: aperture radii, box sizes, fitted
        FWHM, PV path length, ℓ. For the pixel SOLID ANGLE (flux / beam-area /
        N_HI / M_HI) use ``spatial_pixel_area_arcsec2()`` instead, which is exact
        even for skewed WCS. Uses proj_plane_pixel_scales on the celestial
        sub-WCS; falls back to |CDELT1| × 3600 when WCS is unusable.
        """
        if self.wcs is not None:
            try:
                from astropy.wcs.utils import proj_plane_pixel_scales
                celestial = self.wcs.celestial
                if celestial is not None and celestial.pixel_n_dim >= 2:
                    scales_deg = proj_plane_pixel_scales(celestial)
                    return float(np.mean(np.abs(scales_deg))) * 3600.0
            except Exception as exc:
                logger.warning("[fits] proj_plane_pixel_scales failed for path=%s: %s. Using CDELT/CD fallback.", self.path, exc)
        return abs(self._cdelt(1)) * 3600.0

    def spatial_pixel_area_arcsec2(self) -> float:
        """EXACT pixel solid angle in arcsec² — the quantity flux/N_HI/M_HI need.

        Uses astropy ``proj_plane_pixel_area`` on the celestial sub-WCS, which is
        the true parallelogram area (|det| of the local CD matrix) and so is
        correct for non-square AND skewed/rotated pixels. Falls back to
        the header's own CDELT (or CD matrix) when the WCS is unusable.
        """
        if self.wcs is not None:
            try:
                from astropy.wcs.utils import proj_plane_pixel_area
                celestial = self.wcs.celestial
                if celestial is not None and celestial.pixel_n_dim >= 2:
                    # Returns deg² for angular-unit WCS.
                    return float(abs(proj_plane_pixel_area(celestial))) * 3600.0 * 3600.0
            except Exception as exc:
                logger.warning("[fits] proj_plane_pixel_area failed for path=%s: %s. Using CDELT fallback.", self.path, exc)
        return abs(self._cdelt(1)) * abs(self._cdelt(2)) * 3600.0 * 3600.0

    # ── Stokes axis detection ─────────────────────────────────────────────────

    @property
    def stokes_planes(self) -> dict[str, int]:
        """Map Stokes plane names (I, Q, U, V) to their 0-based slice index.

        Scans all FITS axes for CTYPE == 'STOKES'.  The pixel value in the
        FITS Stokes convention is:
          1 → I,  2 → Q,  3 → U,  4 → V,
         -1 → RR, -2 → LL, -3 → RL, -4 → LR  (unused here)

        The returned slice index is the 0-based position along the numpy
        array axis that corresponds to the Stokes FITS axis, after the
        standard FITS → numpy axis reversal.
        """
        if self._stokes_planes is not None:
            return self._stokes_planes

        _STOKES_NAMES = {1: "I", 2: "Q", 3: "U", 4: "V"}
        result: dict[str, int] = {}

        stokes_fits_axis: int | None = None
        for axis in range(1, self.naxis + 1):
            ctype = str(self.header.get(f"CTYPE{axis}", "")).strip().upper()
            if ctype == "STOKES":
                stokes_fits_axis = axis
                break

        if stokes_fits_axis is not None:
            n_stokes = self.axis_sizes[stokes_fits_axis - 1]
            crpix  = float(self.header.get(f"CRPIX{stokes_fits_axis}", 1.0))
            crval  = float(self.header.get(f"CRVAL{stokes_fits_axis}", 1.0))
            cdelt  = float(self.header.get(f"CDELT{stokes_fits_axis}", 1.0))
            # numpy axis = naxis - fits_axis  (FITS is Fortran-order, numpy is C-order)
            numpy_axis = self.naxis - stokes_fits_axis
            for pix_0based in range(n_stokes):
                stokes_value = int(round(crval + (pix_0based + 1 - crpix) * cdelt))
                name = _STOKES_NAMES.get(stokes_value)
                if name is not None:
                    result[name] = pix_0based   # slice index along the Stokes numpy axis

        self._stokes_planes = result
        return result

    def stokes_numpy_axis(self) -> int | None:
        """Return the numpy axis index of the Stokes dimension, or None."""
        for axis in range(1, self.naxis + 1):
            ctype = str(self.header.get(f"CTYPE{axis}", "")).strip().upper()
            if ctype == "STOKES":
                return self.naxis - axis
        return None

    def provenance_context(self) -> dict[str, Any]:
        return {
            "path": self.path,
            "bunit": self.bunit,
            "beam_major": self.beam.major,
            "beam_minor": self.beam.minor,
            "beam_pa": self.beam.pa,
            "active_axes": list(self.active_axes),
            "degenerate_axes": list(self.degenerate_axes),
            "wcs_status": self.wcs_status,
            "wcs_warning_message": self.wcs_warning_message,
            "wcs_sanitized_axes": list(self.wcs_sanitized_axes),
            "ctype": [str(self.header.get(f"CTYPE{i}", "")) for i in range(1, 4)],
            "cunit": [str(self.header.get(f"CUNIT{i}", "")) for i in range(1, 4)],
        }
