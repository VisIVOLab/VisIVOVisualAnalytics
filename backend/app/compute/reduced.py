"""Workers behind ``/v1/cube/reduced_info`` and ``/v1/cube/reduced_bin``: the
reduced cube a headset renders itself (issue #167), and the world coordinates
of its voxels. The reduction rules are in :mod:`app.cube_reduce`."""
from __future__ import annotations

import hashlib
import math
import os
from typing import Any

import numpy as np

from .. import cube_reduce
from ..noise_threshold import channel_noise_sigma, default_threshold, flat_noise_sigma, stride_for
from ..product_cache import file_identity
from ._common import ScientificFitsDataset, logger


def _finite_list(values: np.ndarray) -> list[float | None]:
    """JSON-safe list: NaN and infinities become null."""
    return [float(v) if math.isfinite(float(v)) else None for v in np.asarray(values).ravel()]


def _short_ctype(ctype: str) -> str:
    """``RA---NCP`` -> ``RA``, ``GLON-CAR`` -> ``GLON``, ``VELO-HEL`` -> ``VELO``."""
    return str(ctype or "").strip().split("-")[0].strip()


def _spatial_axis(dataset: ScientificFitsDataset, which: int, shape_zyx, centres) -> dict[str, Any]:
    """World coordinate of each reduced column (``which`` = 0) or row (1).

    Read with the file's full WCS along the central row (for x) or column (for
    y) of the field, so the labels on a projected sky axis (NCP, SIN) are the
    coordinates of those pixels, not a linear approximation of them. Falls back
    to the header's linear axis when the WCS cannot be evaluated."""
    fits_axis = dataset.active_axes[which]
    other_axis = dataset.active_axes[1 - which]
    depth, height, width = shape_zyx
    ctype = str(dataset.header.get(f"CTYPE{fits_axis}", "")).strip()
    unit = str(dataset.sanitized_header.get(f"CUNIT{fits_axis}", "")).strip()
    values = None
    method = "linear"
    if dataset.wcs is not None:
        try:
            n_pix = int(dataset.wcs.pixel_n_dim)
            pix = []
            for p in range(n_pix):
                crpix = float(dataset.header.get(f"CRPIX{p + 1}", 1.0))
                pix.append(np.full(centres.size, crpix - 1.0, dtype=np.float64))
            pix[fits_axis - 1] = centres.astype(np.float64)
            centre_other = 0.5 * ((width if which == 1 else height) - 1)
            pix[other_axis - 1] = np.full(centres.size, centre_other, dtype=np.float64)
            world = dataset.wcs.pixel_to_world_values(*pix)
            values = np.asarray(world[fits_axis - 1], dtype=np.float64).ravel()
            if values.size != centres.size or not np.all(np.isfinite(values)):
                values = None
            else:
                method = "wcs"
                wunits = getattr(dataset.wcs, "world_axis_units", None) or []
                if fits_axis - 1 < len(wunits) and wunits[fits_axis - 1]:
                    unit = str(wunits[fits_axis - 1])
        except Exception as exc:  # noqa: BLE001 - fall back to the linear header axis
            logger.info("[reduced] spatial WCS evaluation failed (%s): linear axis", exc)
            values = None
    if values is None:
        crpix = float(dataset.sanitized_header.get(f"CRPIX{fits_axis}", 1.0))
        crval = float(dataset.sanitized_header.get(f"CRVAL{fits_axis}", 0.0))
        cdelt = dataset._cdelt(fits_axis)  # noqa: SLF001 - the CD-matrix-aware scale
        values = crval + (centres + 1.0 - crpix) * cdelt
    if unit in ("", "deg") and _short_ctype(ctype).upper() in ("RA", "DEC", "GLON", "GLAT",
                                                                "ELON", "ELAT"):
        unit = "deg"
    return {
        "name": _short_ctype(ctype) or ("x" if which == 0 else "y"),
        "ctype": ctype,
        "unit": unit,
        "method": method,
        "world": _finite_list(values),
    }


