"""
test_redshift_nan.py – unit tests for the NaN sentinel fix for redshift.

Background:
  Previously ``0.0`` was used as a sentinel for missing/invalid redshift in
  CosmologyDistanceResponse and CosmologyBatchResponse.  This is ambiguous
  because z=0.0 is a physically valid redshift (local universe).  The fix
  replaces 0.0 sentinels with None (null in JSON) for all cases where no real
  redshift value is available.

Covers:
  1. CosmologyDistanceResponse model — None defaults when valid=False
  2. CosmologyDistanceResponse model — real 0.0 preserved on success path
  3. Batch logic — invalid redshifts produce None (not 0.0) in distances_Mpc
  4. Batch logic — valid z=0.0 produces 0.0 (not None) in distances_Mpc
  5. _apply_tabular_query — NaN rows are excluded by numeric comparison filters
  6. _apply_tabular_query — NaN rows excluded by equality filter
  7. _apply_tabular_query — NaN rows are not in 'in' filter results
  8. _apply_tabular_query — sort with NaN pushes them to last (na_position='last')

Run with:
    cd backend
    pytest tests/test_redshift_nan.py -v
"""

from __future__ import annotations

import math

import numpy as np
import pandas as pd
import pytest


# ── Lazy imports ──────────────────────────────────────────────────────────────


def _response_cls():
    from app.schemas import CosmologyDistanceResponse
    return CosmologyDistanceResponse


def _batch_cls():
    from app.schemas import CosmologyBatchResponse
    return CosmologyBatchResponse


def _tabular_fn():
    from app.dependencies import _apply_tabular_query
    return _apply_tabular_query


def _tabular_models():
    from app.schemas import TabularFilter, TabularSortSpec
    return TabularFilter, TabularSortSpec


def flt(field: str, op: str, value: str):
    TabularFilter, _ = _tabular_models()
    return TabularFilter(field=field, op=op, value=value)


def srt(field: str, direction: str = "asc"):
    _, TabularSortSpec = _tabular_models()
    return TabularSortSpec(field=field, direction=direction)


def apply(df, filters=None, sort=None, limit=0, offset=0):
    return _tabular_fn()(df, filters or [], sort, limit, offset)


# ═══════════════════════════════════════════════════════════════════════════════
# 1. CosmologyDistanceResponse: None defaults when valid=False
# ═══════════════════════════════════════════════════════════════════════════════


def test_distance_response_error_z_is_none():
    """Error response must have z=None, not z=0.0."""
    resp = _response_cls()(valid=False, error="astropy unavailable")
    assert resp.z is None, "z sentinel must be None for error responses"


def test_distance_response_error_comoving_is_none():
    resp = _response_cls()(valid=False, error="some error")
    assert resp.comoving_Mpc is None


def test_distance_response_error_luminosity_is_none():
    resp = _response_cls()(valid=False, error="some error")
    assert resp.luminosity_Mpc is None


def test_distance_response_error_proper_motion_is_none():
    resp = _response_cls()(valid=False, error="some error")
    assert resp.proper_motion_kpc_per_arcsec is None


def test_distance_response_serialises_none_as_null():
    """Pydantic must serialise None fields as JSON null, not 0.0."""
    resp = _response_cls()(valid=False, error="some error")
    d = resp.model_dump()
    assert d["z"] is None
    assert d["comoving_Mpc"] is None
    assert d["luminosity_Mpc"] is None
    assert d["proper_motion_kpc_per_arcsec"] is None


# ═══════════════════════════════════════════════════════════════════════════════
# 2. CosmologyDistanceResponse: real z=0.0 must be preserved on success path
# ═══════════════════════════════════════════════════════════════════════════════


def test_distance_response_real_z_zero_preserved():
    """z=0.0 is a valid physical redshift — must survive serialisation as 0.0."""
    resp = _response_cls()(
        valid=True,
        error="",
        z=0.0,
        model="Planck18",
        comoving_Mpc=0.0,
        luminosity_Mpc=0.0,
        proper_motion_kpc_per_arcsec=0.0,
    )
    assert resp.z == pytest.approx(0.0)
    d = resp.model_dump()
    assert d["z"] == 0.0
    assert d["comoving_Mpc"] == 0.0


