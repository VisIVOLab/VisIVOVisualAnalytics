"""Whole-cube statistics in bounded memory (issue #333).

The cube window's Inspector asks for the statistics of the whole cube when it
opens (``/v1/analysis/cube-stats``). They used to be one pass over every voxel
through a memory map: the pages a pass touches stay in the process, so a
32 GB cube grew the backend past 40 GB on macOS. Two rules now:

* A cube the desktop opens as a preview (``recommended_load_mode`` is
  ``preview_only``: its size is at or above the full-resolution threshold) is
  never read whole for these numbers. When its brick pyramid is ready, the
  minimum and maximum are the level-0 bricks' own (exact, as float32) and the
  mean is the voxel-weighted mean of a coarse level, which covers every voxel;
  the rest comes from a sample. Without a pyramid everything comes from a
  sample: bands of rows spread over the planes, read by positional reads,
  about :data:`SAMPLE_TARGET` voxels whatever the cube's size.
* An exact pass (a smaller cube, or one asked for) reads slabs with explicit
  positional reads into buffers of their own, so the process holds one slab
  at a time and nothing of the file stays mapped. A FITS file compressed as
  a whole (gzip, bzip2, xz) is read the same way through its decompressing
  stream, and a tile-compressed image tile by tile; only a zip archive is
  read whole, and only for an exact pass.

The answer says which (``source``: ``exact``, ``pyramid`` or ``sampled``) and
names the fields that are exact (``exact_fields``).

Values are those ``hdu.data`` holds (BSCALE, BZERO and BLANK applied, as
astropy does), for FITS files, VBT fields and PLUTO variables alike. The cube
is the data with its size-1 axes dropped and any leading axis beyond three
reduced to its first plane (``ScientificFitsDataset.squeeze_to_3d``); an image
is a cube one plane deep.
"""
from __future__ import annotations

import logging
import math
import os
from typing import Any, Callable

import numpy as np

logger = logging.getLogger("visivo.backend")

#: Bytes of float64 values one slab of an exact pass holds.
SLAB_BYTES = 32 * 1024 * 1024
#: About this many voxels make a sample, and the exact pass's median sample.
SAMPLE_TARGET = 4_000_000
#: A sampled band of rows is at least this many bytes of the file, so that a
#: narrow cube is not read a few bytes at a time.
BAND_BYTES = 8 * 1024
#: The pyramid level the mean of a large cube is read from: the finest with at
#: most this many voxels (64 MB of float32).
PYRAMID_MEAN_VOXELS = 1 << 24

_GOLDEN = (math.sqrt(5.0) - 1.0) / 2.0


class _Moments:
    """Count, sum, sum of squares, extremes and a numerically stable variance
    (Chan's parallel mean/M2 update, robust to a large pedestal with a small
    spread) of the finite values added in blocks."""

    def __init__(self) -> None:
        self.count = 0
        self.sum = 0.0
        self.sumsq = 0.0
        self.min = math.inf
        self.max = -math.inf
        self.mean = 0.0
        self.m2 = 0.0

    def add(self, finite: np.ndarray) -> None:
        nb = int(finite.size)
        if not nb:
            return
        # Dot products: no temporary the size of the block but one.
        self.sum += float(finite.sum())
        self.sumsq += float(np.dot(finite, finite))
        self.min = min(self.min, float(finite.min()))
        self.max = max(self.max, float(finite.max()))
        mean_b = float(finite.mean())
        dev = finite - mean_b
        m2_b = float(np.dot(dev, dev))
        del dev
        delta = mean_b - self.mean
        total = self.count + nb
        self.mean += delta * nb / total
        self.m2 += m2_b + delta * delta * self.count * nb / total
        self.count = total


def squeezed_shape(full) -> tuple[int, int, int] | None:
    """The (z, y, x) shape ``squeeze_to_3d`` gives data of shape ``full``, an
    image one plane deep; None when it is neither."""
    dims = [int(n) for n in full if int(n) != 1]
    if len(dims) == 2:
        return (1, dims[0], dims[1])
    if len(dims) >= 3:
        return tuple(dims[-3:])  # type: ignore[return-value]
    return None