def _spectral_axis(dataset: ScientificFitsDataset, shape_zyx, centres) -> dict[str, Any]:
    """Spectral coordinate of each reduced channel, native and (when it can be
    expressed so) as velocity in km/s with the cube's own convention, or
    optical velocity cz = c (nu0/nu - 1) from a frequency axis and RESTFRQ, the
    conventions of the kinematic fit and model (``compute.kinematic_fit``)."""
    from .kinematic_fit import velocity_axis_kms  # noqa: PLC0415
    depth = int(shape_zyx[0])
    fits_axis = dataset.active_axes[2]
    ctype = str(dataset.header.get(f"CTYPE{fits_axis}", "")).strip()
    out: dict[str, Any] = {"name": _short_ctype(ctype) or "z", "ctype": ctype,
                           "unit": "", "world": [], "velocity_kms": None,
                           "velocity_convention": "", "channel_width_kms": None,
                           "full_velocity_kms": None, "full_channel_width_kms": None,
                           "specsys": str(dataset.header.get("SPECSYS", "") or "").strip()}
    if dataset.cube_numpy_spectral_fits_axis() != fits_axis:
        # A transposed cube: the third numpy axis is not the spectral one.
        crpix = float(dataset.sanitized_header.get(f"CRPIX{fits_axis}", 1.0))
        crval = float(dataset.sanitized_header.get(f"CRVAL{fits_axis}", 0.0))
        cdelt = float(dataset.sanitized_header.get(f"CDELT{fits_axis}", 1.0))
        out["unit"] = str(dataset.sanitized_header.get(f"CUNIT{fits_axis}", "")).strip()
        out["world"] = _finite_list(crval + (centres + 1.0 - crpix) * cdelt)
        return out
    spectral = dataset.spectral_axis_metadata(0, depth - 1)
    coords = np.asarray(spectral.get("coordinates", []), dtype=np.float64).ravel()
    unit = str(spectral.get("axis_unit", "") or "")
    out["unit"] = unit
    if coords.size != depth:
        out["world"] = _finite_list(centres)
        out["unit"] = "channel"
        return out
    channels = np.arange(depth, dtype=np.float64)
    native = np.interp(centres, channels, coords)
    out["world"] = _finite_list(native)
    rest = dataset._rest_freq_hz() or 0.0  # noqa: SLF001
    vel_full, vunit, mode = velocity_axis_kms(coords, unit, ctype, float(rest))
    if mode == "velocity":
        out["velocity_kms"] = _finite_list(np.interp(centres, channels, vel_full))
        # The label the selection statistics and the kinematic fit give.
        from .kinematic_fit import velocity_convention_label  # noqa: PLC0415
        out["velocity_convention"] = velocity_convention_label(unit, ctype, dataset.header)[1]
        finite = vel_full[np.isfinite(vel_full)]
        if finite.size > 1:
            out["channel_width_kms"] = float(np.mean(np.abs(np.diff(finite))))
        # Every full-resolution channel too (issue #171: the velocity of a picked
        # channel, of a spectrum, of a selection's channel range), with each
        # channel's own width from its half-channel edges, the widths the
        # kinematic fit and the selection statistics integrate with.
        from .kinematic_fit import channel_edges  # noqa: PLC0415
        edges, _, _ = velocity_axis_kms(channel_edges(coords), unit, ctype, float(rest))
        out["full_velocity_kms"] = _finite_list(vel_full)
        out["full_channel_width_kms"] = _finite_list(np.abs(np.diff(edges)))
    return out


#: The default threshold of a simulation volume: this percentile of its
#: values, the desktop's own default for data that is not noise around a
#: median (``noise_threshold.default_threshold``). A simulation's field has no
#: noise for the radio rule to read, and a field symmetric about zero (a
#: velocity component) would pass for noise.
SIMULATION_PERCENTILE = 99.0


def is_simulation(dataset: ScientificFitsDataset) -> bool:
    """A simulation volume: three active axes, none spectral and none on the
    sky (linear CTYPEs such as a VBT volume's X, Y, Z, or none at all). Its
    axes are lengths, its values a physical field (no noise, beam or rest
    line)."""
    axes = list(dataset.active_axes)
    if len(axes) < 3:
        return False
    if dataset.wcs is not None and getattr(dataset.wcs, "has_celestial", False):
        return False
    for axis in axes[:3]:
        ctype = str(dataset.header.get(f"CTYPE{axis}", "")).strip().upper()
        if ctype.startswith(dataset._SPECTRAL_CTYPE_PREFIXES):  # noqa: SLF001
            return False
        if dataset._is_celestial_ctype(ctype) or ctype in ("RA", "DEC"):  # noqa: SLF001
            return False
    return True


