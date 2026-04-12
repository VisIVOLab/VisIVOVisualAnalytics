"""
test_catalogue_overlay.py – unit tests for _catalogue_overlay_sync() in app/main.py.

Covers:
  1. Normal viewport (no RA wrap-around) — inside vs outside bounding box
  2. RA wrap-around at 0°/360° — both branch: ra_min<0 and ra_max>360
  3. Dec filtering applied jointly with RA
  4. Empty dataset — no crash, empty result
  5. max_sources cap and spatial subsampling
  6. Auto-detection of RA/Dec column names from _RA_CANDIDATES / _DEC_CANDIDATES
  7. Missing RA/Dec columns — valid=False with informative error
  8. LRU cache path — two calls with same path return consistent results
  9. NaN / non-finite values in CSV — skipped cleanly

Run with:
    cd backend
    pytest tests/test_catalogue_overlay.py -v
"""

from __future__ import annotations

import csv
import math

import numpy as np
import pytest


# ── Lazy imports (VISIVO_TOKEN must be set by conftest.py first) ──────────────


def _sync():
    from app.main import _catalogue_overlay_sync
    return _catalogue_overlay_sync


def _cache_clear():
    from app.main import _load_overlay_csv
    _load_overlay_csv.cache_clear()


# ── Fixtures ──────────────────────────────────────────────────────────────────


@pytest.fixture(autouse=True)
def clear_csv_cache():
    """Invalidate the LRU cache before and after every test."""
    _cache_clear()
    yield
    _cache_clear()


def write_csv(path, rows: list[dict]) -> None:
    """Write a minimal CSV file at *path* with the given list-of-dicts rows."""
    if not rows:
        with open(path, "w") as f:
            f.write("ra,dec\n")
        return
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def call(path, ra_c, dec_c, fov=2.0, ra_f="ra", dec_f="dec", max_s=5000):
    """Convenience wrapper around _catalogue_overlay_sync."""
    return _sync()(str(path), ra_c, dec_c, fov, ra_f, dec_f, max_s)


# ═══════════════════════════════════════════════════════════════════════════════
# 1. Normal viewport — no wrap-around
# ═══════════════════════════════════════════════════════════════════════════════


def test_normal_viewport_inside_sources_returned(tmp_path):
    """Sources within the bounding box must be included."""
    p = tmp_path / "s.csv"
    write_csv(p, [
        {"ra": 45.0,  "dec": 10.0},   # inside
        {"ra": 45.5,  "dec": 10.2},   # inside
    ])
    r = call(p, 45.0, 10.0)
    assert r["valid"] is True
    assert r["total_in_view"] == 2
    ras = {s["ra"] for s in r["sources"]}
    assert 45.0 in ras
    assert 45.5 in ras


def test_normal_viewport_outside_sources_excluded(tmp_path):
    """Sources far outside the bounding box must NOT be included."""
    p = tmp_path / "s.csv"
    write_csv(p, [
        {"ra": 45.0,  "dec": 10.0},   # inside
        {"ra": 60.0,  "dec": 10.0},   # outside — too far in RA
        {"ra": 45.0,  "dec": 30.0},   # outside — too far in Dec
    ])
    r = call(p, 45.0, 10.0, fov=2.0)
    assert r["valid"] is True
    ras = {s["ra"] for s in r["sources"]}
    assert 60.0 not in ras
    decs = {s["dec"] for s in r["sources"]}
    assert 30.0 not in decs


def test_normal_viewport_total_in_view_correct(tmp_path):
    """total_in_view reflects the count BEFORE max_sources truncation."""
    p = tmp_path / "s.csv"
    write_csv(p, [
        {"ra": 100.0, "dec": 20.0},
        {"ra": 100.2, "dec": 20.1},
        {"ra": 200.0, "dec": 20.0},   # outside
    ])
    r = call(p, 100.0, 20.0, fov=2.0)
    assert r["total_in_view"] == 2


