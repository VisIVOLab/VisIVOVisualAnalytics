"""A brick pyramid of a cube: the cube in fixed-size bricks at several
resolutions, built out of core (issue #69).

Rendering a cube larger than a GPU, or than the node's memory, means reading
only the bricks a view needs at the resolution it needs. This module writes
that structure once per cube and reads bricks from it; it is the format the
server-side out-of-core renderer (#70) and the client-side bricked renderer
(#15) share.

Format (one HDF5 file, ``FORMAT`` / ``VERSION`` in the root attributes):

* ``level0`` ... ``levelN``: groups, each with ``data`` (float32, axes z, y, x,
  chunked by the brick, NaN for blank voxels) and ``min`` / ``max`` (float32,
  one value per brick, NaN for a brick with no finite voxel). Level k+1 is
  level k halved on every axis (rounded up) by the mean of each 2x2x2 block
  over its finite voxels; the top level fits in one brick.
* root attributes: the brick size, the number of levels, the shape of level 0,
  and the source it was built from (path, HDU, size and modification time), so
  that a pyramid of a file that has since changed is recognised as stale.

Bricks are not stored with a border: :meth:`BrickPyramid.read_brick` reads the
neighbouring voxels across bricks (``ghost``), which is what interpolation at
brick edges needs, and pads with NaN outside the cube.

The builder never holds the cube: it reads the source in blocks of at most
``max_block_bytes`` and builds each level from the one below it, block by block.

Command line::

    python -m app.brick_pyramid build cube.fits [out.h5] [--brick 64] [--max-block-mb 256]
    python -m app.brick_pyramid info out.h5
"""
from __future__ import annotations

import math
import os
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

import numpy as np

FORMAT = "visivo-brick-pyramid"
VERSION = 1
DEFAULT_BRICK = 64
DEFAULT_MAX_BLOCK_BYTES = 256 * 1024 ** 2


class StalePyramid(RuntimeError):
    """The pyramid was built from a source file that has changed since."""


@dataclass(frozen=True)
class SourceSignature:
    path: str
    hdu: int
    size: int
    mtime_ns: int

    @classmethod
    def of(cls, path: str | os.PathLike, hdu: int) -> "SourceSignature":
        st = os.stat(path)
        return cls(str(Path(path).resolve()), int(hdu), int(st.st_size), int(st.st_mtime_ns))


