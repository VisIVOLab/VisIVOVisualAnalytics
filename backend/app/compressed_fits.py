"""Open compressed FITS as if it were not (issue #57).

Archives distribute FITS two compressed ways, and neither can be read the way
every reader of this backend reads a FITS file, by memory-mapping it:

* **gzip** (``.fits.gz``) compresses the file as a whole. There is no random
  access: astropy reading a plane of a gzipped cube decompresses the stream up
  to that plane, every time, and a memory map is impossible.
* **Tile compression** (fpack, ``.fz``; also plain ``.fits`` names) stores each
  image as a binary table of compressed tiles (``ZIMAGE = T``). astropy reads
  it through ``CompImageHDU``, tile by tile, but it has no memory map either,
  and the readers that map the data raw would read compressed bytes as pixels.

So a compressed file is decompressed **once**, into the workspace cache, as an
ordinary FITS file (:func:`materialize`), and every reader — previews, slices,
moments, the brick pyramid, the render processes, SODA — reads that file as it
reads any other. The cache is keyed by the source's resolved path, size and
modification time, so a changed source is decompressed again; the decompressed
name is the source's without its compression suffix (``cube.fits.gz`` →
``cube.fits``), so products derived from it are named as from the uncompressed
file. The original path stays what the user sees: the session keeps it as the
dataset's ``source_path`` and :func:`source_of` maps a cache file back to it
for provenance.

Tile-compressed files are decompressed even when small. A direct read would
only pay off for files small enough that decompressing them costs
milliseconds, and it would need every reader that maps raw bytes to learn
``CompImageHDU``; one path for every size is the one that cannot be wrong.

The cache:

* lives in the workspace (``<exports>/.decompressed``, hidden from the
  Workspace listing), or in ``VISIVO_DECOMPRESS_CACHE_DIR``;
* is bounded by ``VISIVO_DECOMPRESS_CACHE_MAX_BYTES`` (100 GiB by default,
  ``0`` for no limit): the least recently used entries no session holds open
  are removed to make room, and a file that cannot fit is refused by name;
* refuses up front a decompression the disk cannot hold, and checks again
  while writing, so a full disk ends in a message rather than a short file;
* serialises two opens of the same file (a lock per entry, across threads and
  processes), so the second one finds the first one's result.

A decompression reports its progress and polls a stop callback between chunks
(the ``/v1/tasks/decompress`` task uses both); an interrupted one leaves no
entry behind.
"""
from __future__ import annotations

import contextlib
import errno
import gzip
import hashlib
import json
import logging
import os
import shutil
import threading
import time
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterator

import numpy as np

logger = logging.getLogger("visivo.backend")

GZIP_MAGIC = b"\x1f\x8b"
#: Names that announce a compressed FITS. A tile-compressed file may also be
#: called ``.fits``; it is recognised by its content (:func:`compression_of`).
COMPRESSED_FITS_SUFFIXES = (".fits.gz", ".fit.gz", ".fts.gz", ".fz")

_BLOCK = 2880
_CHUNK_BYTES = 16 * 1024 * 1024        # one gzip read / one tile-decoding block
_SPACE_CHECK_BYTES = 1024 ** 3         # re-check the free space every GiB written
_FREE_MARGIN_BYTES = 1024 ** 3         # never fill the disk to the last GiB
_DEFAULT_MAX_BYTES = 100 * 1024 ** 3
_MANIFEST = "source.json"

#: The cache directory; set by app.dependencies to ``<exports>/.decompressed``
#: (the workspace is known there). ``VISIVO_DECOMPRESS_CACHE_DIR`` wins.
cache_dir_provider: Callable[[], Path] | None = None
#: The paths open datasets read; set by app.dependencies from the session
#: registry, so eviction never removes a file a session still uses.
in_use_provider: Callable[[], set[str]] | None = None


class CompressedFitsError(ValueError):
    """A compressed file that cannot be decompressed; the message is written
    for the user (what is wrong, with which file, what to do)."""


class DecompressionCancelled(Exception):
    """The stop callback asked to end the decompression; nothing is cached."""


@dataclass(frozen=True)
class Materialized:
    """The ordinary FITS file a compressed one was decompressed to."""

    path: Path            # the decompressed file in the cache
    source: Path          # the compressed file, as the user named it
    compression: str      # "gzip" or "tile"
    cache_hit: bool
    seconds: float        # spent decompressing (0 on a hit)
    source_bytes: int
    bytes: int            # of the decompressed file


