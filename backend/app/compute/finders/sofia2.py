"""SoFiA-2 as a finder plugin: a child process of the event loop.

The synchronous :func:`app.compute.sofia.worker_sofia` serves the request/reply
``/v1/sourcefind`` route. A task needs more: the run must stop when the task is
cancelled, and its log must be kept. So the process is started with asyncio
here (see ``_process``), killed on cancellation or on the timeout, and its
output is written next to the products.
"""
from __future__ import annotations

import asyncio
import re
from pathlib import Path
from typing import Any

from ..sofia import _parse_votable_rows, _sofia_value, resolve_sofia_bin
from ._process import ChildTimeout, run_child

# SoFiA-2 parameters a caller may set, with what they mean (the names are
# SoFiA's own; see its documentation for the full list and the defaults).
PARAMETERS = {
    "scfind.threshold": "S/N threshold of the smooth-and-clip finder (default 5.0)",
    "scfind.kernelsXY": "spatial smoothing kernels in pixels, e.g. '0, 3, 6'",
    "scfind.kernelsZ": "spectral smoothing kernels in channels, e.g. '0, 3, 7'",
    "linker.minSizeXY": "minimum spatial size of a source in pixels",
    "linker.minSizeZ": "minimum spectral size of a source in channels",
    "reliability.enable": "true to filter detections by reliability",
    "reliability.threshold": "reliability threshold (default 0.9)",
}

# What a caller may set: the pipeline's own switches and the finder, linker,
# reliability, dilation and parametrisation sections. Not the input and output
# sections, which the service sets, and not the few other keys that name a
# file for SoFiA to read: a parameter must never make the node open a path of
# the caller's choosing.
# Memory per byte of cube: SoFiA-2 holds the cube, the mask and the copy the
# smooth-and-clip finder smooths. Measured on the SKAVA testbed: 2.25x the
# cube's bytes of anonymous memory at the peak (a 512 MB cube runs under a
# 1280 MB cgroup limit and is killed under 1024 MB); a little on top for the
# process itself and the catalogue it writes.
MEMORY_FACTOR = 2.3
MEMORY_FLOOR_BYTES = 128 << 20

_ALLOWED_SECTIONS = ("pipeline", "flag", "scfind", "threshold", "linker", "reliability", "dilation", "parameter")
_FILE_INPUTS = {"flag.cube", "flag.catalog", "reliability.catalog"}
_KEY = re.compile(r"^[A-Za-z]+\.[A-Za-z0-9]+$")


def info():
    from . import FinderInfo

    binary = resolve_sofia_bin()
    return FinderInfo(
        name="sofia2",
        kinds=("cube",),
        description="SoFiA-2 smooth-and-clip source finder for spectral-line cubes.",
        parameters=PARAMETERS,
        available=bool(binary),
        reason="" if binary else "SoFiA-2 executable not found (set VISIVO_SOFIA_BIN or put sofia2 on PATH)",
        memory_factor=MEMORY_FACTOR,
        memory_floor_bytes=MEMORY_FLOOR_BYTES,
    )


def refused(params: dict[str, Any]) -> str | None:
    """Why a parameter set is not accepted, or None when it is."""
    for key, value in params.items():
        name = str(key).strip()
        if not _KEY.match(name):
            return f"parameter {name!r} is not a SoFiA-2 parameter name"
        section = name.split(".", 1)[0].lower()
        if section not in _ALLOWED_SECTIONS:
            return f"parameter {name!r} is not accepted: the {section} section is set by the service"
        if name in _FILE_INPUTS:
            return f"parameter {name!r} is not accepted: it names a file for SoFiA-2 to read"
        text = _sofia_value(value)
        if "\n" in text or "\r" in text or len(text) > 500:
            return f"parameter {name!r} has an invalid value"
    return None


async def run(path: str, out_dir: Path, params: dict[str, Any], timeout: float) -> dict[str, Any]:
    from . import FinderError, FinderUnavailable

    sofia_bin = resolve_sofia_bin()
    if not sofia_bin:
        raise FinderUnavailable("SoFiA-2 executable not found")
    why = refused(params)
    if why is not None:
        raise FinderError(why)
    if "\n" in path or "\r" in path:
        # A pathname can hold a newline; in the .par it would be a second line.
        raise FinderError("the dataset path contains a line break, which SoFiA-2 cannot be given")

    par_lines = [
        f"input.data = {path}",
        f"output.directory = {out_dir}",
        "output.filename = sofia_out",
        "output.writeCatXML = true",
        "output.writeMask = true",
        "output.overwrite = true",
    ]
    par_lines += [f"{str(key).strip()} = {_sofia_value(value)}" for key, value in params.items()]
    par_path = out_dir / "sofia.par"
    par_path.write_text("\n".join(par_lines) + "\n")

    try:
        code, out, err = await run_child([sofia_bin, str(par_path)], out_dir, timeout, out_dir / "sofia.log")
    except ChildTimeout as exc:
        raise FinderError(f"SoFiA-2 {exc}") from exc
    if code != 0:
        # Killed by SIGKILL: on a node under a memory limit, that is the
        # kernel's out-of-memory killer, not a SoFiA-2 error.
        killed = code in (-9, 137)
        hint = (f" (killed; most often the node ran out of memory, SoFiA-2 needs about "
                f"{MEMORY_FACTOR:g}x the cube's size)" if killed else "")
        raise FinderError(f"SoFiA-2 failed (exit {code}){hint}: {err[:500] or out[-500:]}")

    cat_path = out_dir / "sofia_out_cat.xml"
    mask_path = out_dir / "sofia_out_mask.fits"
    sources = await asyncio.to_thread(_parse_votable_rows, cat_path) if cat_path.exists() else []
    return {
        "n_sources": len(sources),
        "sources": sources,
        "mask_path": str(mask_path) if mask_path.exists() else "",
        "catalog_path": str(cat_path) if cat_path.exists() else "",
        "log_path": str(out_dir / "sofia.log"),
    }