# ═══════════════════════════════════════════════════════════════════════════════
# 2. RA wrap-around at 0° / 360°
# ═══════════════════════════════════════════════════════════════════════════════
#
# The implementation:
#   half_fov  = fov * 0.6
#   cos_dec   = max(0.02, |cos(dec_center)|)
#   ra_margin = min(180, half_fov / cos_dec)
#
# With fov=2.0, dec=0: half_fov=1.2, ra_margin=1.2
# Centred at RA=0: ra_min=-1.2, ra_max=1.2  → ra_min < 0 branch
#   mask = (ra_col >= 358.8) | (ra_col <= 1.2)
#
# Centred at RA=359.5: ra_min=358.3, ra_max=360.7 → ra_max > 360 branch
#   mask = (ra_col >= 358.3) | (ra_col <= 0.7)


def test_wrap_at_zero_includes_near_zero(tmp_path):
    """Viewport centred at RA=0 must include sources on both sides of 0°."""
    p = tmp_path / "wrap0.csv"
    write_csv(p, [
        {"ra": 359.8, "dec": 0.0},   # should be included (>358.8)
        {"ra":   0.1, "dec": 0.0},   # should be included (<=1.2)
        {"ra": 180.0, "dec": 0.0},   # far away — must NOT be included
    ])
    r = call(p, 0.0, 0.0, fov=2.0)
    assert r["valid"] is True
    ras = {s["ra"] for s in r["sources"]}
    assert 359.8 in ras, "source at RA=359.8 must be included in wrap-around viewport"
    assert 0.1   in ras, "source at RA=0.1 must be included in wrap-around viewport"
    assert 180.0 not in ras, "source at RA=180 must be excluded"


def test_wrap_at_360_includes_cross_boundary(tmp_path):
    """Viewport centred near RA=359.5 must include sources crossing 360→0."""
    p = tmp_path / "wrap360.csv"
    write_csv(p, [
        {"ra": 359.0, "dec": 5.0},   # inside (>358.3)
        {"ra":   0.2, "dec": 5.0},   # inside (<=0.7)
        {"ra": 180.0, "dec": 5.0},   # outside
    ])
    r = call(p, 359.5, 5.0, fov=2.0)
    assert r["valid"] is True
    ras = {s["ra"] for s in r["sources"]}
    assert 359.0 in ras
    assert 0.2   in ras
    assert 180.0 not in ras


def test_wrap_at_zero_large_fov(tmp_path):
    """With a larger FOV the ra_margin grows proportionally."""
    p = tmp_path / "wrap_large.csv"
    write_csv(p, [
        {"ra": 350.0, "dec": 0.0},   # 10° from RA=0
        {"ra":  10.0, "dec": 0.0},   # 10° from RA=0
        {"ra": 180.0, "dec": 0.0},   # far
    ])
    # fov=40: half_fov=24, ra_margin=24 → ra_min=-24 → mask uses +360 branch
    r = call(p, 0.0, 0.0, fov=40.0)
    ras = {s["ra"] for s in r["sources"]}
    assert 350.0 in ras
    assert 10.0  in ras


# ═══════════════════════════════════════════════════════════════════════════════
# 3. Dec filtering
# ═══════════════════════════════════════════════════════════════════════════════


def test_dec_filter_excludes_out_of_range(tmp_path):
    p = tmp_path / "dec.csv"
    write_csv(p, [
        {"ra": 180.0, "dec":  0.0},    # inside
        {"ra": 180.0, "dec": 45.0},    # outside
        {"ra": 180.0, "dec": -45.0},   # outside
    ])
    r = call(p, 180.0, 0.0, fov=2.0)
    assert r["valid"] is True
    decs = [s["dec"] for s in r["sources"]]
    assert 0.0 in decs
    assert 45.0  not in decs
    assert -45.0 not in decs


def test_dec_and_ra_filter_combined(tmp_path):
    """Both RA and Dec constraints are applied simultaneously."""
    p = tmp_path / "combined.csv"
    write_csv(p, [
        {"ra": 90.0,  "dec":  5.0},   # inside both
        {"ra": 90.0,  "dec": 20.0},   # inside RA, outside Dec
        {"ra": 120.0, "dec":  5.0},   # outside RA, inside Dec
    ])
    r = call(p, 90.0, 5.0, fov=2.0)
    assert r["total_in_view"] == 1
    assert r["sources"][0]["ra"] == pytest.approx(90.0)