# ── What is it ───────────────────────────────────────────────────────────────


def has_compressed_name(path: str | Path) -> bool:
    """Whether the name announces a compressed FITS (``.fits.gz``, ``.fz``…)."""
    return str(path).lower().endswith(COMPRESSED_FITS_SUFFIXES)


def _image_is_tile_compressed(path: str | Path) -> bool:
    """Whether the HDU the backend reads as the image (``image_hdu_index``) is
    tile-compressed. Headers only, up to that HDU."""
    from astropy.io import fits  # noqa: PLC0415

    from .fits_dataset import image_hdu_index  # noqa: PLC0415
    try:
        with fits.open(path, memmap=False, lazy_load_hdus=True) as hdul:
            return isinstance(hdul[image_hdu_index(hdul)], fits.CompImageHDU)
    except Exception:  # noqa: BLE001 — not readable as FITS: not ours to say why
        return False


@dataclass(frozen=True)
class HeaderOnlyHdu:
    """An HDU as far as :func:`app.fits_dataset.image_hdu_index` and the
    classifier look at one: its header, and whether astropy calls it an image."""

    header: object        # astropy.io.fits.Header
    is_image: bool


def _hdu_data_bytes(header) -> int:
    """The unpadded data size an HDU header declares (random groups too)."""
    naxis = int(header.get("NAXIS", 0))
    if naxis <= 0:
        return 0
    axes = [int(header.get(f"NAXIS{k}", 0)) for k in range(1, naxis + 1)]
    if header.get("GROUPS") is True and axes and axes[0] == 0:
        axes = axes[1:]   # random groups: NAXIS1 = 0 is a marker, not a length
    count = 1
    for n in axes:
        count *= n
    return (abs(int(header.get("BITPIX", 8))) // 8 * int(header.get("GCOUNT", 1))
            * (int(header.get("PCOUNT", 0)) + count))


def _has_two_long_axes(header) -> bool:
    """The rule of :func:`app.fits_dataset.image_hdu_index` for an image HDU:
    at least two axes longer than one."""
    naxis = int(header.get("NAXIS", 0))
    return naxis >= 2 and sum(int(header.get(f"NAXIS{k}", 1)) > 1
                              for k in range(1, naxis + 1)) >= 2


# A FITS header is a few blocks; 1000 blocks (36000 cards, 2.9 MB) is far more
# than any real one. Without a cap, a crafted stream with no END card is read
# into memory whole (a 4 MB .fits.gz decompresses to gigabytes).
_MAX_HEADER_BLOCKS = 1000


def gzip_image_headers(path: str | Path,
                       max_skip_bytes: int | None = None) -> list[HeaderOnlyHdu] | None:
    """The HDU headers of a gzipped FITS, read off the gzip stream up to the
    HDU :func:`~app.fits_dataset.image_hdu_index` picks (all of them when none
    qualifies). None when the stream does not hold a FITS this can read
    (damaged, not FITS, or a gzipped fpack file, whose image headers are
    tables): the caller then asks astropy.

    Why not astropy: ``fits.open`` on a ``.gz`` seeks past the first HDU's
    data to find the next header, and a gzip stream can only seek by
    decompressing; on a 2 GB cube that is the whole file, about 5 s, to read
    one header. Here a primary image costs one header block.

    ``max_skip_bytes`` bounds the HDU data decompressed to reach later
    headers (the file browser's preview passes it): past it the headers read
    so far are returned. None keeps reading to the image HDU."""
    from astropy.io import fits  # noqa: PLC0415

    hdus: list[HeaderOnlyHdu] = []
    skipped = 0
    try:
        with gzip.open(path, "rb") as stream:
            while True:
                raw = bytearray()
                ended = False
                blocks = 0
                while not ended:
                    blocks += 1
                    if blocks > _MAX_HEADER_BLOCKS:
                        return None   # no END card in sight: not a FITS header
                    block = stream.read(_BLOCK)
                    if len(block) < _BLOCK:
                        # The end of the stream: after the last HDU, or a
                        # header cut short (astropy then says what is wrong).
                        return hdus if hdus and not raw else None
                    raw += block
                    ended = any(block[i:i + 3] == b"END" and not block[i + 3:i + 80].strip()
                                for i in range(0, _BLOCK, 80))
                text = raw.decode("ascii", errors="replace")
                if not hdus and not text.startswith("SIMPLE  ="):
                    return None
                if hdus and not text.startswith("XTENSION="):
                    return hdus   # trailing bytes after the last HDU
                header = fits.Header.fromstring(text)
                if header.get("ZIMAGE") is True:
                    return None
                xtension = str(header.get("XTENSION", "")).strip().upper()
                is_image = (header.get("GROUPS") is not True) if not hdus else xtension == "IMAGE"
                hdus.append(HeaderOnlyHdu(header, is_image))
                if is_image and _has_two_long_axes(header):
                    return hdus   # the first that qualifies is the one picked
                skip = _padded(_hdu_data_bytes(header))
                if skip:
                    skipped += skip
                    if max_skip_bytes is not None and skipped > max_skip_bytes:
                        return hdus
                    stream.seek(skip, os.SEEK_CUR)
    except (OSError, EOFError, zlib.error, ValueError):
        return None


