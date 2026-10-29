"""Source finders as plugins: what this node can run, behind one interface.

A finder takes a dataset and writes, in an output directory, a label mask
(FITS on the dataset's grid: 0 is background, n is the n-th source) and a
catalogue (VOTable), and returns the catalogue's rows. Two ship here:

* ``sofia2``: SoFiA-2 for spectral cubes, when the executable is installed
  (``VISIVO_SOFIA_BIN``, or ``sofia2``/``sofia`` on PATH). It runs as a child
  process of the event loop, so a cancelled task kills it.
* ``photutils``: image segmentation for 2-D images; always present.

:func:`available` is what the node advertises (``/v1/node/status``,
``/v1/finders``), and what a catalogue routing finder jobs to nodes reads.
"""
from __future__ import annotations

import asyncio
import os
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Awaitable, Callable

from . import photutils_finder, sofia2


class FinderError(RuntimeError):
    """The finder could not run or failed: the task's error, never a crash."""


class FinderUnavailable(FinderError):
    """The finder is not installed on this node."""


@dataclass(frozen=True)
class FinderInfo:
    name: str
    kinds: tuple[str, ...]
    description: str
    parameters: dict[str, str]
    available: bool
    reason: str = ""
    # How much memory a run needs, from the dataset's size: factor × bytes + floor.
    memory_factor: float = 1.0
    memory_floor_bytes: int = 0

    def as_dict(self) -> dict[str, Any]:
        d = asdict(self)
        d["kinds"] = list(self.kinds)
        return d


@dataclass(frozen=True)
class _Finder:
    info: Callable[[], FinderInfo]
    run: Callable[[str, Path, dict[str, Any], float], Awaitable[dict[str, Any]]]


_REGISTRY: dict[str, _Finder] = {
    "sofia2": _Finder(info=sofia2.info, run=sofia2.run),
    "photutils": _Finder(info=photutils_finder.info, run=photutils_finder.run),
}


def names() -> list[str]:
    return list(_REGISTRY)


def info(name: str) -> FinderInfo | None:
    finder = _REGISTRY.get(name)
    return finder.info() if finder else None


def catalogue() -> list[FinderInfo]:
    """Every finder known here, available or not (with the reason)."""
    return [f.info() for f in _REGISTRY.values()]


def available() -> list[str]:
    return [f.name for f in catalogue() if f.available]


def memory_needs() -> dict[str, dict[str, float | int]]:
    """Per available finder, how its memory need scales with the dataset:
    ``factor`` times the dataset's bytes plus ``floor_bytes``. A placement
    decision (the broker's) reads it from the node status."""
    return {f.name: {"factor": f.memory_factor, "floor_bytes": f.memory_floor_bytes}
            for f in catalogue() if f.available}


def default_timeout() -> float:
    """Seconds a finder may run (``VISIVO_FINDER_TIMEOUT_S``, an hour by default)."""
    try:
        return max(1.0, float(os.environ.get("VISIVO_FINDER_TIMEOUT_S", "3600")))
    except ValueError:
        return 3600.0


async def run(name: str, path: str, kind: str, out_dir: Path, params: dict[str, Any] | None,
              timeout: float | None = None) -> dict[str, Any]:
    """Run finder ``name`` on ``path`` (a dataset of ``kind``), writing into ``out_dir``.

    Returns ``{finder, n_sources, sources, mask_path, catalog_path}``. Raises
    :class:`FinderError` for anything the caller should report as the task's
    failure; ``asyncio.CancelledError`` propagates after the finder is stopped.
    """
    finder = _REGISTRY.get(name)
    if finder is None:
        raise FinderError(f"unknown finder {name!r}; this node has {', '.join(_REGISTRY)}")
    meta = finder.info()
    if not meta.available:
        raise FinderUnavailable(f"{name} is not available on this node: {meta.reason}")
    if kind not in meta.kinds:
        raise FinderError(f"{name} works on {', '.join(meta.kinds)} datasets, not on a {kind}")
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    result = await finder.run(path, out_dir, dict(params or {}), timeout or default_timeout())
    return {"finder": name, **result}


__all__ = ["FinderError", "FinderUnavailable", "FinderInfo", "available", "catalogue",
           "default_timeout", "info", "names", "run"]