def test_distance_response_real_z_nonzero_preserved():
    resp = _response_cls()(
        valid=True,
        error="",
        z=1.5,
        model="Planck18",
        comoving_Mpc=4300.0,
        luminosity_Mpc=10750.0,
        proper_motion_kpc_per_arcsec=8.5,
    )
    assert resp.z == pytest.approx(1.5)
    assert resp.comoving_Mpc == pytest.approx(4300.0)


# ═══════════════════════════════════════════════════════════════════════════════
# 3. Batch response: invalid redshifts produce None, not 0.0
# ═══════════════════════════════════════════════════════════════════════════════


def test_batch_response_none_entry_accepted():
    """distances_Mpc must accept None entries (not just floats)."""
    resp = _batch_cls()(
        valid=True,
        error="",
        model="Planck18",
        distances_Mpc=[1000.0, None, 2000.0],
    )
    assert resp.distances_Mpc[1] is None


def test_batch_response_all_none_accepted():
    resp = _batch_cls()(valid=True, error="", model="Planck18", distances_Mpc=[None, None])
    assert all(v is None for v in resp.distances_Mpc)


def test_batch_response_mixed_none_and_float():
    distances = [0.0, None, 3303.0, None]
    resp = _batch_cls()(valid=True, error="", model="Planck18", distances_Mpc=distances)
    assert resp.distances_Mpc[0] == pytest.approx(0.0)
    assert resp.distances_Mpc[1] is None
    assert resp.distances_Mpc[2] == pytest.approx(3303.0)
    assert resp.distances_Mpc[3] is None


# ── Helper: simulate the batch computation logic ──────────────────────────────
# These tests exercise the same logic used in cosmology_distance_batch without
# requiring astropy or the HTTP layer.

def _batch_distances(redshifts: list[float]) -> list[float | None]:
    """Replicate the sentinel logic from cosmology_distance_batch."""
    zs = np.array(redshifts, dtype=float)
    distances = np.full_like(zs, float("nan"))
    valid_mask = np.isfinite(zs) & (zs >= 0.0)
    # Pretend cosmo.comoving_distance(z) = z * 3303.0  (linear mock)
    if valid_mask.any():
        distances[valid_mask] = zs[valid_mask] * 3303.0
    return [None if not math.isfinite(d) else float(d) for d in distances]


def test_batch_logic_negative_z_gives_none():
    result = _batch_distances([-0.5])
    assert result[0] is None, "Negative redshift must yield None, not 0.0"


def test_batch_logic_nan_z_gives_none():
    result = _batch_distances([float("nan")])
    assert result[0] is None, "NaN redshift must yield None"


def test_batch_logic_valid_z_zero_gives_zero():
    result = _batch_distances([0.0])
    # z=0 → distance 0 (real physical value, not sentinel)
    assert result[0] == pytest.approx(0.0), "z=0.0 must give 0.0 distance, not None"


def test_batch_logic_valid_z_nonzero_gives_float():
    result = _batch_distances([1.0])
    assert result[0] is not None
    assert result[0] > 0.0


def test_batch_logic_mixed_valid_and_invalid():
    result = _batch_distances([1.0, -1.0, 0.5, float("nan")])
    assert result[0] is not None   # valid
    assert result[1] is None       # negative → None
    assert result[2] is not None   # valid
    assert result[3] is None       # NaN → None


def test_batch_logic_order_preserved():
    """Output order must match input order."""
    result = _batch_distances([2.0, -1.0, 0.0, float("nan"), 1.0])
    # indices: 0→valid, 1→invalid(neg), 2→valid(0.0), 3→invalid(nan), 4→valid
    assert result[0] is not None
    assert result[1] is None
    assert result[2] == pytest.approx(0.0)
    assert result[3] is None
    assert result[4] is not None


def test_batch_logic_all_invalid_gives_all_none():
    result = _batch_distances([-1.0, -2.0, float("nan")])
    assert all(v is None for v in result)


def test_batch_logic_all_valid():
    result = _batch_distances([0.0, 0.5, 1.0])
    assert all(v is not None for v in result)