def _linear_axis(dataset: ScientificFitsDataset, which: int, centres) -> dict[str, Any]:
    """World coordinate of each reduced voxel along axis ``which`` (0 x, 1 y,
    2 z) of a simulation volume, from the header's linear axis (CRVAL, CRPIX,
    CDELT), with its spacing per full-resolution voxel."""
    fits_axis = dataset.active_axes[which]
    ctype = str(dataset.header.get(f"CTYPE{fits_axis}", "")).strip()
    unit = str(dataset.sanitized_header.get(f"CUNIT{fits_axis}", "")).strip()
    crpix = float(dataset.sanitized_header.get(f"CRPIX{fits_axis}", 1.0))
    crval = float(dataset.sanitized_header.get(f"CRVAL{fits_axis}", 0.0))
    cdelt = _axis_spacing(dataset, which)
    values = crval + (centres + 1.0 - crpix) * (cdelt if cdelt is not None else 1.0)
    out: dict[str, Any] = {
        "name": _short_ctype(ctype) or "XYZ"[which],
        "ctype": ctype,
        "unit": unit,
        "method": "linear",
        "world": _finite_list(values),
        "spacing": cdelt,
    }
    if which == 2:
        # No velocity: the keys of a spectral axis, empty, for a reader of either.
        out.update({"velocity_kms": None, "velocity_convention": "", "channel_width_kms": None,
                    "full_velocity_kms": None, "full_channel_width_kms": None, "specsys": ""})
    return out


def _axis_spacing(dataset: ScientificFitsDataset, which: int) -> float | None:
    """The header's step per full-resolution voxel along axis ``which`` (CD
    matrix included, signed), or None."""
    try:
        v = float(dataset._cdelt(dataset.active_axes[which]))  # noqa: SLF001
    except Exception:  # noqa: BLE001
        return None
    return v if math.isfinite(v) and v != 0.0 else None


def _percentile_threshold(sample: np.ndarray) -> float | None:
    """:data:`SIMULATION_PERCENTILE` of the finite values of ``sample``."""
    s = np.asarray(sample, dtype=np.float32).ravel()
    s = s[np.isfinite(s)]
    return float(np.percentile(s, SIMULATION_PERCENTILE)) if s.size else None


def _open_cube(path: str):
    dataset = ScientificFitsDataset(path)
    hdul, raw = dataset.open_memmap()
    data = dataset.squeeze_to_3d(raw)
    if data.ndim != 3 or len(dataset.active_axes) < 3:
        hdul.close()
        raise ValueError("A reduced cube needs a 3-D cube.")
    return dataset, hdul, data


def _factors(shape_zyx, factor_xy: int, factor_z: int, max_dim: int,
             encoding: str = "uint8", limit: bool = True) -> tuple[int, int, int]:
    auto_xy, _, auto_z = cube_reduce.auto_factors(shape_zyx, max_dim)
    fxy = int(factor_xy) if factor_xy and factor_xy > 0 else auto_xy
    fz = int(factor_z) if factor_z and factor_z > 0 else auto_z
    if limit:
        cube_reduce.check_size(cube_reduce.reduced_shape(shape_zyx, (fxy, fxy, fz)), encoding)
    return fxy, fxy, fz


def _full_threshold(data: np.ndarray, simulation: bool = False) -> float | None:
    """The cube's default 3-D threshold, the rule the desktop's view starts from
    (issue #93): on a stride sample, for the voxels of the WHOLE cube. A
    simulation volume's is a percentile (:data:`SIMULATION_PERCENTILE`)."""
    return _full_noise(data, simulation)[0]


def _full_noise(data: np.ndarray, simulation: bool = False,
                ) -> tuple[float | None, float | None]:
    """(:func:`_full_threshold`, the desktop's Noise σ) from one stride sample
    of the whole cube: σ per sampled channel as the desktop's Estimate Noise
    measures it over the whole field (``noise_threshold.channel_noise_sigma``),
    their median (issue #258). A simulation volume has no noise: its
    threshold is the percentile, its σ None."""
    n_voxels = int(np.prod(data.shape))
    st = stride_for(n_voxels)
    sample = np.asarray(data[::st, ::st, ::st], dtype=np.float32)
    if not np.isfinite(sample).any():
        return None, None
    if simulation:
        return _percentile_threshold(sample), None
    t = default_threshold(sample, n_voxels)
    return (float(t) if t is not None and math.isfinite(t) else None,
            channel_noise_sigma(sample))