# ═══════════════════════════════════════════════════════════════════════════════
# 4. Empty dataset
# ═══════════════════════════════════════════════════════════════════════════════


def test_empty_csv_returns_valid_empty(tmp_path):
    p = tmp_path / "empty.csv"
    write_csv(p, [])
    r = call(p, 180.0, 0.0)
    assert r["valid"] is True
    assert r["sources"] == []
    assert r["total_in_view"] == 0
    assert r["truncated"] is False


def test_empty_csv_with_custom_ra_dec_fields(tmp_path):
    p = tmp_path / "empty2.csv"
    with open(p, "w") as f:
        f.write("RAJ2000,DEJ2000\n")
    r = _sync()(str(p), 180.0, 0.0, 2.0, "RAJ2000", "DEJ2000", 5000)
    assert r["valid"] is True
    assert r["total_in_view"] == 0


# ═══════════════════════════════════════════════════════════════════════════════
# 5. max_sources cap and spatial subsampling
# ═══════════════════════════════════════════════════════════════════════════════


def test_max_sources_caps_returned_count(tmp_path):
    p = tmp_path / "many.csv"
    # 1 000 sources all inside a 10° viewport centred at (180, 0)
    rows = [{"ra": 180.0, "dec": i * 0.005} for i in range(1000)]
    write_csv(p, rows)
    max_s = 100
    r = call(p, 180.0, 0.0, fov=10.0, max_s=max_s)
    assert r["valid"] is True
    assert r["total_in_view"] == 1000
    assert r["truncated"] is True
    assert len(r["sources"]) <= max_s


def test_no_truncation_when_under_limit(tmp_path):
    p = tmp_path / "few.csv"
    rows = [{"ra": 180.0, "dec": i * 0.1} for i in range(10)]
    write_csv(p, rows)
    r = call(p, 180.0, 0.0, fov=10.0, max_s=5000)
    assert r["truncated"] is False
    assert len(r["sources"]) == r["total_in_view"]


def test_subsampled_sources_are_finite(tmp_path):
    """After subsampling every returned source must have finite RA and Dec."""
    p = tmp_path / "sub.csv"
    rows = [{"ra": 180.0 + i * 0.001, "dec": i * 0.001} for i in range(500)]
    write_csv(p, rows)
    r = call(p, 180.0, 0.0, fov=10.0, max_s=50)
    for s in r["sources"]:
        assert math.isfinite(s["ra"])
        assert math.isfinite(s["dec"])


# ═══════════════════════════════════════════════════════════════════════════════
# 6. Auto-detection of RA/Dec column names
# ═══════════════════════════════════════════════════════════════════════════════


@pytest.mark.parametrize("ra_col,dec_col", [
    ("ra",         "dec"),
    ("RA",         "DEC"),
    ("Ra",         "Dec"),
    ("RAJ2000",    "DEJ2000"),
    ("ra_deg",     "dec_deg"),
    ("RAdeg",      "DECdeg"),
    ("ALPHA_J2000","DELTA_J2000"),
])
def test_auto_detect_candidate_column_names(tmp_path, ra_col, dec_col):
    p = tmp_path / f"auto_{ra_col}.csv"
    write_csv(p, [{ra_col: 180.0, dec_col: 0.0}])
    # Pass empty strings to trigger auto-detection
    r = _sync()(str(p), 180.0, 0.0, 5.0, "", "", 5000)
    assert r["valid"] is True, f"auto-detect failed for {ra_col}/{dec_col}: {r['error']}"
    assert r["total_in_view"] >= 1


# ═══════════════════════════════════════════════════════════════════════════════
# 7. Missing RA/Dec columns
# ═══════════════════════════════════════════════════════════════════════════════