def compression_of(path: str | Path) -> str | None:
    """``"gzip"``, ``"tile"`` or None (an ordinary file, or not FITS at all).

    gzip from its magic bytes; tile compression from the image HDU's type, so
    a tile-compressed file named ``.fits`` is recognised too."""
    try:
        with open(path, "rb") as f:
            head = f.read(9)
    except OSError:
        return None
    if head[:2] == GZIP_MAGIC:
        return "gzip"
    if head == b"SIMPLE  =" and _image_is_tile_compressed(path):
        return "tile"
    return None


# ── The cache ────────────────────────────────────────────────────────────────


def cache_dir() -> Path:
    explicit = os.environ.get("VISIVO_DECOMPRESS_CACHE_DIR", "").strip()
    if explicit:
        return Path(explicit).expanduser()
    if cache_dir_provider is not None:
        return Path(cache_dir_provider())
    exports = os.environ.get("VISIVO_EXPORTS_DIR", str(Path.home() / ".visivo" / "exports"))
    return Path(exports) / ".decompressed"


def cache_max_bytes() -> int:
    """``VISIVO_DECOMPRESS_CACHE_MAX_BYTES``; 0 means no limit."""
    raw = os.environ.get("VISIVO_DECOMPRESS_CACHE_MAX_BYTES", "").strip()
    try:
        return max(0, int(raw)) if raw else _DEFAULT_MAX_BYTES
    except ValueError:
        logger.warning("[decompress] VISIVO_DECOMPRESS_CACHE_MAX_BYTES=%r is not a number; "
                       "using %d", raw, _DEFAULT_MAX_BYTES)
        return _DEFAULT_MAX_BYTES


def _decompressed_name(source: Path) -> str:
    """``cube.fits.gz`` → ``cube.fits``; ``img.fz`` → ``img.fits``;
    ``cube.fits.fz.gz`` (a gzipped fpack file) → ``cube.fits``."""
    name = source.name
    lower = name.lower()
    for suffix in (".gz", ".fz"):   # in this order: .fz.gz loses both
        if lower.endswith(suffix):
            name, lower = name[: -len(suffix)], lower[: -len(suffix)]
    if not lower.endswith((".fits", ".fit", ".fts")):
        name += ".fits"
    return name


def _key(source: Path, stat: os.stat_result) -> str:
    text = f"{source}\n{stat.st_size}\n{stat.st_mtime_ns}"
    return hashlib.sha256(text.encode("utf-8", "surrogateescape")).hexdigest()[:24]


def source_of(path: str | Path) -> str | None:
    """The compressed file a cache file was decompressed from, or None when
    ``path`` is not a file of the cache."""
    try:
        path = Path(path)
        manifest = path.parent / _MANIFEST
        if path.parent.parent.resolve() != cache_dir().resolve() or not manifest.is_file():
            return None
        return str(json.loads(manifest.read_text())["source"])
    except (OSError, ValueError, KeyError, TypeError):
        return None


def _dir_bytes(directory: Path) -> int:
    total = 0
    for child in directory.iterdir():
        try:
            total += child.stat().st_size
        except OSError:
            continue
    return total


# Entry locks: a threading lock for the threads of this process, plus a file
# lock (where the platform has one) for other processes on the same cache.
try:
    import fcntl as _fcntl  # noqa: F401
    _HAVE_FILE_LOCK = True
except ImportError:
    _HAVE_FILE_LOCK = False
_THREAD_LOCKS: dict[str, threading.Lock] = {}
_THREAD_LOCKS_GUARD = threading.Lock()