#: The rest frequency of the HI line (Hz), to name it in the info line.
HI_REST_HZ = 1420405751.768


def _beam(dataset: ScientificFitsDataset, depth: int) -> dict[str, Any] | None:
    """The restoring beam (BMAJ/BMIN/BPA, issue #258) in arcsec and degrees,
    or None. A cube with one beam per channel (a BEAMS table) and no header
    beam gives its central channel's, flagged ``per_channel``."""
    b = dataset.beam
    per_channel = False
    if not (b.major and b.minor) and dataset.has_per_channel_beam:
        b = dataset.beam_for_channel(max(0, int(depth) // 2))
        per_channel = True
    if not (b.major and b.minor) or not (math.isfinite(b.major) and math.isfinite(b.minor)):
        return None
    pa = float(b.pa) if b.pa is not None and math.isfinite(float(b.pa)) else None
    return {"major_arcsec": float(b.major) * 3600.0, "minor_arcsec": float(b.minor) * 3600.0,
            "pa_deg": pa, "per_channel": per_channel}


def binning_threshold(data: np.ndarray, reduced: np.ndarray, mode: str,
                      simulation: bool = False) -> float | None:
    """The default threshold of a reduced cube.

    ``mean``: the noise rule applied to the reduced cube itself, whose noise is
    lower than the cube's (it falls as the square root of the block).
    ``max``: the full cube's threshold. A block maximum is above it exactly when
    some voxel of the block is, so the reduced cube draws the blocks holding a
    voxel the desktop would draw. The noise rule on the reduced cube reads the
    noise from the lower tail, which a block maximum compresses, and set too low
    a cut: on pure Gaussian noise binned 3x3x1 it drew 0.15 % of the voxels
    instead of the rule's 1e-4.

    A simulation volume (``simulation``): the percentile rule
    (:data:`SIMULATION_PERCENTILE`) on the reduced cube (``mean``) or on the
    full cube (``max``).
    """
    if mode == "max":
        return _full_threshold(data, simulation)
    st = stride_for(reduced.size)
    sample = reduced[::st, ::st, ::st]
    if not np.isfinite(sample).any():
        return None
    if simulation:
        return _percentile_threshold(sample)
    t = default_threshold(sample, int(reduced.size))
    return float(t) if t is not None and math.isfinite(t) else None


#: A reduced cube is read from a pyramid level of at most this many voxels
#: (1 GiB of float32) when it can be (see :func:`app.cube_reduce.pyramid_plan`).
PYRAMID_READ_VOXELS = int(os.environ.get("VISIVO_VR_PYRAMID_READ_BYTES", str(1024 ** 3))) // 4
#: ...and never from a larger one than this (16 GiB): a finer level is read,
#: in bounded pieces, only so that automatic factors are not inflated more
#: than cube_reduce.MAX_INFLATION.
PYRAMID_MAX_READ_VOXELS = int(os.environ.get("VISIVO_VR_PYRAMID_MAX_READ_BYTES",
                                             str(16 * 1024 ** 3))) // 4
#: Level-0 voxels read to repair the level voxels next to blank ones (1 GiB).
PYRAMID_REPAIR_VOXELS = int(os.environ.get("VISIVO_VR_PYRAMID_REPAIR_BYTES",
                                           str(1024 ** 3))) // 4
#: The memory one piece of a pyramid level takes while it is reduced.
_PYRAMID_BLOCK_BYTES = 128 * 1024 * 1024


class _Pyramid:
    """The cube's brick pyramid, opened for a reduced cube: checked against the
    cube's file (a changed file is refused) and against the cube the reduction
    reads (the same (z, y, x) shape), with the level plan of the request."""

    def __init__(self, pyramid_path: str, source: str, shape_zyx, factor_xy: int,
                 factor_z: int, max_dim: int) -> None:
        from ..brick_pyramid import BrickPyramid  # noqa: PLC0415
        self.pyr = BrickPyramid(pyramid_path, source=source)
        try:
            shapes = [self.pyr.shape(k) for k in range(self.pyr.levels)]
            if tuple(shapes[0]) != tuple(int(n) for n in shape_zyx):
                raise ValueError(f"The brick pyramid holds a {shapes[0]} cube, not the "
                                 f"{tuple(shape_zyx)} one opened.")
            self.shapes = shapes
            self.level, self.extra, self.factors = cube_reduce.pyramid_plan(
                shapes, factor_xy, factor_z, max_dim, PYRAMID_READ_VOXELS,
                PYRAMID_MAX_READ_VOXELS)
            auto_xy, _, auto_z = cube_reduce.auto_factors(shapes[0], max_dim)
            self.asked = (int(factor_xy) if factor_xy and factor_xy > 0 else auto_xy,
                          int(factor_z) if factor_z and factor_z > 0 else auto_z)
            self.repair: dict[str, int] | None = None
        except BaseException:
            self.pyr.close()
            raise

    def describe(self) -> dict[str, Any]:
        lz, ly, lx = self.shapes[self.level]
        out = {"level": self.level, "levels": len(self.shapes),
               "level_shape": [lx, ly, lz], "extra_factors": list(self.extra),
               "brick": self.pyr.brick,
               "asked_factors": [self.asked[0], self.asked[0], self.asked[1]],
               "reduction": ("block means: the level weighted by pixel count, level "
                             "voxels next to blank ones repaired from level 0")}
        if self.repair is not None:
            out["border"] = dict(self.repair)
        return out

    def threshold(self, n_voxels: int, simulation: bool = False) -> float | None:
        """The cube's default threshold from level-0 bricks, as the out-of-core
        render session takes it (:func:`app.render_session._ooc.level0_sample`)."""
        return self.noise(n_voxels, simulation)[0]

    def noise(self, n_voxels: int, simulation: bool = False,
              ) -> tuple[float | None, float | None]:
        """(:meth:`threshold`, σ) from one level-0 sample. The sample has no
        channel structure, so σ is 1.4826 x MAD about ONE median over all its
        channels together (``noise_threshold.flat_noise_sigma``), not the
        desktop's median of per-channel σ, each about its own channel's
        median. The two agree only for a flat baseline (every channel's
        median the same, the noise the same along the band). Where channel
        medians vary (continuum left in, a baseline ripple, a frequency-
        dependent noise) the spread of the medians adds to this σ, and it is
        larger than the desktop's; ``noise_sigma_method`` says which rule was
        used. A simulation volume: the percentile threshold
        (:data:`SIMULATION_PERCENTILE`) and no σ."""
        from ..render_session._ooc import level0_sample  # noqa: PLC0415
        sample = level0_sample(self.pyr)
        if not sample.size:
            return None, None
        if simulation:
            return _percentile_threshold(sample), None
        t = default_threshold(sample, n_voxels)
        return (float(t) if t is not None and math.isfinite(t) else None,
                flat_noise_sigma(sample))

    def reduce(self) -> np.ndarray:
        h5 = self.pyr._h5  # noqa: SLF001
        out, self.repair = cube_reduce.pyramid_reduce(
            h5[f"level{self.level}/data"], h5["level0/data"], self.level, self.shapes[0],
            self.extra, self.pyr.brick, max_block_bytes=_PYRAMID_BLOCK_BYTES,
            repair_voxels=PYRAMID_REPAIR_VOXELS)
        return out

    def close(self) -> None:
        self.pyr.close()


def _check_limit(shape_zyx, factors, encoding: str, limit: bool) -> None:
    if limit:
        cube_reduce.check_size(cube_reduce.reduced_shape(shape_zyx, factors), encoding)


def worker_cube_reduced_info(path: str, factor_xy: int, factor_z: int,
                             max_dim: int, encoding: str = "uint8",
                             limit: bool = True, pyramid_path: str | None = None,
                             ) -> dict[str, Any]:
    """Shape, factors, default threshold and world axes of a reduced cube,
    without reducing it. With ``pyramid_path`` the reduced cube comes from the
    cube's brick pyramid (``source`` "pyramid", see :func:`worker_cube_reduced`):
    the factors are then those of :func:`app.cube_reduce.pyramid_plan` and the
    threshold comes from level-0 bricks, not from a stride over the cube."""
    # Reuse the cache's stat identity (including VBT headers/byte order),
    # without reading a potentially huge cube for a content hash.
    identity = file_identity(path)
    revision = hashlib.sha256(identity.encode()).hexdigest() if identity else ""
    dataset, hdul, data = _open_cube(path)
    pyramid = None
    try:
        simulation = is_simulation(dataset)
        shape = tuple(int(n) for n in data.shape)
        if pyramid_path:
            pyramid = _Pyramid(pyramid_path, path, shape, factor_xy, factor_z, max_dim)
            factors = pyramid.factors
            _check_limit(shape, factors, encoding, limit)
            threshold, sigma = pyramid.noise(int(np.prod(shape)), simulation)
            sigma_method = ("1.4826 x MAD about the median of a sample of level-0 bricks "
                            "of the brick pyramid, all channels together: the desktop's "
                            "per-channel rule only for a flat baseline, larger where "
                            "channel medians vary")
            threshold_method = ("the 99th percentile of a sample of level-0 bricks"
                                if simulation else "the noise rule on level-0 bricks")
            plan = pyramid.describe()
        else:
            factors = _factors(shape, factor_xy, factor_z, max_dim, encoding, limit)
            threshold, sigma = _full_noise(data, simulation)
            st = stride_for(int(np.prod(shape)))
            every = "" if st == 1 else f", on every {st}th voxel along each axis"
            sigma_method = ("the desktop's Estimate Noise over the whole field: median over "
                            "channels of 1.4826 x MAD about each channel's median" + every)
            threshold_method = (f"the {SIMULATION_PERCENTILE:g}th percentile of the values{every}"
                                if simulation else f"the noise rule{every}")
            plan = None
        beam = None if simulation else _beam(dataset, shape[0])
        rest_hz = None if simulation else dataset._rest_freq_hz()  # noqa: SLF001
    finally:
        if pyramid is not None:
            pyramid.close()
        hdul.close()
    fx, fy, fz = factors
    depth, height, width = shape
    cx = cube_reduce.block_centres(width, fx)
    cy = cube_reduce.block_centres(height, fy)
    cz = cube_reduce.block_centres(depth, fz)
    if simulation:
        axes = {"x": _linear_axis(dataset, 0, cx), "y": _linear_axis(dataset, 1, cy),
                "z": _linear_axis(dataset, 2, cz)}
        pixel_arcsec = None
    else:
        axes = {
            "x": _spatial_axis(dataset, 0, shape, cx),
            "y": _spatial_axis(dataset, 1, shape, cy),
            "z": _spectral_axis(dataset, shape, cz),
        }
        # Pixel scale on the sky (arcsec), for the explicit velocity-to-space scale.
        try:
            pixel_arcsec = float(dataset.spatial_pixel_scale_arcsec())
        except Exception:  # noqa: BLE001
            pixel_arcsec = None
    spacing = [_axis_spacing(dataset, i) for i in range(3)]
    rd, rh, rw = cube_reduce.reduced_shape(shape, factors)
    if file_identity(path) != identity:
        raise ValueError("The cube changed while reading its metadata: open it again.")
    return {
        "file_revision": revision,
        "full_shape": [width, height, depth],
        "factors": [fx, fy, fz],
        "reduced_shape": [rw, rh, rd],
        "default_threshold": threshold,
        "default_threshold_method": threshold_method if threshold is not None else "",
        # The noise σ of the full cube (BUNIT), for the threshold in multiples
        # of σ (issue #258), and how it was measured; null when unmeasurable
        # and for a simulation volume.
        "noise_sigma": sigma,
        "noise_sigma_method": sigma_method if sigma is not None else "",
        # A simulation volume (issue #317): no spectral axis, linear axes (X, Y,
        # Z lengths), a physical field. The headset draws its box to the
        # spacing and offers no spectral tools.
        "simulation": simulation,
        # The header's OBJECT: a VBT volume's field name (the colour bar's title).
        "object": str(dataset.header.get("OBJECT", "") or "").strip(),
        # The header's step per full-resolution voxel along x, y, z (signed,
        # CD matrix included; null when the header has none), in each axis' unit.
        "spacing": spacing,
        # The cube's spectral information for the info line (issue #258).
        "rest_frequency_hz": float(rest_hz) if rest_hz and math.isfinite(float(rest_hz)) else None,
        "rest_line": ("HI" if rest_hz and abs(float(rest_hz) - HI_REST_HZ) < 1e6 else ""),
        "beam": beam,
        "bunit": dataset.bunit or "",
        "pixel_scale_arcsec": pixel_arcsec if pixel_arcsec and math.isfinite(pixel_arcsec) else None,
        "axes": axes,
        "modes": ["mean"] if plan else list(cube_reduce.MODES),
        "encodings": list(cube_reduce.ENCODINGS),
        "source": "pyramid" if plan else "cube",
        "pyramid": plan,
    }


def worker_cube_reduced(path: str, factor_xy: int, factor_z: int, max_dim: int,
                        mode: str, encoding: str, limit: bool = True,
                        pyramid_path: str | None = None, scale: str = "auto",
                        ) -> dict[str, Any]:
    """The reduced cube as the headset's 3-D texture, plus its value range and
    its default threshold (:func:`binning_threshold`). ``limit=False`` lifts the
    headset size cap, for measurements on the backend (the validation scripts).

    With ``pyramid_path``, read from the cube's brick pyramid instead of the
    whole cube: the level :func:`app.cube_reduce.pyramid_plan` picks, binned
    further by integer factors (mean binning only: the levels are means).

    ``file_revision`` is the revision of the file actually read (as
    reduced_info gives it): taken before reading and checked after, so a file
    written again meanwhile is refused rather than answered under the
    revision of a request made earlier (or queued for a heavy slot).

    ``scale`` (issue #317): ``linear`` quantises the values, ``log`` their
    log10 (a value at or below zero is then blank), ``auto`` the log only for
    a simulation volume (:func:`is_simulation`) whose values are positive and
    span more than ``cube_reduce.AUTO_LOG_DECADES`` decades: any other cube
    stays linear under ``auto``, exactly as before #317. The reduction itself (block means or maxima) is on the values
    either way; only the quantisation of its result changes. ``range_min`` and
    ``range_max`` are values (on a log scale, the positive ones'), ``log_min``
    and ``log_max`` the log10 range the codes span (None on a linear scale)."""
    if scale not in cube_reduce.SCALES:
        raise ValueError(f"scale must be one of {cube_reduce.SCALES}, not {scale!r}.")
    identity = file_identity(path)
    dataset, hdul, data = _open_cube(path)
    pyramid = None
    try:
        simulation = is_simulation(dataset)
        shape = tuple(int(n) for n in data.shape)
        if pyramid_path:
            if mode != "mean":
                raise ValueError("A reduced cube from the brick pyramid is binned by the mean "
                                 "(its levels are means): max binning reads the whole cube.")
            pyramid = _Pyramid(pyramid_path, path, shape, factor_xy, factor_z, max_dim)
            factors = pyramid.factors
            _check_limit(shape, factors, encoding, limit)
            reduced = pyramid.reduce()
            plan = pyramid.describe()
        else:
            factors = _factors(shape, factor_xy, factor_z, max_dim, encoding, limit)
            reduced = cube_reduce.reduce_cube(data, factors, mode)
            plan = None
        own = binning_threshold(data, reduced, mode, simulation)
    finally:
        if pyramid is not None:
            pyramid.close()
        hdul.close()
    if file_identity(path) != identity:
        raise ValueError("The cube changed while it was being reduced: load it again.")
    lo, hi = cube_reduce.finite_range(reduced)
    used = cube_reduce.resolve_scale(scale, lo, hi, simulation)
    log_min = log_max = None
    nonpositive = 0
    if used == "log":
        lo, hi, nonpositive = cube_reduce.positive_range(reduced)
        if not hi > 0.0:
            raise ValueError("A log scale needs positive values: this cube has none.")
        log_min, log_max = math.log10(lo), math.log10(hi)
        raw = cube_reduce.quantise(reduced, log_min, log_max, encoding, "log")
    else:
        raw = cube_reduce.quantise(reduced, lo, hi, encoding)
    rd, rh, rw = reduced.shape
    return {
        "file_revision": hashlib.sha256(identity.encode()).hexdigest() if identity else "",
        "width": int(rw), "height": int(rh), "depth": int(rd),
        "full_shape": [shape[2], shape[1], shape[0]],
        "factors": list(factors),
        "mode": mode,
        "encoding": encoding,
        "range_min": lo, "range_max": hi,
        "scale": used,
        "log_min": log_min, "log_max": log_max,
        # Finite reduced voxels at or below zero, blank on a log scale.
        "nonpositive_voxels": int(nonpositive),
        "simulation": simulation,
        "threshold": own,
        "bunit": dataset.bunit or "",
        "source": "pyramid" if plan else "cube",
        "pyramid": plan,
        "raw_bytes": raw,
    }