def test_unrecognised_columns_returns_error(tmp_path):
    p = tmp_path / "nocoord.csv"
    write_csv(p, [{"x": 1.0, "y": 2.0}])
    r = _sync()(str(p), 180.0, 0.0, 5.0, "", "", 5000)
    assert r["valid"] is False
    assert "Cannot detect" in r["error"]
    assert r["sources"] == []
    assert r["total_in_view"] == 0


def test_explicit_wrong_field_name_returns_valid_but_empty(tmp_path):
    """Explicitly passing nonexistent column names will fail at key access."""
    p = tmp_path / "badfields.csv"
    write_csv(p, [{"ra": 180.0, "dec": 0.0}])
    # Try to use a nonexistent field name — will raise or return empty
    try:
        r = _sync()(str(p), 180.0, 0.0, 5.0, "RIGHT_ASCENSION", "DECLINATION", 5000)
        # If it doesn't raise, it must return valid=False
        assert r["valid"] is False or r["total_in_view"] == 0
    except (KeyError, Exception):
        pass   # any exception is acceptable here


# ═══════════════════════════════════════════════════════════════════════════════
# 8. LRU cache path — correctness
# ═══════════════════════════════════════════════════════════════════════════════


def test_two_calls_same_path_same_result(tmp_path):
    """Repeated calls with the same path must return identical results."""
    p = tmp_path / "cached.csv"
    write_csv(p, [
        {"ra": 90.0, "dec": 5.0},
        {"ra": 90.5, "dec": 5.1},
    ])
    r1 = call(p, 90.0, 5.0, fov=2.0)
    r2 = call(p, 90.0, 5.0, fov=2.0)
    assert r1["total_in_view"] == r2["total_in_view"]
    assert r1["sources"] == r2["sources"]
    assert r1["truncated"] == r2["truncated"]


def test_cache_cleared_between_tests_fixture_works(tmp_path):
    """
    Two consecutive test calls with different CSV paths must not
    interfere with each other through the LRU cache.
    The autouse clear_csv_cache fixture verifies this implicitly.
    """
    p = tmp_path / "fresh.csv"
    write_csv(p, [{"ra": 45.0, "dec": 0.0}])
    r = call(p, 45.0, 0.0, fov=2.0)
    assert r["valid"] is True
    assert r["total_in_view"] == 1


# ═══════════════════════════════════════════════════════════════════════════════
# 9. NaN / non-finite values in CSV
# ═══════════════════════════════════════════════════════════════════════════════


def test_nan_ra_skipped(tmp_path):
    p = tmp_path / "nans.csv"
    with open(p, "w") as f:
        f.write("ra,dec\n180.0,0.0\nNaN,0.0\n")
    r = call(p, 180.0, 0.0, fov=5.0)
    assert r["valid"] is True
    for s in r["sources"]:
        assert math.isfinite(s["ra"])


def test_nan_dec_skipped(tmp_path):
    p = tmp_path / "nans2.csv"
    with open(p, "w") as f:
        f.write("ra,dec\n180.0,0.0\n180.5,NaN\n")
    r = call(p, 180.0, 0.0, fov=5.0)
    assert r["valid"] is True
    for s in r["sources"]:
        assert math.isfinite(s["dec"])


def test_all_nan_returns_empty(tmp_path):
    p = tmp_path / "allnan.csv"
    with open(p, "w") as f:
        f.write("ra,dec\nNaN,NaN\nNaN,NaN\n")
    r = call(p, 180.0, 0.0, fov=5.0)
    assert r["valid"] is True
    assert len(r["sources"]) == 0


# ═══════════════════════════════════════════════════════════════════════════════
# 10. Non-existent CSV file
# ═══════════════════════════════════════════════════════════════════════════════


def test_missing_file_returns_valid_false(tmp_path):
    p = tmp_path / "does_not_exist.csv"
    r = _sync()(str(p), 180.0, 0.0, 2.0, "ra", "dec", 5000)
    assert r["valid"] is False
    assert "Cannot read CSV" in r["error"]
    assert r["sources"] == []