def _thread_lock(key: str) -> threading.Lock:
    with _THREAD_LOCKS_GUARD:
        return _THREAD_LOCKS.setdefault(key, threading.Lock())


@contextlib.contextmanager
def _entry_lock(key: str, should_stop: Callable[[], bool] | None = None,
                blocking: bool = True) -> Iterator[bool]:
    """Hold the entry's lock; yields False when ``blocking`` is False and the
    lock is taken. While waiting, ``should_stop`` is polled, so a cancelled
    open does not wait for somebody else's decompression to end."""
    lock = _thread_lock(key)
    while not lock.acquire(timeout=0.25 if blocking else 0):
        if not blocking:
            yield False
            return
        if should_stop is not None and should_stop():
            raise DecompressionCancelled("Cancelled while waiting for another decompression.")
    handle = None
    try:
        try:
            import fcntl  # noqa: PLC0415 — POSIX only; Windows keeps the thread lock
        except ImportError:
            fcntl = None
        if fcntl is not None:
            directory = cache_dir()
            directory.mkdir(parents=True, exist_ok=True)
            handle = open(directory / f"{key}.lock", "a+b")  # noqa: SIM115 — closed below
            while True:
                try:
                    fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except BlockingIOError:
                    if not blocking:
                        yield False
                        return
                    if should_stop is not None and should_stop():
                        raise DecompressionCancelled(
                            "Cancelled while waiting for another decompression.") from None
                    time.sleep(0.25)
        yield True
    finally:
        if handle is not None:
            handle.close()   # releases the file lock
        lock.release()


def _entries() -> list[tuple[float, int, Path]]:
    """``(last used, bytes, directory)`` of every cache entry."""
    root = cache_dir()
    if not root.is_dir():
        return []
    out = []
    for child in root.iterdir():
        if not child.is_dir():
            continue
        manifest = child / _MANIFEST
        try:
            used = manifest.stat().st_mtime if manifest.exists() else child.stat().st_mtime
            out.append((used, _dir_bytes(child), child))
        except OSError:
            continue
    return out


def _in_use() -> set[str]:
    if in_use_provider is None:
        return set()
    try:
        return {str(Path(p).resolve()) for p in in_use_provider()}
    except Exception:  # noqa: BLE001 — never let bookkeeping break an open
        logger.debug("[decompress] in-use lookup failed", exc_info=True)
        return set()


#: An entry used this recently is never evicted: it covers the moment between
#: a decompression and the session registering the dataset, and readers that
#: hold the path without a session (a SODA job, a render process starting).
_EVICT_GRACE_SECONDS = 600.0

# Space promised to decompressions under way in this process, by entry key:
# admissions are serialised, so two of them cannot both see the same room.
_RESERVED: dict[str, int] = {}
_ADMISSION = threading.Lock()


def _held(directory: Path) -> bool:
    in_use = _in_use()
    try:
        return any(str(f.resolve()) in in_use for f in directory.iterdir())
    except OSError:
        return False


def _recently_used(directory: Path, now: float) -> bool:
    try:
        return now - (directory / _MANIFEST).stat().st_mtime < _EVICT_GRACE_SECONDS
    except OSError:
        return False


def _make_room(needed: int, keep: str, source: Path, *, strict: bool = True) -> None:
    """Remove least recently used entries until ``needed`` more bytes fit the
    cache limit, counting the space reserved by decompressions under way.

    Never removed: an entry a session holds open (checked again under the
    entry's lock), one used in the last ``_EVICT_GRACE_SECONDS``, the one being
    written and one another open is writing (its lock is taken). ``strict``
    raises when the room cannot be made; otherwise the cache stays over its
    limit until the next decompression (used after a write that came out
    larger than its estimate). Must be called under ``_ADMISSION``."""
    limit = cache_max_bytes()
    if limit <= 0:
        return
    entries = _entries()
    reserved = {k: v for k, v in _RESERVED.items() if k != keep}
    total = sum(max(size, reserved.get(d.name, 0)) for _, size, d in entries)
    total += sum(v for k, v in reserved.items() if not (cache_dir() / k).is_dir())
    if total + needed <= limit:
        return
    now = time.time()
    for used, size, directory in sorted(entries):
        if total + needed <= limit:
            break
        if directory.name == keep or directory.name in reserved:
            continue
        if now - used < _EVICT_GRACE_SECONDS:
            continue
        with _entry_lock(directory.name, blocking=False) as held:
            # Again under the lock: a cache hit may have refreshed it, or a
            # session taken it, since the scan.
            if not held or _held(directory) or _recently_used(directory, now):
                continue
            # The lock file stays: a process waiting on it must lock the same
            # file as the next one to create the entry.
            shutil.rmtree(directory, ignore_errors=True)
        logger.info("[decompress] evicted %s (%.1f MB) to make room", directory.name, size / 1e6)
        total -= size
    if strict and total + needed > limit:
        raise CompressedFitsError(
            f"Decompressing {source.name} needs about {_fmt(needed)}, and the decompression "
            f"cache cannot make room: its limit is {_fmt(limit)} "
            f"(VISIVO_DECOMPRESS_CACHE_MAX_BYTES) and {_fmt(total)} of it holds datasets "
            f"open or used in the last {_EVICT_GRACE_SECONDS / 60:.0f} minutes. Close some "
            f"datasets or raise the limit.")


