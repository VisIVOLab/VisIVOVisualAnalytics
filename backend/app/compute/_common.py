"""Shared infrastructure for the compute worker package.

Imports, logger, the scientific-FITS dataset facade, and the small leaf
helpers (payload encoding + value ranges) used across more than one domain
module.  Domain modules pull what they need from here.
"""

from __future__ import annotations

import base64
import zlib
from typing import Any  # noqa: F401  (re-exported for domain modules)

import numpy as np

from ..fits_dataset import ScientificFitsDataset  # noqa: F401

import logging

logger = logging.getLogger("visivo.compute")


def _qt_zlib_compress(raw: bytes, level: int = 6) -> bytes:
    """Prepend the uncompressed length as a 4-byte big-endian uint (Qt convention).

    ``level`` controls zlib's speed/ratio trade-off:
      * 6 (default) — ~50 MB/s compress, ~60-70% ratio. Fine for small
        payloads (slices, moment maps).
      * 1 — ~150-200 MB/s compress, ~40-50% ratio. Use for multi-GB
        payloads (subvolume, preview) where the per-MB compression time
        dominates the bandwidth saved at higher levels.
    """
    return len(raw).to_bytes(4, byteorder="big", signed=False) + zlib.compress(raw, level)

def _b64f32(array: np.ndarray) -> str:
    return base64.b64encode(np.ascontiguousarray(array, dtype=np.float32).tobytes()).decode("ascii")

def _b64i32(array: np.ndarray) -> str:
    return base64.b64encode(np.ascontiguousarray(array, dtype=np.int32).tobytes()).decode("ascii")

def _b64f32_compressed(array: np.ndarray, level: int = 6) -> tuple[str, str]:
    raw = np.ascontiguousarray(array, dtype=np.float32).tobytes()
    return "qt-zlib", base64.b64encode(_qt_zlib_compress(raw, level)).decode("ascii")

def _b64i32_compressed(array: np.ndarray, level: int = 6) -> tuple[str, str]:
    raw = np.ascontiguousarray(array, dtype=np.int32).tobytes()
    return "qt-zlib", base64.b64encode(_qt_zlib_compress(raw, level)).decode("ascii")

def _finite_range(array: np.ndarray) -> tuple[float, float]:
    finite = np.isfinite(array)
    if not finite.any():
        return 0.0, 0.0
    return float(np.nanmin(array)), float(np.nanmax(array))

def _integrated_spacing(coords: np.ndarray) -> np.ndarray:
    if coords.size == 0:
        return np.zeros(0, dtype=np.float64)
    if coords.size == 1:
        return np.ones(1, dtype=np.float64)
    edges = np.empty(coords.size + 1, dtype=np.float64)
    edges[1:-1] = 0.5 * (coords[:-1] + coords[1:])
    edges[0] = coords[0] - 0.5 * (coords[1] - coords[0])
    edges[-1] = coords[-1] + 0.5 * (coords[-1] - coords[-2])
    return np.abs(np.diff(edges))
