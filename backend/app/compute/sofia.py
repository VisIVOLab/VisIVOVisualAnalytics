from __future__ import annotations

import base64
import concurrent.futures as _cf  # noqa: F401
import logging  # noqa: F401
import os as _os  # noqa: F401
import shutil  # noqa: F401
import subprocess  # noqa: F401
import warnings  # noqa: F401
import zlib  # noqa: F401
from pathlib import Path  # noqa: F401
from typing import Any  # noqa: F401

import numpy as np

from ..fits_dataset import ScientificFitsDataset  # noqa: F401
from ._common import (  # noqa: F401
    logger,
    _qt_zlib_compress,
    _b64f32,
    _b64i32,
    _b64f32_compressed,
    _b64i32_compressed,
    _finite_range,
    _integrated_spacing,
)


def _sofia_value(v: Any) -> str:
    if isinstance(v, bool):
        return "true" if v else "false"
    return str(v)


def _parse_votable_rows(path: Path) -> list[dict[str, Any]]:
    """Parse a VOTable catalogue into a list of flat {column: value} dicts."""
    import xml.etree.ElementTree as ET  # noqa: PLC0415

    def _local(tag: str) -> str:
        return tag.split("}")[-1]

    def _coerce(s: str | None):
        if s is None:
            return None
        t = s.strip()
        try:
            return int(t)
        except ValueError:
            pass
        try:
            return float(t)
        except ValueError:
            return t

    root = ET.parse(path).getroot()
    fields = [el.get("name") for el in root.iter() if _local(el.tag) == "FIELD"]
    rows: list[dict[str, Any]] = []
    for tr in (el for el in root.iter() if _local(el.tag) == "TR"):
        tds = [td.text for td in list(tr) if _local(td.tag) == "TD"]
        rows.append({name: _coerce(val) for name, val in zip(fields, tds)})
    return rows


def resolve_sofia_bin() -> str | None:
    """Locate the SoFiA-2 executable.

    SoFiA-2's own ``compile.sh`` installs the binary as ``sofia``, while many
    deployments rename/symlink it to ``sofia2`` — so accept either, and allow
    an explicit override via ``VISIVO_SOFIA_BIN`` (e.g. when it lives outside
    PATH inside a bundled Apptainer image).
    """
    override = (_os.environ.get("VISIVO_SOFIA_BIN") or "").strip()
    if override:
        return override
    for name in ("sofia2", "sofia"):
        found = shutil.which(name)
        if found:
            return found
    return None


def worker_sofia(cube_path: str, out_dir: str, sofia_params: dict[str, Any] | None,
                 timeout: float | None = None) -> dict[str, Any]:
    """Run SoFiA-2 on ``cube_path``, writing outputs into ``out_dir``.

    Returns the catalogue (flat rows), the source count, and the mask-cube path
    (empty when SoFiA did not write a mask). Raises on non-zero SoFiA exit. When
    ``timeout`` (seconds) is given, the SoFiA child is killed and a clear error
    raised if it runs longer — so a caller with its own deadline (e.g. the
    copilot) doesn't leave the finder running unbounded past its response.
    """
    sofia_bin = resolve_sofia_bin() or "sofia2"
    par_lines = [
        f"input.data = {cube_path}",
        f"output.directory = {out_dir}",
        "output.filename = sofia_out",
        "output.writeCatXML = true",
        "output.writeMask = true",
        "output.overwrite = true",
    ]
    for key, val in (sofia_params or {}).items():
        par_lines.append(f"{key} = {_sofia_value(val)}")
    par_path = Path(out_dir) / "sofia.par"
    par_path.write_text("\n".join(par_lines) + "\n")

    try:
        proc = subprocess.run(
            [sofia_bin, str(par_path)], capture_output=True, text=True, timeout=timeout,
        )
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(
            f"SoFiA-2 exceeded the {timeout:.0f}s time limit and was stopped.") from exc
    if getattr(proc, "returncode", 1) != 0:
        raise RuntimeError(
            f"SoFiA-2 failed (exit {proc.returncode}): "
            f"{(getattr(proc, 'stderr', '') or '')[:500]}")

    cat_path = Path(out_dir) / "sofia_out_cat.xml"
    sources = _parse_votable_rows(cat_path) if cat_path.exists() else []
    mask_path = Path(out_dir) / "sofia_out_mask.fits"
    return {
        "valid": True,
        "error": "",
        "n_sources": len(sources),
        "sources": sources,
        "mask_path": str(mask_path) if mask_path.exists() else "",
        "catalog_path": str(cat_path) if cat_path.exists() else "",
    }


# ── Spectral probe / aperture (interactive profiler at SKA scale) ───────────────
# Extract a spectrum at a pixel (or over a circular aperture) directly on the
# node, so the desktop profiler works on multi-GB remote cubes without pulling
# the whole cube to the client.
