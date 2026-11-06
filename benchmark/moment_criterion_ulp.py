"""Acceptance criterion for out-of-core moment maps, for
``moment_ooc_bench.py check --criterion``.

Tolerances in units of the float32 spacing (ULP) of the reference value at
each pixel, plus an absolute floor in spectral-axis units for M2 (0.1 Hz on a
frequency axis):

  between two backend ways (reference "inmem")
      M0 <= 1 ULP, M1 <= 1 ULP, M2 <= 2 ULP + 0.1
  against the independent float64 reference (reference "float64")
      M0 relative <= 1e-6, M1 <= 1 ULP, M2 <= 1 ULP + 0.1

and in both cases the same set of blanked (NaN) pixels: a pixel with no valid
voxel has no moment, whichever way computed it.
"""
from __future__ import annotations

import numpy as np

M2_FLOOR = 0.1   # spectral-axis units (Hz for FREQ cubes)


def _tolerance(order: int, ref: np.ndarray, reference: str) -> np.ndarray:
    ulp = np.spacing(np.abs(ref).astype(np.float32)).astype(np.float64)
    if reference == "float64":
        if order == 0:
            return 1e-6 * np.abs(ref)
        if order == 1:
            return ulp
        return ulp + M2_FLOOR
    if order == 2:
        return 2.0 * ulp + M2_FLOOR
    return ulp


def compare(order: int, ref: np.ndarray, test: np.ndarray, reference: str) -> dict:
    # Blank means NaN; an infinity is an overflow, never a blank pixel.
    if np.isinf(ref).any() or np.isinf(test).any():
        return {"ok": False, "infinite_ref": int(np.isinf(ref).sum()),
                "infinite_test": int(np.isinf(test).sum())}
    fr, ft = ~np.isnan(ref), ~np.isnan(test)
    both = fr & ft
    r = ref[both].astype(np.float64)
    diff = np.abs(test[both].astype(np.float64) - r)
    tol = _tolerance(order, r, reference)
    bad = diff > tol
    ulp = np.spacing(np.abs(r).astype(np.float32)).astype(np.float64)
    return {"ok": bool(not bad.any() and np.array_equal(fr, ft)),
            "nan_mismatch": int((fr != ft).sum()),
            "finite_ref": int(fr.sum()), "finite_test": int(ft.sum()),
            "max_abs_diff": float(diff.max()) if diff.size else 0.0,
            "max_diff_in_ulp": float((diff / ulp).max()) if diff.size else 0.0,
            "n_out_of_tolerance": int(bad.sum())}
