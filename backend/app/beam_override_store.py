"""A restoring beam typed by the user for a FITS dataset whose header has none
(issue #416): a small JSON file that every process of the backend reads.

Single-dish survey cubes (CHIMPS, 15 arcsec) often ship without BMAJ/BMIN, and
every per-beam quantity (Jy/beam fluxes, beams per region, the effective
number of independent samples behind an error) then has nothing to work with.
The beam is read by :class:`app.fits_dataset.ScientificFitsDataset` wherever
it is constructed, in the worker pool and render sessions as much as in the
process that answered the request, so like the VBT byte order
(:mod:`app.vbt_byte_order_store`) it is a file, not session memory.

A typed beam fills a gap and never overrides a header: it is used only when
the header has no complete BMAJ/BMIN and no per-channel BEAMS table. It is
keyed by the real path of the file the user opened (a compressed file, not
its decompressed cache copy) and valid while that file is the one it was set
for (size, modification time, inode). The store is ``$VISIVO_BEAM_OVERRIDES``
when set, else ``.beam-overrides.json`` in the workspace
(``$VISIVO_EXPORTS_DIR``); it is written under the same cross-process lock
and atomic replace as the byte-order store.
"""
from __future__ import annotations

import json
import logging
import math
import os
import threading
import time
from pathlib import Path

from .vbt_byte_order_store import _exclusive, _write

logger = logging.getLogger("visivo.backend")

_LOCK = threading.Lock()
_cache: tuple[tuple, dict] | None = None
#: Largest beam accepted, in degrees: anything wider is a unit mistake
#: (arcmin or degrees typed as arcsec the other way round).
MAX_BEAM_DEG = 10.0


def store_path() -> Path:
    explicit = os.environ.get("VISIVO_BEAM_OVERRIDES")
    if explicit:
        return Path(explicit)
    root = os.environ.get("VISIVO_EXPORTS_DIR", str(Path.home() / ".visivo" / "exports"))
    return Path(root) / ".beam-overrides.json"


def source_path(path: str | os.PathLike) -> str:
    """The file the user opened for ``path``: the compressed source of a
    decompressed cache copy, else ``path`` itself (symlinks followed)."""
    from .compressed_fits import source_of  # noqa: PLC0415
    return os.path.realpath(source_of(path) or str(path))


def _identity(key: str) -> list | None:
    try:
        st = os.stat(key)
    except OSError:
        return None
    return [st.st_size, st.st_mtime_ns, st.st_ino]


def _load() -> dict:
    global _cache
    path = store_path()
    try:
        st = os.stat(path)
    except OSError:
        return {}
    stamp = (str(path), st.st_size, st.st_mtime_ns, st.st_ino)
    with _LOCK:
        if _cache is not None and _cache[0] == stamp:
            return _cache[1]
    try:
        entries = json.loads(path.read_text("utf-8"))
        if not isinstance(entries, dict):
            raise ValueError("not an object")
    except (OSError, ValueError) as exc:
        logger.warning("[beam-override] ignoring unreadable store %s: %s", path, exc)
        entries = {}
    with _LOCK:
        _cache = (stamp, entries)
    return entries


def get(path: str | os.PathLike) -> tuple[float, float, float] | None:
    """``(major_deg, minor_deg, pa_deg)`` typed for ``path``, or None (none
    set, or set for a version of the file that is gone)."""
    entries = _load()
    if not entries:
        return None
    key = source_path(path)
    entry = entries.get(key)
    if not isinstance(entry, dict) or entry.get("identity") != _identity(key):
        return None
    try:
        major, minor, pa = (float(entry[k]) for k in ("bmaj_deg", "bmin_deg", "bpa_deg"))
    except (KeyError, TypeError, ValueError):
        return None
    if not (_valid_axis(major) and _valid_axis(minor) and math.isfinite(pa)):
        return None
    return major, minor, pa


def _valid_axis(value: float) -> bool:
    return math.isfinite(value) and 0.0 < value <= MAX_BEAM_DEG


def validate(major_arcsec: float, minor_arcsec: float | None,
             pa_deg: float | None) -> tuple[float, float, float]:
    """Beam in FITS units (degrees) from arcsec, or ValueError with the reason.
    ``minor`` defaults to ``major`` (a circular beam), ``pa`` to 0."""
    major = float(major_arcsec)
    minor = major if minor_arcsec is None else float(minor_arcsec)
    pa = 0.0 if pa_deg is None else float(pa_deg)
    if not (math.isfinite(major) and math.isfinite(minor) and major > 0 and minor > 0):
        raise ValueError("The beam axes must be positive numbers of arcseconds.")
    if minor > major:
        raise ValueError("The beam minor axis cannot be larger than the major axis.")
    if major / 3600.0 > MAX_BEAM_DEG:
        raise ValueError(
            f"A beam of {major:g} arcsec is wider than {MAX_BEAM_DEG:g} degrees; "
            "the axes are in arcseconds.")
    if not math.isfinite(pa):
        raise ValueError("The beam position angle must be a finite number of degrees.")
    return major / 3600.0, minor / 3600.0, pa


def set_beam(path: str | os.PathLike, beam_deg: tuple[float, float, float] | None,
             by: str = "") -> None:
    """Keep ``beam_deg`` (``(major, minor, pa)`` in degrees) for ``path``, or
    forget it when None. Entries whose file has changed are dropped on the
    way; one whose file cannot be stat'ed (an offline mount) is kept."""
    global _cache
    key = source_path(path)
    identity = _identity(key)
    if beam_deg is not None and identity is None:
        raise ValueError(f"The beam cannot be kept for {key}: the file cannot be read.")
    store = store_path()
    with _exclusive(store):
        try:
            entries = json.loads(store.read_text("utf-8"))
            if not isinstance(entries, dict):
                entries = {}
        except (OSError, ValueError):
            entries = {}

        def current(k: str, v) -> bool:
            if not isinstance(v, dict):
                return False
            now = _identity(k)
            return now is None or v.get("identity") == now

        entries = {k: v for k, v in entries.items() if k != key and current(k, v)}
        if beam_deg is not None:
            major, minor, pa = beam_deg
            entries[key] = {"bmaj_deg": major, "bmin_deg": minor, "bpa_deg": pa,
                            "identity": identity, "set_by": by,
                            "set_at": time.strftime("%Y-%m-%dT%H:%M:%S%z")}
        _write(store, entries)
        with _LOCK:
            _cache = None
    logger.info("[beam-override] %s: %s by %s", key,
                "cleared" if beam_deg is None else
                "BMAJ=%.3g\" BMIN=%.3g\" BPA=%.3g" % (beam_deg[0] * 3600, beam_deg[1] * 3600,
                                                      beam_deg[2]),
                by or "unknown")


def clear() -> None:
    """Forget every typed beam (tests)."""
    global _cache
    try:
        os.unlink(store_path())
    except OSError:
        pass
    with _LOCK:
        _cache = None