class _Reader:
    """Blocks ``[za, zb) x [ya, yb) x every column`` of the cube as the values
    ``hdu.data`` holds. ``read`` makes a new array per block; ``close``
    releases the file. ``dtype`` is the values' type (after any scaling)."""

    def __init__(self, shape, read: Callable[[int, int, int, int], np.ndarray],
                 close: Callable[[], None], itemsize: int, dtype, stream: bool = False) -> None:
        self.shape = tuple(int(n) for n in shape)
        self.read = read
        self.close = close
        #: Bytes per stored sample: how much of the file a band of rows is.
        self.itemsize = int(itemsize)
        self.dtype = np.dtype(dtype)
        #: Read through a decompressing stream: any read past the start costs
        #: the decompression of everything before it, so a sample costs a pass.
        self.stream = bool(stream)


class _ByteCube:
    """A C-ordered cube of ``raw_dtype`` samples from byte ``offset`` of a
    file, read by ``fill(view, offset)``: blocks of whole planes are one
    contiguous range, a band of rows one range per plane. ``scale`` is
    astropy's own conversion of raw samples into ``hdu.data`` values (BSCALE,
    BZERO, BLANK, unsigned integers), None when the samples are the values."""

    def __init__(self, fill, offset: int, raw_dtype, shape, scale=None) -> None:
        self._fill = fill
        self.offset = int(offset)
        self.raw_dtype = np.dtype(raw_dtype)
        self.shape = tuple(int(n) for n in shape)
        self._scale = scale
        self.dtype = self.read(0, 0, 0, 0).dtype

    def read(self, za: int, zb: int, ya: int, yb: int) -> np.ndarray:
        _nz, ny, nx = self.shape
        nzb, nyb = max(0, zb - za), max(0, yb - ya)
        item = self.raw_dtype.itemsize
        buf = np.empty(nzb * nyb * nx * item, dtype=np.uint8)
        view = memoryview(buf)
        if nyb == ny:
            if len(view):
                self._fill(view, self.offset + za * ny * nx * item)
        else:
            band = nyb * nx * item
            for k in range(nzb):
                if band:
                    self._fill(view[k * band:(k + 1) * band],
                               self.offset + ((za + k) * ny + ya) * nx * item)
        out = buf.view(self.raw_dtype).reshape(nzb, nyb, nx)
        if not out.dtype.isnative:
            out = out.byteswap(inplace=True).view(out.dtype.newbyteorder("="))
        if self._scale is not None:
            out = self._scale(out)
        return out


def _seek_fill(fh):
    """``fill`` by seek and read, for a file object this reader owns alone: a
    compressed stream (whose seeks forward decompress, backwards restart) or
    a platform without ``pread``."""
    def fill(view: memoryview, offset: int) -> None:
        fh.seek(offset)
        while len(view):
            n = fh.readinto(view)
            if not n:
                raise ValueError(f"The data ends before byte {offset}: the file is truncated.")
            view = view[n:]
            offset += n
    return fill


#: Compressed FITS astropy reads through a stream it can seek: read the same
#: way here, a slab at a time, instead of decompressed whole.
_STREAMS = {"gzip": "gzip", "bzip2": "bz2", "lzma": "lzma"}