def _check_free_space(directory: Path, needed: int, source: Path) -> None:
    try:
        free = shutil.disk_usage(directory).free
    except OSError:
        return
    if free < needed + _FREE_MARGIN_BYTES:
        raise CompressedFitsError(
            f"Decompressing {source.name} needs about {_fmt(needed)} in the decompression "
            f"cache ({directory}), which has {_fmt(free)} free. Free some space there, or "
            f"point VISIVO_DECOMPRESS_CACHE_DIR at a larger disk.")


def _fmt(n: int) -> str:
    for unit, scale in (("TB", 1e12), ("GB", 1e9), ("MB", 1e6), ("kB", 1e3)):
        if n >= scale:
            return f"{n / scale:.1f} {unit}"
    return f"{n} bytes"


# ── Decompressing ────────────────────────────────────────────────────────────


def _padded(n: int) -> int:
    return -(-n // _BLOCK) * _BLOCK


def _image_bytes(header) -> int:
    naxis = int(header.get("NAXIS", 0))
    if naxis == 0:
        return 0
    count = 1
    for i in range(1, naxis + 1):
        count *= int(header.get(f"NAXIS{i}", 0))
    return count * abs(int(header.get("BITPIX", 8))) // 8


def _estimate(path: Path, compression: str) -> int:
    """The decompressed size, from headers (and the gzip trailer) only."""
    from astropy.io import fits  # noqa: PLC0415
    size = path.stat().st_size
    if compression == "gzip":
        estimate = size
        with contextlib.suppress(OSError):
            with open(path, "rb") as f:
                f.seek(-4, os.SEEK_END)
                # ISIZE: the uncompressed size modulo 2**32 (a lower bound).
                estimate = max(estimate, int.from_bytes(f.read(4), "little"))
        # The HDUs up to the image, from the start of the stream (cheap; not
        # through astropy, which would decompress the whole stream to skip
        # the image's data). A lower bound too: HDUs after it are not read.
        hdus = gzip_image_headers(path) or []
        declared = sum(_padded(len(h.header.tostring())) + _padded(_hdu_data_bytes(h.header))
                       for h in hdus)
        return max(estimate, declared)
    total = 0
    with fits.open(path, memmap=False, lazy_load_hdus=True) as hdul:
        for i, hdu in enumerate(hdul):
            if isinstance(hdu, fits.CompImageHDU):
                total += _BLOCK * 4 + _padded(_image_bytes(hdu.header))
            else:
                info = hdul.fileinfo(i)
                total += int(info["datLoc"]) - int(info["hdrLoc"]) + int(info["datSpan"])
    return total


class _Writer:
    """The output file, with progress, stop and free-space checks per chunk."""

    def __init__(self, out, total: int, progress, should_stop, source: Path) -> None:
        self.out, self.total, self.written = out, max(1, total), 0
        self.progress, self.should_stop, self.source = progress, should_stop, source
        self._next_space_check = _SPACE_CHECK_BYTES

    def write(self, data: bytes, fraction: float | None = None) -> None:
        if self.should_stop is not None and self.should_stop():
            raise DecompressionCancelled("Decompression cancelled.")
        try:
            self.out.write(data)
        except OSError as exc:
            if exc.errno == errno.ENOSPC:
                raise CompressedFitsError(
                    f"The disk of the decompression cache ({cache_dir()}) filled up while "
                    f"decompressing {self.source.name}. Free some space there, or point "
                    f"VISIVO_DECOMPRESS_CACHE_DIR at a larger disk.") from exc
            raise
        self.written += len(data)
        if self.written >= self._next_space_check:
            self._next_space_check = self.written + _SPACE_CHECK_BYTES
            remaining = max(0, self.total - self.written)
            _check_free_space(Path(self.out.name).parent, remaining, self.source)
        if self.progress is not None:
            self.progress(min(1.0, fraction if fraction is not None else self.written / self.total))


def _gunzip(source: Path, target: Path, estimate: int, progress, should_stop) -> None:
    size = max(1, source.stat().st_size)
    try:
        with open(source, "rb") as raw, gzip.GzipFile(fileobj=raw, mode="rb") as stream, \
                open(target, "wb") as out:
            writer = _Writer(out, estimate, progress, should_stop, source)
            first = True
            while True:
                chunk = stream.read(_CHUNK_BYTES)
                if not chunk:
                    break
                if first:
                    first = False
                    if not chunk.startswith(b"SIMPLE  ="):
                        raise CompressedFitsError(
                            f"{source.name} is gzip-compressed, but what it holds is not a "
                            f"FITS file.")
                writer.write(chunk, raw.tell() / size)
            if first:
                raise CompressedFitsError(f"{source.name} is an empty gzip stream.")
    except EOFError as exc:
        raise CompressedFitsError(
            f"{source.name} is truncated: the gzip stream ends before its end marker "
            f"({_fmt(source.stat().st_size)} on disk). The download or copy was probably "
            f"interrupted; fetch the file again.") from exc
    except (gzip.BadGzipFile, zlib.error) as exc:
        raise CompressedFitsError(
            f"{source.name} is not a valid gzip file ({exc}); the file is damaged.") from exc


_DTYPES = {8: "u1", 16: ">i2", 32: ">i4", 64: ">i8", -32: ">f4", -64: ">f8"}


def _image_header(header, primary: bool, more_follow: bool):
    """The header of a decompressed image, as a primary or an IMAGE extension.
    Checksums are dropped: they described the compressed table."""
    h = header.copy()
    for key in ("SIMPLE", "XTENSION", "PCOUNT", "GCOUNT", "EXTEND", "CHECKSUM", "DATASUM"):
        while key in h:
            del h[key]
    naxis = int(h.get("NAXIS", 0))
    last = f"NAXIS{naxis}" if naxis else "NAXIS"
    if primary:
        h.insert(0, ("SIMPLE", True, "conforms to FITS standard"))
        if more_follow:
            h.set("EXTEND", True, after=last)
    else:
        h.insert(0, ("XTENSION", "IMAGE", "Image extension"))
        h.set("PCOUNT", 0, after=last)
        h.set("GCOUNT", 1, after="PCOUNT")
    return h


def _blocks(shape: tuple[int, ...], itemsize: int, tile_rows: int) -> Iterator[tuple]:
    """Index tuples covering an array in C order, each about _CHUNK_BYTES,
    split along the outermost axis that keeps a block under that size and in
    whole tiles along it."""
    if not shape:
        return
    k = 0
    while k < len(shape) - 1 and int(np.prod(shape[k + 1:])) * itemsize > _CHUNK_BYTES:
        k += 1
    inner = int(np.prod(shape[k + 1:])) * itemsize
    step = max(1, _CHUNK_BYTES // max(1, inner))
    if tile_rows > 1 and k == 0:
        step = max(tile_rows, step // tile_rows * tile_rows)
    for lead in np.ndindex(*shape[:k]):
        for start in range(0, shape[k], step):
            yield tuple(lead) + (slice(start, min(start + step, shape[k])),)


def _expand_tiles(source: Path, target: Path, estimate: int, progress, should_stop,
                  display_name: Path | None = None) -> None:
    """Write ``source`` with every tile-compressed image decompressed; other
    HDUs are copied byte for byte. An empty primary followed by an image that
    was a primary before compression (``ZSIMPLE = T``, as fpack writes it)
    becomes that primary again, as funpack does."""
    from astropy.io import fits  # noqa: PLC0415
    name = display_name or source
    with fits.open(source, memmap=False, disable_image_compression=True,
                   lazy_load_hdus=False) as raw_hdul:
        zsimple = [bool(h.header.get("ZSIMPLE", False)) for h in raw_hdul]
        # Tile length along the outermost numpy axis (ZTILE of the last FITS
        # axis): blocks are read in whole tiles along it.
        ztile = [max(1, int(h.header.get(f"ZTILE{int(h.header.get('ZNAXIS', 0))}", 1) or 1))
                 for h in raw_hdul]
    with fits.open(source, memmap=False, do_not_scale_image_data=True,
                   lazy_load_hdus=False) as hdul, open(source, "rb") as raw, \
            open(target, "wb") as out:
        n = len(hdul)
        merge = (n > 1 and int(hdul[0].header.get("NAXIS", 0)) == 0
                 and isinstance(hdul[1], fits.CompImageHDU) and zsimple[1])
        writer = _Writer(out, estimate, progress, should_stop, name)
        written_hdus = 0
        for i, hdu in enumerate(hdul):
            if i == 0 and merge:
                continue
            if isinstance(hdu, fits.CompImageHDU):
                header = hdu.header
                bitpix = int(header.get("BITPIX", -32))
                dtype = np.dtype(_DTYPES[bitpix])
                writer.write(_image_header(header, primary=written_hdus == 0,
                                           more_follow=i < n - 1).tostring().encode("ascii"))
                shape = tuple(int(s) for s in hdu.shape)
                count = 0
                try:
                    for index in _blocks(shape, dtype.itemsize, ztile[i]):
                        block = np.ascontiguousarray(hdu.section[index], dtype=dtype)
                        writer.write(block.tobytes())
                        count += block.nbytes
                except (DecompressionCancelled, CompressedFitsError):
                    raise
                except Exception as exc:  # noqa: BLE001 — astropy/cfitsio decoding errors
                    raise CompressedFitsError(
                        f"The tile-compressed image in HDU {i} of {name.name} cannot be "
                        f"decompressed ({exc}); the file is damaged.") from exc
                writer.write(b"\0" * (_padded(count) - count))
            else:
                info = hdul.fileinfo(i)
                start = int(info["hdrLoc"])
                end = int(info["datLoc"]) + int(info["datSpan"])
                raw.seek(start)
                remaining = end - start
                if written_hdus == 0 and i != 0:
                    raise CompressedFitsError(f"{name.name}: cannot rebuild its primary HDU.")
                while remaining > 0:
                    chunk = raw.read(min(_CHUNK_BYTES, remaining))
                    if not chunk:
                        raise CompressedFitsError(f"{name.name} is truncated in HDU {i}.")
                    writer.write(chunk)
                    remaining -= len(chunk)
            written_hdus += 1



def _publish(partial: Path, target: Path) -> None:
    """Move the finished copy into place without replacing one already there:
    without a file lock (Windows) another process may have published the same
    file first, and its copy may be open. A hard link is the atomic
    no-clobber move. Where links are not available: under a file lock
    nobody else publishes, so a rename is safe; without one (Windows),
    ``os.rename`` refuses an existing target there, which is the no-clobber
    move again."""
    try:
        os.link(partial, target)
    except FileExistsError:
        pass                      # the other copy wins; it is the same file
    except OSError:
        if _HAVE_FILE_LOCK:
            os.replace(partial, target)
            return
        try:
            os.rename(partial, target)
            return
        except FileExistsError:
            pass
    partial.unlink(missing_ok=True)


def _check_whole(target: Path, source: Path) -> None:
    """The decompressed file must be as long as its headers say (#118): a gzip
    of a truncated FITS is a complete gzip stream of a short file."""
    from .fits_truncation import FitsTruncatedError, check_not_truncated  # noqa: PLC0415
    try:
        check_not_truncated(target)
    except FitsTruncatedError as exc:
        raise FitsTruncatedError(
            str(exc).replace(target.name, f"{source.name} (decompressed)", 1),
            path=str(source), size=exc.size, declared=exc.declared) from exc


def _hit(entry: Path, target: Path, source: Path, compression: str,
         stat: os.stat_result) -> Materialized | None:
    """The cache entry's copy when it is complete, marked as used now (the
    eviction order); None when there is none. Under the entry's lock."""
    if not (target.is_file() and (entry / _MANIFEST).is_file()):
        return None
    with contextlib.suppress(OSError):
        os.utime(entry / _MANIFEST)
    logger.info("[decompress] cache hit %s -> %s", source, target)
    return Materialized(target, source, compression, True, 0.0, stat.st_size,
                        target.stat().st_size)


def cached_copy(path: str | Path) -> Materialized | None:
    """The decompressed copy of ``path`` when the cache already holds it, and
    None otherwise (not compressed, not decompressed yet, or being
    decompressed right now). Never decompresses and never waits: the
    ``/v1/tasks/decompress`` route answers a cached file at once with it,
    without a task to follow."""
    source = Path(path)
    compression = compression_of(source)
    if compression is None:
        return None
    try:
        resolved = source.resolve()
        stat = resolved.stat()
    except OSError:
        return None
    key = _key(resolved, stat)
    entry = cache_dir() / key
    with _entry_lock(key, blocking=False) as held:
        if not held:
            return None
        return _hit(entry, entry / _decompressed_name(source), source, compression, stat)


def materialize(path: str | Path, progress: Callable[[float], None] | None = None,
                should_stop: Callable[[], bool] | None = None) -> Materialized | None:
    """The decompressed copy of a compressed FITS, made on first use; None
    when ``path`` is not compressed (read it as it is).

    ``progress`` is called with the fraction done; ``should_stop`` is polled
    between chunks and, when it answers True, the decompression ends with
    :class:`DecompressionCancelled` and nothing is cached. Raises
    :class:`CompressedFitsError` for a damaged or truncated file, a full disk
    or a cache that cannot make room."""
    source = Path(path)
    compression = compression_of(source)
    if compression is None:
        return None
    resolved = source.resolve()
    stat = resolved.stat()
    key = _key(resolved, stat)
    entry = cache_dir() / key
    target = entry / _decompressed_name(source)
    with _entry_lock(key, should_stop):
        hit = _hit(entry, target, source, compression, stat)
        if hit is not None:
            return hit
        estimate = _estimate(resolved, compression)
        with _ADMISSION:
            _make_room(estimate, key, source)
            entry.parent.mkdir(parents=True, exist_ok=True)
            _check_free_space(entry.parent, estimate + sum(_RESERVED.values()), source)
            _RESERVED[key] = estimate
        # Staging names of this call alone: where there is no file lock
        # (Windows), two processes may decompress the same file at once, and
        # each must write its own copy; the last rename wins, both whole.
        stamp = f"{os.getpid()}-{threading.get_ident()}"
        partial = entry / f".{target.name}.{stamp}.partial"
        started = time.monotonic()
        try:
            entry.mkdir(parents=True, exist_ok=True)
            if compression == "gzip":
                _gunzip(resolved, partial, estimate, progress, should_stop)
                if _image_is_tile_compressed(partial):
                    # A gzipped fpack file: expand its tiles too.
                    expanded = entry / f".{target.name}.{stamp}.expanded"
                    try:
                        _expand_tiles(partial, expanded, _estimate(partial, "tile"),
                                      progress, should_stop, display_name=source)
                    except BaseException:
                        expanded.unlink(missing_ok=True)
                        raise
                    os.replace(expanded, partial)
            else:
                _expand_tiles(resolved, partial, estimate, progress, should_stop,
                              display_name=source)
            _check_whole(partial, source)
            if should_stop is not None and should_stop():
                raise DecompressionCancelled("Decompression cancelled.")
            seconds = time.monotonic() - started
            (entry / _MANIFEST).write_text(json.dumps({
                "source": str(source), "resolved": str(resolved), "size": stat.st_size,
                "mtime_ns": stat.st_mtime_ns, "compression": compression,
                "seconds": round(seconds, 3)}))
            _publish(partial, target)
        except BaseException:
            partial.unlink(missing_ok=True)
            # The entry directory is removed only under a file lock: without
            # one (Windows) another process may be about to write into it.
            if _HAVE_FILE_LOCK and not target.exists():
                shutil.rmtree(entry, ignore_errors=True)
            raise
        finally:
            with _ADMISSION:
                _RESERVED.pop(key, None)
                # The estimate is from headers; a larger result is trimmed
                # from the other entries now, without refusing what is done.
                _make_room(0, key, source, strict=False)
    size = target.stat().st_size
    logger.info("[decompress] %s (%s, %s) -> %s (%s) in %.1f s", source, compression,
                _fmt(stat.st_size), target, _fmt(size), seconds)
    if progress is not None:
        progress(1.0)
    return Materialized(target, source, compression, False, seconds, stat.st_size, size)


def readable_path(path: str | Path, **kwargs) -> Path:
    """The path a reader should open: the decompressed copy of a compressed
    FITS, else ``path`` itself."""
    made = materialize(path, **kwargs)
    return made.path if made is not None else Path(path)