# ═══════════════════════════════════════════════════════════════════════════════
# 4. _apply_tabular_query: NaN in a numeric column is excluded by filters
# ═══════════════════════════════════════════════════════════════════════════════
# This verifies that introducing NaN sentinels into catalogues does not crash
# the filter engine and that NaN rows are silently excluded (pandas semantics).


@pytest.fixture()
def df_with_nan() -> pd.DataFrame:
    """DataFrame with a 'z' column where some rows have NaN (missing redshift)."""
    return pd.DataFrame(
        {
            "id":    [1,    2,    3,    4,    5],
            "name":  ["a",  "b",  "c",  "d",  "e"],
            "z":     [0.1,  0.5,  float("nan"), 1.2,  float("nan")],
        }
    )


def test_nan_excluded_by_gt_filter(df_with_nan):
    """Rows with NaN z must not appear in z > 0.0 results."""
    result, total = apply(df_with_nan.copy(), [flt("z", ">", "0.0")])
    assert total == 3           # rows 1, 2, 4 (NaN rows excluded)
    assert float("nan") not in list(result["z"])
    assert 3 not in list(result["id"])   # id=3 has NaN z
    assert 5 not in list(result["id"])   # id=5 has NaN z


def test_nan_excluded_by_lt_filter(df_with_nan):
    result, total = apply(df_with_nan.copy(), [flt("z", "<", "1.0")])
    assert total == 2           # rows 1 (z=0.1) and 2 (z=0.5)
    assert 3 not in list(result["id"])
    assert 5 not in list(result["id"])


def test_nan_excluded_by_eq_filter(df_with_nan):
    """NaN != anything, so z = NaN rows must never match."""
    result, total = apply(df_with_nan.copy(), [flt("z", "=", "0.0")])
    assert total == 0   # z=0.0 is not in the fixture; NaN rows excluded too


def test_nan_excluded_by_gte_filter(df_with_nan):
    result, total = apply(df_with_nan.copy(), [flt("z", ">=", "0.5")])
    # z=0.5 (id=2) and z=1.2 (id=4) → 2 rows; NaN rows excluded
    assert total == 2
    assert set(result["id"]) == {2, 4}


def test_nan_passthrough_by_neq_filter(df_with_nan):
    """Pandas semantics: NaN != 0.5 evaluates to True, so NaN rows pass through !=.

    This is the standard pandas/numpy behaviour for inequality comparisons:
    only ordered comparisons (>, >=, <, <=, ==) exclude NaN.  Callers who want
    to drop NaN values before an != filter should pre-filter with isna().
    """
    result, total = apply(df_with_nan.copy(), [flt("z", "!=", "0.5")])
    ids = list(result["id"])
    # z=0.5 (id=2) is excluded by !=
    assert 2 not in ids
    # NaN rows pass through != (pandas: NaN != 0.5 is True)
    assert 3 in ids
    assert 5 in ids
    # valid non-0.5 rows are included
    assert 1 in ids
    assert 4 in ids


def test_nan_not_in_isin_filter(df_with_nan):
    """'in' filter must not return NaN rows."""
    result, total = apply(df_with_nan.copy(), [flt("z", "in", "0.1, 0.5")])
    assert total == 2
    assert 3 not in list(result["id"])
    assert 5 not in list(result["id"])


def test_sort_with_nan_puts_nan_last(df_with_nan):
    """Sorting by z ascending must push NaN rows to the end."""
    result, _ = apply(df_with_nan.copy(), sort=srt("z", "asc"))
    z_vals = list(result["z"])
    non_nan = [v for v in z_vals if math.isfinite(v)]
    nan_vals = [v for v in z_vals if not math.isfinite(v)]
    # Non-NaN values come first and in ascending order
    assert non_nan == sorted(non_nan)
    # NaN values are last
    assert z_vals.index(z_vals[-1]) > z_vals.index(non_nan[-1]) if nan_vals else True


def test_no_filter_includes_nan_rows(df_with_nan):
    """Without any filter, all rows (including NaN) are returned."""
    result, total = apply(df_with_nan.copy())
    assert total == 5
    assert len(result) == 5