def _byte_reader(path: str, hdu_index: int) -> _Reader | None:
    """A reader of the cube's bytes, one block at a time: positional reads of
    a raw field or an uncompressed FITS image (seek and read without
    ``pread``), a stream for a FITS file compressed as a whole. None where
    neither serves (a tile-compressed image, a zip archive, an odd layout)."""
    import importlib  # noqa: PLC0415

    from astropy.io import fits  # noqa: PLC0415

    from .compute.moment import _FITS_RAW_DTYPES, _pread_into  # noqa: PLC0415
    from .volume_source import field_data_file, is_field_ref, raw_field  # noqa: PLC0415

    field = raw_field(path) if is_field_ref(path) else None
    fh = open(field_data_file(path), "rb", buffering=0)  # noqa: SIM115 - closed by the reader
    handles: list = [fh]

    def close() -> None:
        for h in reversed(handles):
            h.close()

    try:
        size = os.fstat(fh.fileno()).st_size
        if hasattr(os, "pread"):
            fd = fh.fileno()
            fill = lambda view, offset: _pread_into(fd, view, offset)  # noqa: E731
        else:
            fill = _seek_fill(fh)
        if field is not None:
            if field.offset + field.nbytes > size:
                raise ValueError(f"{field.path} is shorter than its field {field.index} needs.")
            raw = _ByteCube(fill, field.offset, field.dtype, field.shape)
            return _Reader(raw.shape, raw.read, close, raw.raw_dtype.itemsize, raw.dtype)
        try:
            # Headers only: touching the data of a scaled image converts it whole.
            hdul = fits.open(fh, memmap=False, mode="readonly", lazy_load_hdus=True)
            handles.append(hdul)
            hdu = hdul[hdu_index]
            compression = getattr(hdul._file, "compression", None)
            usable = (isinstance(hdu, (fits.PrimaryHDU, fits.ImageHDU))
                      and not isinstance(hdu, fits.CompImageHDU)
                      and getattr(hdu, "_data_offset", None) is not None
                      and hasattr(hdu, "_scale_data")
                      and int(hdu._orig_bitpix) in _FITS_RAW_DTYPES
                      and (not compression or compression in _STREAMS))
            shape = squeezed_shape(hdu.shape or ()) if usable else None
        except Exception:  # noqa: BLE001 - astropy's own reader reports its errors
            close()
            return None
        if shape is None:
            close()
            return None
        dtype = np.dtype(_FITS_RAW_DTYPES[int(hdu._orig_bitpix)])
        offset = int(hdu._data_offset)
        if compression:
            # Offsets are of the decompressed stream (astropy's).
            stream = importlib.import_module(_STREAMS[compression]).open(field_data_file(path), "rb")
            handles.append(stream)
            fill = _seek_fill(stream)
        elif offset + int(np.prod(shape)) * dtype.itemsize > size:
            close()
            return None         # a truncated file: astropy says why
        plain = hdu._orig_bzero == 0 and hdu._orig_bscale == 1 and hdu._blank is None
        raw = _ByteCube(fill, offset, dtype, shape, None if plain else hdu._scale_data)
        return _Reader(raw.shape, raw.read, close, raw.raw_dtype.itemsize, raw.dtype,
                       stream=bool(compression))
    except BaseException:
        close()
        raise


def _fallback_reader(path: str, hdu_index: int, allow_whole: bool = True) -> _Reader:
    """Where byte reads cannot serve: the brick pyramid's block reader (a
    tile-compressed image read tile by tile) when it reads the same cube,
    else astropy's own array, read whole (a zip archive)."""
    from astropy.io import fits  # noqa: PLC0415

    from .brick_pyramid import _SourceCube  # noqa: PLC0415
    from .fits_dataset import ScientificFitsDataset  # noqa: PLC0415
    from .volume_source import volume_shape  # noqa: PLC0415

    full, _ = volume_shape(path, hdu_index)
    shape = squeezed_shape(full)
    if shape is None:
        raise ValueError(f"Unsupported cube shape {tuple(full)}.")
    try:
        cube = _SourceCube(path, hdu_index, SLAB_BYTES)
    except ValueError:
        cube = None
    if cube is not None and tuple(cube.shape) == shape:
        nx = shape[2]
        return _Reader(shape, lambda za, zb, ya, yb: cube.block(za, zb, ya, yb, 0, nx,
                                                              dtype=np.float64),
                       cube.close, 8, np.float64)
    if cube is not None:
        cube.close()
    if not allow_whole:
        raise ValueError(f"{path} can only be read whole: ask for exact statistics, "
                         "or decompress it first.")
    # Not memory-mapped: astropy refuses to map scaled data.
    hdul = fits.open(path, memmap=False, mode="readonly")
    try:
        data = hdul[hdu_index].data
        if data is None:
            raise ValueError("FITS file contains no image data.")
        arr = ScientificFitsDataset.squeeze_to_3d(np.asarray(data))
        if arr.ndim == 2:
            arr = arr[np.newaxis]
    except BaseException:
        hdul.close()
        raise
    return _Reader(arr.shape, lambda za, zb, ya, yb: np.asarray(arr[za:zb, ya:yb]),
                   hdul.close, arr.dtype.itemsize, arr.dtype)