def level_shapes(shape: tuple[int, int, int], brick: int) -> list[tuple[int, int, int]]:
    """The shape of every level, level 0 first: halved (rounded up) until the
    level fits in one brick."""
    shapes = [tuple(int(n) for n in shape)]
    while max(shapes[-1]) > brick:
        shapes.append(tuple((n + 1) // 2 for n in shapes[-1]))
    return shapes


def default_pyramid_path(source: str | os.PathLike, hdu: int = 0) -> Path:
    """Where the pyramid of ``source`` goes unless told otherwise: a hidden
    directory of the workspace (``VISIVO_PYRAMID_DIR`` overrides it), named from
    the source's path, HDU, size and modification time, so a changed file gets
    a new pyramid rather than a stale one."""
    import hashlib  # noqa: PLC0415

    sig = SourceSignature.of(source, hdu)
    root = os.environ.get("VISIVO_PYRAMID_DIR")
    if root:
        directory = Path(root)
    else:
        from .dependencies import get_exports_dir  # noqa: PLC0415
        directory = get_exports_dir() / ".pyramids"
    key = hashlib.sha256(f"{sig.path}|{sig.hdu}|{sig.size}|{sig.mtime_ns}".encode()).hexdigest()[:16]
    return directory / f"{Path(sig.path).stem}-{key}.pyr.h5"


# ── building ────────────────────────────────────────────────────────────────


def _block_extent(shape: tuple[int, int, int], brick: int, itemsize: int,
                  max_bytes: int) -> tuple[int, int, int]:
    """The (z, y, x) size of the blocks a level is processed in: one brick deep,
    and as many whole bricks in y and x as fit in ``max_bytes`` (at least one).
    Blocks are brick-aligned, so every write fills whole chunks."""
    nz, ny, nx = shape
    bz = min(brick, nz)
    per_brick = bz * brick * brick * itemsize
    budget = max(1, max_bytes // per_brick)          # bricks per block
    bx_bricks = min(math.ceil(nx / brick), budget)
    by_bricks = min(math.ceil(ny / brick), max(1, budget // bx_bricks))
    return bz, min(ny, by_bricks * brick), min(nx, bx_bricks * brick)


def _brick_stats(block: np.ndarray, brick: int) -> tuple[np.ndarray, np.ndarray]:
    """Per-brick nanmin and nanmax of a brick-aligned block (edge bricks may be
    partial); NaN for a brick with no finite voxel. Brick by brick, so the
    temporaries are one brick, never a padded copy of the block."""
    grid = tuple(math.ceil(n / brick) for n in block.shape)
    lo = np.full(grid, np.nan, dtype=np.float32)
    hi = np.full(grid, np.nan, dtype=np.float32)
    for gz, gy, gx in np.ndindex(grid):
        tile = block[gz * brick:(gz + 1) * brick, gy * brick:(gy + 1) * brick,
                     gx * brick:(gx + 1) * brick]
        values = tile[np.isfinite(tile)]
        if values.size:
            lo[gz, gy, gx], hi[gz, gy, gx] = values.min(), values.max()
    return lo, hi


def _halve(block: np.ndarray) -> np.ndarray:
    """Mean of each 2x2x2 block over its finite voxels (NaN where none is
    finite); an odd edge is padded with NaN, so it averages what exists."""
    nz, ny, nx = block.shape
    pad = ((0, nz % 2), (0, ny % 2), (0, nx % 2))
    if any(p[1] for p in pad):
        block = np.pad(block, pad, constant_values=np.nan)
    cells = block.reshape(block.shape[0] // 2, 2, block.shape[1] // 2, 2, block.shape[2] // 2, 2)
    finite = np.isfinite(cells)
    count = finite.sum(axis=(1, 3, 5))
    total = np.where(finite, cells, 0.0).sum(axis=(1, 3, 5), dtype=np.float64)
    with np.errstate(invalid="ignore", divide="ignore"):
        out = np.where(count > 0, total / np.maximum(count, 1), np.nan)
    return out.astype(np.float32)


class _SourceCube:
    """Blocks of a FITS cube as float32 (z, y, x), read without holding the cube.

    Uncompressed data is memory-mapped raw (astropy refuses to map scaled data);
    a tile-compressed image is read through ``section``, which decompresses
    only the tiles a block touches. Both are scaled block by block, in double
    precision before the final float32 (so that unsigned 32-bit data, stored
    with BZERO = 2**31, keeps its values): BLANK becomes NaN, then BSCALE and
    BZERO apply. Two inputs cannot be read in bounded memory and are refused: a
    whole-file compressed FITS (``.fits.gz``), which astropy decompresses whole,
    and a tile-compressed image whose tiles are larger than ``max_bytes``.
    Degenerate axes are dropped; of a fourth axis longer than one only the first
    plane is used, as everywhere else in the backend
    (``ScientificFitsDataset.squeeze_to_3d``)."""

    def __init__(self, path: str, hdu: int, max_bytes: int) -> None:
        import logging  # noqa: PLC0415
        from astropy.io import fits  # noqa: PLC0415

        # A file compressed as a whole is recognised by its first bytes, before
        # astropy opens it: it would decompress the whole member to do so.
        with open(path, "rb") as fh:
            magic = fh.read(4)
        if magic[:2] == b"\x1f\x8b" or magic[:3] == b"BZh" or magic == b"PK\x03\x04":
            raise ValueError(f"{path} is compressed as a whole, which can only be read whole: "
                             "decompress it first.")
        self._hdul = fits.open(path, memmap=True, mode="readonly", do_not_scale_image_data=True)
        try:
            if getattr(self._hdul._file, "compression", None):
                raise ValueError(f"{path} is compressed as a whole ({self._hdul._file.compression}), "
                                 "which can only be read whole: decompress it first.")
            item = self._hdul[hdu]
            full = tuple(int(n) for n in (item.shape or ()))
            kept = [i for i, n in enumerate(full) if n > 1]
            if len(kept) < 3:
                raise ValueError(f"HDU {hdu} of {path} is not a cube (shape {full}).")
            if len(kept) > 3:
                logging.getLogger(__name__).warning(
                    "brick pyramid of %s: keeping the first plane of the leading axes %s",
                    path, [full[i] for i in kept[:-3]])
            self._axes = kept[-3:]
            self._ndim = len(full)
            self.shape = tuple(full[i] for i in self._axes)
            header = item.header
            if isinstance(item, fits.CompImageHDU):
                tile = [int(n) for n in (getattr(item, "tile_shape", None) or ())]
                if tile and int(np.prod(tile)) * 8 > max_bytes:
                    raise ValueError(f"the compression tiles of {path} ({tile}) are larger than the "
                                     f"block budget ({max_bytes} bytes): rewrite it with smaller tiles.")
                self._read = item.section
                # Tiles along the data's axes, numpy order (astropy's tile_shape).
                self._tile = tuple(tile) if tile else None
                integer = int(header.get("BITPIX", -32)) > 0
            else:
                if item.data is None:
                    raise ValueError(f"HDU {hdu} of {path} holds no image data.")
                self._read = item.data
                self._tile = None
                integer = item.data.dtype.kind in "iu"
            bscale, bzero = float(header.get("BSCALE", 1.0)), float(header.get("BZERO", 0.0))
            blank = header.get("BLANK") if integer else None
            self._scale = (bscale, bzero, None if blank is None else int(blank))
        except BaseException:
            self._hdul.close()
            raise

    def _raw(self, ranges: tuple[tuple[int, int], ...]) -> np.ndarray:
        """The stored (unscaled) values of a (z, y, x) range."""
        index: list = [0] * self._ndim
        for axis, (a, b) in zip(self._axes, ranges):
            index[axis] = slice(a, b)
        if self._tile is None:
            return np.asarray(self._read[tuple(index)])
        # A compressed section decompresses every tile it touches into one
        # buffer: read one tile's intersection at a time, so that no more than
        # one tile is decompressed at once whatever the block.
        tiles = [self._tile[axis] for axis in self._axes]
        pieces = [[(s, min(b, (s // t + 1) * t)) for s in [a] + list(range((a // t + 1) * t, b, t))]
                  for (a, b), t in zip(ranges, tiles)]
        out = None
        for (za, zb), (ya, yb), (xa, xb) in ((z, y, x) for z in pieces[0] for y in pieces[1] for x in pieces[2]):
            sub: list = list(index)
            for axis, (a, b) in zip(self._axes, ((za, zb), (ya, yb), (xa, xb))):
                sub[axis] = slice(a, b)
            part = np.asarray(self._read[tuple(sub)])
            if out is None:
                out = np.empty(tuple(b - a for a, b in ranges), dtype=part.dtype)
            out[za - ranges[0][0]:zb - ranges[0][0], ya - ranges[1][0]:yb - ranges[1][0],
                xa - ranges[2][0]:xb - ranges[2][0]] = part
        return out

    def block(self, z0: int, z1: int, y0: int, y1: int, x0: int, x1: int) -> np.ndarray:
        raw = self._raw(((z0, z1), (y0, y1), (x0, x1)))
        bscale, bzero, blank = self._scale
        if bscale == 1.0 and bzero == 0.0 and blank is None:
            return raw.astype(np.float32, copy=False)
        values = raw
        if (raw.dtype.kind == "i" and bscale == 1.0
                and bzero == float(2 ** (8 * raw.dtype.itemsize - 1))):
            # Unsigned data stored as signed (BZERO = 2**(bits-1)): restore it
            # with integer arithmetic, exact at every width, uint64 included.
            unsigned = raw.dtype.newbyteorder("=").str.replace("i", "u")
            values = raw.astype(raw.dtype.newbyteorder("="), copy=False).view(unsigned) \
                ^ np.array(1 << (8 * raw.dtype.itemsize - 1), dtype=unsigned)
            # Straight to float32: through float64 a uint64 would be rounded twice.
            out = values.astype(np.float32)
            if blank is not None:
                out[raw == blank] = np.nan
            return out
        else:
            # Double precision before the float32 result.
            out = raw.astype(np.float64)
            if bscale != 1.0 or bzero != 0.0:
                out = out * bscale + bzero
        if blank is not None and raw.dtype.kind in "iu":
            out[raw == blank] = np.nan
        return out.astype(np.float32)

    def close(self) -> None:
        self._hdul.close()


def build_pyramid(source: str | os.PathLike, out: str | os.PathLike | None = None, *,
                  hdu: int = 0, brick: int = DEFAULT_BRICK,
                  max_block_bytes: int = DEFAULT_MAX_BLOCK_BYTES,
                  progress: Callable[[str, float], None] | None = None,
                  should_stop: Callable[[], None] | None = None) -> Path:
    """Write the brick pyramid of ``source`` (a FITS cube) and return its path.

    The cube is read in brick-aligned blocks of at most ``max_block_bytes`` of
    float32 data; each level after the first is built from the one below in the
    same way, so the memory used does not grow with the cube: it stays within a
    small multiple of ``max_block_bytes`` (the block, its conversion and the
    halving's temporaries). ``max_block_bytes`` must hold at least what one brick
    of a higher level reads from the level below, ``8 * brick**3 * 4`` bytes.
    The file is written under a temporary name and renamed into place when
    complete, so a reader never sees a partial pyramid. ``progress(stage,
    fraction)`` is called as blocks are written; ``should_stop()`` may raise to
    abandon the build (the temporary file is then removed).
    """
    import h5py  # noqa: PLC0415

    if brick < 8 or brick & (brick - 1):
        raise ValueError("brick must be a power of two, at least 8")
    if max_block_bytes < 8 * brick ** 3 * 4:
        raise ValueError(f"max_block_bytes must be at least {8 * brick ** 3 * 4} for brick {brick}")
    sig = SourceSignature.of(source, hdu)
    out_path = Path(out) if out is not None else default_pyramid_path(source, hdu)
    if out_path.resolve() == Path(sig.path) or (out_path.exists() and out_path.samefile(sig.path)):
        raise ValueError("the pyramid cannot be written over its source")
    out_path.parent.mkdir(parents=True, exist_ok=True)

    cube = _SourceCube(sig.path, hdu, max_block_bytes)
    fd, tmp_name = tempfile.mkstemp(prefix=out_path.name + ".", suffix=".part", dir=out_path.parent)
    os.close(fd)
    try:
        shapes = level_shapes(cube.shape, brick)
        with h5py.File(tmp_name, "w") as h5:
            h5.attrs.update({
                "format": FORMAT, "version": VERSION, "brick": brick, "levels": len(shapes),
                "shape": np.asarray(shapes[0], dtype=np.int64), "axis_order": "zyx",
                "downsample": "mean of 2x2x2 over finite voxels",
                "source_path": sig.path, "source_hdu": sig.hdu,
                "source_size": sig.size, "source_mtime_ns": sig.mtime_ns,
            })
            for level, shape in enumerate(shapes):
                group = h5.create_group(f"level{level}")
                group.create_dataset("data", shape=shape, dtype="f4",
                                     chunks=tuple(min(brick, n) for n in shape),
                                     fillvalue=np.nan)
                grid = tuple(math.ceil(n / brick) for n in shape)
                group.create_dataset("min", shape=grid, dtype="f4", fillvalue=np.nan)
                group.create_dataset("max", shape=grid, dtype="f4", fillvalue=np.nan)

            read_level0 = cube.block

            def read_below(level):
                data = h5[f"level{level - 1}/data"]
                return lambda z0, z1, y0, y1, x0, x1: _halve(
                    data[2 * z0:2 * z1, 2 * y0:2 * y1, 2 * x0:2 * x1])

            for level, shape in enumerate(shapes):
                read = read_level0 if level == 0 else read_below(level)
                # A block of this level reads 8x its size from the level below.
                itemsize = 4 if level == 0 else 4 * 8
                bz, by, bx = _block_extent(shape, brick, itemsize, max_block_bytes)
                group = h5[f"level{level}"]
                nz, ny, nx = shape
                total = math.ceil(nz / bz) * math.ceil(ny / by) * math.ceil(nx / bx)
                done = 0
                for z0 in range(0, nz, bz):
                    for y0 in range(0, ny, by):
                        for x0 in range(0, nx, bx):
                            if should_stop is not None:
                                should_stop()
                            z1, y1, x1 = min(nz, z0 + bz), min(ny, y0 + by), min(nx, x0 + bx)
                            block = read(z0, z1, y0, y1, x0, x1)
                            group["data"][z0:z1, y0:y1, x0:x1] = block
                            lo, hi = _brick_stats(block, brick)
                            gz, gy, gx = z0 // brick, y0 // brick, x0 // brick
                            group["min"][gz:gz + lo.shape[0], gy:gy + lo.shape[1], gx:gx + lo.shape[2]] = lo
                            group["max"][gz:gz + hi.shape[0], gy:gy + hi.shape[1], gx:gx + hi.shape[2]] = hi
                            done += 1
                            if progress is not None:
                                progress(f"level{level}", done / total)
        os.replace(tmp_name, out_path)
    except BaseException:
        Path(tmp_name).unlink(missing_ok=True)
        raise
    finally:
        cube.close()
    return out_path


# ── reading ─────────────────────────────────────────────────────────────────


class BrickPyramid:
    """A pyramid opened for reading. ``source``, when given, must be the file the
    pyramid was built from, unchanged (:class:`StalePyramid` otherwise)."""

    def __init__(self, path: str | os.PathLike, source: str | os.PathLike | None = None) -> None:
        import h5py  # noqa: PLC0415

        self.path = Path(path)
        self._h5 = h5py.File(self.path, "r")
        try:
            attrs = self._h5.attrs
            if attrs.get("format") != FORMAT:
                raise ValueError(f"{self.path} is not a brick pyramid.")
            if int(attrs.get("version", 0)) != VERSION:
                raise ValueError(f"{self.path} has format version {attrs.get('version')}, "
                                 f"this reader knows {VERSION}.")
            self.brick = int(attrs["brick"])
            self.levels = int(attrs["levels"])
            self.source = SourceSignature(str(attrs["source_path"]), int(attrs["source_hdu"]),
                                          int(attrs["source_size"]), int(attrs["source_mtime_ns"]))
            if source is not None:
                current = SourceSignature.of(source, self.source.hdu)
                if current.path != self.source.path:
                    raise StalePyramid(f"{self.path} was built from {self.source.path}, not {current.path}.")
                if (current.size, current.mtime_ns) != (self.source.size, self.source.mtime_ns):
                    raise StalePyramid(f"{source} has changed since {self.path} was built.")
        except BaseException:
            self._h5.close()
            raise

    def close(self) -> None:
        self._h5.close()

    def __enter__(self) -> "BrickPyramid":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def shape(self, level: int) -> tuple[int, int, int]:
        return tuple(int(n) for n in self._h5[f"level{level}/data"].shape)

    def grid(self, level: int) -> tuple[int, int, int]:
        """The number of bricks along z, y and x at ``level``."""
        return tuple(int(n) for n in self._h5[f"level{level}/min"].shape)

    def stats(self, level: int) -> tuple[np.ndarray, np.ndarray]:
        """Per-brick minimum and maximum of the whole of ``level`` (NaN for an
        empty brick): 8 bytes per brick, about 8 MB for a 1 TB cube in 64-voxel
        bricks. :meth:`brick_stats` reads one brick's."""
        return self._h5[f"level{level}/min"][...], self._h5[f"level{level}/max"][...]

    def brick_stats(self, level: int, bz: int, by: int, bx: int) -> tuple[float, float]:
        """Minimum and maximum of one brick (NaN for an empty brick)."""
        return (float(self._h5[f"level{level}/min"][bz, by, bx]),
                float(self._h5[f"level{level}/max"][bz, by, bx]))

    def read_brick(self, level: int, bz: int, by: int, bx: int, ghost: int = 1) -> np.ndarray:
        """Brick (bz, by, bx) of ``level`` with ``ghost`` voxels of its neighbours
        on every side, always ``brick + 2*ghost`` voxels per axis: what lies
        outside the cube (the border, and the part of an edge brick beyond the
        cube) is NaN."""
        if not 0 <= level < self.levels:
            raise IndexError(f"level {level} outside 0..{self.levels - 1}")
        grid = self.grid(level)
        if not all(0 <= i < n for i, n in zip((bz, by, bx), grid)):
            raise IndexError(f"brick {(bz, by, bx)} outside the grid {grid} of level {level}")
        b, g = self.brick, int(ghost)
        data = self._h5[f"level{level}/data"]
        out = np.full((b + 2 * g,) * 3, np.nan, dtype=np.float32)
        lo = [i * b - g for i in (bz, by, bx)]
        src = [slice(max(0, l), min(n, l + b + 2 * g)) for l, n in zip(lo, data.shape)]
        dst = [slice(s.start - l, s.stop - l) for s, l in zip(src, lo)]
        out[tuple(dst)] = data[tuple(src)]
        return out


def main(argv: list[str] | None = None) -> int:
    import argparse  # noqa: PLC0415
    import time  # noqa: PLC0415

    ap = argparse.ArgumentParser(prog="python -m app.brick_pyramid", description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="build the pyramid of a FITS cube")
    b.add_argument("source")
    b.add_argument("out", nargs="?")
    b.add_argument("--hdu", type=int, default=0)
    b.add_argument("--brick", type=int, default=DEFAULT_BRICK)
    b.add_argument("--max-block-mb", type=float, default=DEFAULT_MAX_BLOCK_BYTES / 1024 ** 2)
    i = sub.add_parser("info", help="describe a pyramid")
    i.add_argument("path")
    args = ap.parse_args(argv)

    if args.cmd == "build":
        start = time.perf_counter()
        last = [""]

        def progress(stage: str, fraction: float) -> None:
            if stage != last[0] or fraction >= 1.0:
                print(f"{stage}: {fraction:.0%}", flush=True)
                last[0] = stage

        path = build_pyramid(args.source, args.out, hdu=args.hdu, brick=args.brick,
                             max_block_bytes=int(args.max_block_mb * 1024 ** 2), progress=progress)
        print(f"{path} ({path.stat().st_size / 1024 ** 2:.0f} MB) in {time.perf_counter() - start:.1f} s")
        return 0
    with BrickPyramid(args.path) as p:
        print(f"{p.path}: brick {p.brick}, {p.levels} levels, source {p.source.path} (HDU {p.source.hdu})")
        for level in range(p.levels):
            print(f"  level{level}: shape {p.shape(level)}, bricks {p.grid(level)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