def open_reader(path: str, hdu_index: int, allow_whole: bool = True) -> _Reader:
    """The reader of the cube at ``path`` (a FITS file or a field reference).
    ``allow_whole`` False refuses a file that can only be read whole."""
    return _byte_reader(path, hdu_index) or _fallback_reader(path, hdu_index, allow_whole)


def _blocks(shape, budget_bytes: int = SLAB_BYTES):
    """``(za, zb, ya, yb)`` covering the cube in slabs of at most about
    ``budget_bytes`` of float64: whole planes, or bands of rows of one plane
    when a plane alone is larger."""
    nz, ny, nx = shape
    plane = ny * nx * 8
    if plane <= budget_bytes:
        step = max(1, budget_bytes // max(1, plane))
        for za in range(0, nz, step):
            yield za, min(nz, za + step), 0, ny
        return
    rows = max(1, budget_bytes // max(1, nx * 8))
    for z in range(nz):
        for ya in range(0, ny, rows):
            yield z, z + 1, ya, min(ny, ya + rows)


def sample_bands(shape, itemsize: int, target: int | None = None) -> list[tuple[int, int, int]]:
    """``(z, ya, yb)``: bands of whole rows spread over the cube, about
    ``target`` voxels in all. Planes are evenly spaced (stratified, each at the
    centre of its share of z); in each, the bands are evenly spaced too, with a
    phase that moves from plane to plane (golden ratio), so that the sample
    does not see the same rows in every plane. A band is at least
    :data:`BAND_BYTES` of the file. The whole cube when it is not larger than
    ``target``."""
    target = SAMPLE_TARGET if target is None else int(target)
    nz, ny, nx = (int(n) for n in shape)
    band = min(ny, max(1, -(-BAND_BYTES // max(1, nx * int(itemsize)))))
    per_plane = -(-ny // band)
    units = max(1, -(-int(target) // (band * nx)))
    if units >= nz * per_plane:
        return [(z, 0, ny) for z in range(nz)]
    pz = min(nz, max(1, round(math.sqrt(units * nz / per_plane))))
    pb = min(per_plane, max(1, -(-units // pz)))
    pz = min(nz, max(1, -(-units // pb)))
    out: list[tuple[int, int, int]] = []
    for i in range(pz):
        z = min(nz - 1, int((i + 0.5) * nz / pz))
        phase = (i * _GOLDEN) % 1.0
        starts = sorted({min(per_plane - 1, int((j + phase) * per_plane / pb)) for j in range(pb)})
        for b in starts:
            out.append((z, b * band, min(ny, (b + 1) * band)))
    return out


def _finite(values: np.ndarray) -> np.ndarray:
    """The finite values of a block, flat, as float64 (no second copy when
    all are finite)."""
    flat = np.asarray(values, dtype=np.float64).reshape(-1)
    ok = np.isfinite(flat)
    return flat if ok.all() else flat[ok]


def exact_statistics(reader: _Reader) -> dict[str, Any]:
    """One pass over every voxel, a slab at a time, plus a strided sample of
    the finite voxels for the median (all of them for a small cube)."""
    nz, ny, nx = reader.shape
    total = nz * ny * nx
    stride = max(1, total // SAMPLE_TARGET)
    acc = _Moments()
    parts: list[np.ndarray] = []
    finite_offset = 0       # running index into the concatenated finite voxels
    for za, zb, ya, yb in _blocks(reader.shape):
        finite = _finite(reader.read(za, zb, ya, yb))
        acc.add(finite)
        # Strided over the FINITE voxels (not the grid), so blank masks cannot
        # alias or starve the sample; the phase carries over blocks.
        sub = finite if stride == 1 else finite[((-finite_offset) % stride)::stride]
        if sub.size:
            parts.append(sub.copy() if stride != 1 else sub)
        finite_offset += int(finite.size)
        del finite
    sample = np.concatenate(parts) if parts else np.empty(0)
    out = _summary(acc, sample, total, int(acc.count), acc.mean, acc.sum)
    out.update(source="exact", median_exact=bool(stride == 1), sample_count=int(sample.size))
    out["exact_fields"] = ["total_count", "valid_count", "blanked_count", "min", "max",
                           "mean", "std", "rms", "sum"] + (["median"] if stride == 1 else [])
    return out


def _summary(acc: _Moments, sample: np.ndarray, total: int, valid: int, mean: float,
             vsum: float) -> dict[str, Any]:
    if acc.count == 0:
        raise ValueError("Cube has no finite voxels.")
    if sample.size:
        median = float(np.median(sample))
        mad = float(np.median(np.abs(sample - median)))
    else:
        median = mad = float("nan")
    return {
        "total_count": int(total),
        "valid_count": int(valid),
        "blanked_count": int(total - valid),
        "min": float(acc.min),
        "max": float(acc.max),
        "mean": float(mean),
        "median": median,
        "std": math.sqrt(max(0.0, acc.m2 / acc.count)),
        "rms": math.sqrt(max(0.0, acc.sumsq / acc.count)),
        "mad_sigma": float(1.4826 * mad) if math.isfinite(mad) else float("nan"),
        "sum": float(vsum),
    }


def _axis_counts(n0: int, level: int) -> np.ndarray:
    """Cube voxels behind each voxel of ``level`` along an axis of ``n0``."""
    f = 1 << int(level)
    nk = -(-int(n0) // f)
    lo = np.arange(nk, dtype=np.int64) * f
    return (np.minimum(lo + f, n0) - lo).astype(np.float64)


def pyramid_summary(pyramid_path: str, source_path: str, shape) -> dict[str, Any] | None:
    """From a ready pyramid of the cube: the extremes of its level-0 bricks and
    the mean of a coarse level, each level voxel weighted by the cube voxels
    it stands for. That mean is the cube's (to float32 rounding) when the cube
    has no blank voxel and every axis is a multiple of the level's block; a
    level is built by unweighted means of up to 8 voxels of the level below,
    so a partial block at the end of an axis, or one next to blank voxels,
    weighs slightly off (2e-4 of the mean on a 67 x 97 x 131 gradient). None
    when the pyramid does not hold this cube or has no finite voxel."""
    from .brick_pyramid import BrickPyramid  # noqa: PLC0415

    with BrickPyramid(pyramid_path, source=source_path) as pyr:
        if pyr.shape(0) != tuple(int(n) for n in shape):
            return None
        mins, maxs = pyr.stats(0)
        if not (np.isfinite(mins).any() and np.isfinite(maxs).any()):
            return None
        level = pyr.levels - 1
        for k in range(pyr.levels):
            if int(np.prod(pyr.shape(k))) <= PYRAMID_MEAN_VOXELS:
                level = k
                break
        kz, ky, kx = pyr.shape(level)
        wz, wy, wx = (_axis_counts(n0, level) for n0 in shape)
        plane_w = wy[:, None] * wx[None, :]
        step = max(1, (4 << 20) // max(1, ky * kx))
        num = den = 0.0
        for z0 in range(0, kz, step):
            z1 = min(kz, z0 + step)
            v = pyr.read_region(level, (slice(z0, z1), slice(0, ky), slice(0, kx)),
                                ghost=0, edge="nan").astype(np.float64)
            w = wz[z0:z1, None, None] * plane_w[None]
            ok = np.isfinite(v)
            num += float((v[ok] * w[ok]).sum())
            den += float(w[ok].sum())
        return {"min": float(np.nanmin(mins)), "max": float(np.nanmax(maxs)),
                "mean": num / den if den > 0 else math.nan, "level": int(level)}


def sampled_statistics(reader: _Reader, pyramid: dict[str, Any] | None = None,
                       target: int | None = None) -> dict[str, Any]:
    """Statistics from a sample of :func:`sample_bands`; with ``pyramid`` (a
    :func:`pyramid_summary`) the extremes and the mean are the pyramid's.
    Counts and the sum are scaled up from the sample's share of finite
    voxels."""
    nz, ny, nx = reader.shape
    total = nz * ny * nx
    acc = _Moments()
    parts: list[np.ndarray] = []
    seen = 0
    for z, ya, yb in sample_bands(reader.shape, reader.itemsize, target):
        block = reader.read(z, z + 1, ya, yb)
        seen += int(block.size)
        finite = _finite(block)
        acc.add(finite)
        if finite.size:
            parts.append(finite)
    if acc.count == 0:
        raise ValueError("No finite voxel in a sample of the cube: ask for exact statistics.")
    sample = np.concatenate(parts)
    valid = int(round(total * acc.count / max(1, seen)))
    mean = acc.mean
    if pyramid is not None and math.isfinite(pyramid.get("mean", math.nan)):
        mean = float(pyramid["mean"])
    out = _summary(acc, sample, total, valid, mean, mean * valid)
    out.update(median_exact=False, sample_count=int(seen))
    if pyramid is not None:
        # The pyramid holds float32: its extremes are the cube's own when the
        # values are float32 or integers of at most 16 bits.
        lossless = reader.dtype == np.float32 or (reader.dtype.kind in "iu"
                                                  and reader.dtype.itemsize <= 2)
        out.update(min=float(pyramid["min"]), max=float(pyramid["max"]), source="pyramid",
                   pyramid_level=int(pyramid["level"]),
                   exact_fields=["total_count"] + (["min", "max"] if lossless else []))
    else:
        out.update(source="sampled", exact_fields=["total_count"])
    return out


def cube_statistics(path: str, exact: bool = False) -> dict[str, Any]:
    """Whole-cube statistics of ``path`` (a FITS file or a field reference):
    an exact pass below the full-resolution threshold or when ``exact``,
    else from the ready pyramid and a sample, or a sample alone."""
    from .fits_dataset import ScientificFitsDataset  # noqa: PLC0415
    from .preview_pyramid import ready_pyramid  # noqa: PLC0415

    dataset = ScientificFitsDataset(path)
    # The data's own size too: a compressed file is smaller than its cube.
    data_bytes = int(np.prod(dataset.shape)) * abs(int(dataset.header.get("BITPIX", -32))) // 8
    large = (dataset.recommended_load_mode() == "preview_only"
             or data_bytes >= dataset._full_res_threshold_bytes())
    reader = open_reader(path, dataset.hdu_index, allow_whole=exact or not large)
    try:
        if exact or not large or reader.stream:
            # A stream compressed as a whole is decompressed to reach a sample
            # as to make a pass (and it never has a pyramid): the exact pass,
            # in the same bounded memory.
            out = exact_statistics(reader)
        else:
            summary = None
            pyr = ready_pyramid(path)
            if pyr:
                try:
                    summary = pyramid_summary(pyr, path, reader.shape)
                except Exception as exc:  # noqa: BLE001 - rebuilt or evicted since: sample
                    logger.info("[cube-stats] pyramid %s unusable (%s): sampling", pyr, exc)
            out = sampled_statistics(reader, summary)
    finally:
        reader.close()
    out["bunit"] = dataset.intensity_unit
    return out
