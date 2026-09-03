"""Geodesic LEVEL FIELDS for the interactive Reach slider.

The lasso validation on two cubes established that the useful ``reach`` depends
on a source's surface brightness, is not derivable from the header, and so has
to stay a control the user drags (validation/README.md). These tests pin the
property that makes dragging it cheap AND safe: one field computed up to
``reach_max``, thresholded locally, must give exactly the voxels the single-grow
path would have produced — otherwise the preview and the cube the user finally
applies would disagree.
"""

from __future__ import annotations

import numpy as np
import pytest

from app.compute.selection import (
    MAX_LEVEL_REACH,
    cube_kinematic_levels,
    cube_kinematic_select,
    cube_kinematic_select_multi,
    levels_to_mask,
)

REACH_MAX = 24.0


@pytest.fixture(scope="module")
def cube() -> np.ndarray:
    """Two Gaussian blobs with a faint bridge, on noise — enough structure that
    the budget actually changes the answer between steps."""
    rng = np.random.default_rng(20260903)
    nz, ny, nx = 24, 48, 48
    zz, yy, xx = np.mgrid[0:nz, 0:ny, 0:nx]

    def blob(z0, y0, x0, amp, s):
        return amp * np.exp(-(((zz - z0) ** 2 + (yy - y0) ** 2 + (xx - x0) ** 2) / (2.0 * s * s)))

    data = (blob(12, 16, 16, 8.0, 3.0)          # bright source
            + blob(12, 32, 32, 5.0, 3.0)        # second source
            + blob(12, 24, 24, 0.9, 6.0))       # faint bridge between them
    data += rng.normal(0.0, 0.1, size=data.shape)
    return data.astype(np.float32)


def _seed(cube: np.ndarray, z: int, y: int, x: int) -> tuple[int, int, int]:
    return (z, y, x)


# ── the property the whole design rests on ───────────────────────────────────

@pytest.mark.parametrize("reach", [1, 2, 3, 5, 8, 13, 21, 24])
def test_level_threshold_equals_a_direct_grow(cube, reach):
    """``levels <= r`` is the same voxel set as growing at budget r.

    Exact, not approximate: levels are ceil(distance), and for an INTEGER r
    ``ceil(d) <= r ⟺ d <= r``. Both sides use the same faint-margin shell
    (margin_reach), which is why this holds for r well below REACH_MAX.
    """
    seed = _seed(cube, 12, 16, 16)
    lv = cube_kinematic_levels(cube, [seed], [], reach_max=REACH_MAX, nsigma=2.5)
    from_levels = levels_to_mask(lv, reach, cube.shape)
    direct = cube_kinematic_select(cube, seed, reach=float(reach), nsigma=2.5,
                                   margin_reach=REACH_MAX)
    assert direct.any(), "the fixture should select something at every tested budget"
    np.testing.assert_array_equal(from_levels, direct)


def test_multi_seed_union_and_carve_match_the_direct_path(cube):
    """One add field and one sub field describe every seed combination, because a
    union of level sets is the level set of the pointwise minimum."""
    seeds = [_seed(cube, 12, 16, 16), _seed(cube, 12, 32, 32)]
    subs = [_seed(cube, 12, 32, 32)]
    lv = cube_kinematic_levels(cube, seeds, subs, reach_max=REACH_MAX, nsigma=2.5)
    for reach in (2, 5, 8, 16):
        direct = cube_kinematic_select_multi(
            cube, seeds, subs, reach=float(reach), nsigma=2.5, margin_reach=REACH_MAX)
        np.testing.assert_array_equal(levels_to_mask(lv, reach, cube.shape), direct)


def test_selection_grows_monotonically_with_the_budget(cube):
    """Raising the slider may only ADD voxels — a level set is nested by
    construction, and the UI relies on that to feel like a single control."""
    lv = cube_kinematic_levels(cube, [_seed(cube, 12, 16, 16)], [],
                               reach_max=REACH_MAX, nsigma=2.5)
    prev = None
    for reach in range(1, int(REACH_MAX) + 1):
        cur = levels_to_mask(lv, reach, cube.shape)
        if prev is not None:
            assert (cur | prev).sum() == cur.sum(), f"budget {reach} dropped voxels"
        prev = cur


# ── the field itself ─────────────────────────────────────────────────────────

def test_fields_are_uint8_over_a_shared_object_sized_box(cube):
    lv = cube_kinematic_levels(cube, [_seed(cube, 12, 16, 16)], [],
                               reach_max=REACH_MAX, nsigma=2.5)
    assert lv["add_levels"].dtype == np.uint8 and lv["sub_levels"].dtype == np.uint8
    assert lv["add_levels"].shape == lv["sub_levels"].shape == lv["shape"]
    z0, y0, x0 = lv["origin"]
    dz, dy, dx = lv["shape"]
    assert 0 <= z0 and z0 + dz <= cube.shape[0]
    assert 0 <= y0 and y0 + dy <= cube.shape[1]
    assert 0 <= x0 and x0 + dx <= cube.shape[2]


def test_field_is_bounded_by_the_object_not_the_cube():
    """The out-of-core promise: a small source in a large cube must not produce a
    cube-sized field. (The shared fixture is too small to show this — its faint
    margin reaches every edge, which is itself the object being cube-sized.)"""
    rng = np.random.default_rng(7)
    data = rng.normal(0.0, 0.1, size=(40, 160, 160)).astype(np.float32)
    zz, yy, xx = np.mgrid[0:40, 0:160, 0:160]
    data += 8.0 * np.exp(-(((zz - 20) ** 2 + (yy - 30) ** 2 + (xx - 30) ** 2) / 8.0))
    lv = cube_kinematic_levels(data, [(20, 30, 30)], [], reach_max=8.0, nsigma=2.5)
    assert np.prod(lv["shape"]) < data.size / 10, (
        f"field {lv['shape']} should track the blob, not the {data.shape} cube")


def test_no_sub_seeds_means_nothing_is_carved(cube):
    lv = cube_kinematic_levels(cube, [_seed(cube, 12, 16, 16)], [],
                               reach_max=REACH_MAX, nsigma=2.5)
    # The sentinel everywhere: `sub <= r` is empty for any budget.
    assert (lv["sub_levels"] == lv["unreachable"]).all()


def test_seed_is_level_zero(cube):
    seed = _seed(cube, 12, 16, 16)
    lv = cube_kinematic_levels(cube, [seed], [], reach_max=REACH_MAX, nsigma=2.5)
    z0, y0, x0 = lv["origin"]
    assert lv["add_levels"][seed[0] - z0, seed[1] - y0, seed[2] - x0] == 0
    # ...so even the smallest budget selects the clicked voxel.
    assert levels_to_mask(lv, 0, cube.shape)[seed]


def test_beyond_the_budget_is_the_unreachable_sentinel(cube):
    lv = cube_kinematic_levels(cube, [_seed(cube, 12, 16, 16)], [],
                               reach_max=8.0, nsigma=2.5)
    lvl = lv["add_levels"]
    assert lvl.max() == lv["unreachable"], "some voxels must lie beyond a budget of 8"
    real = lvl[lvl != lv["unreachable"]]
    assert real.max() <= 8, "no real level may exceed the requested budget"


# ── input handling ───────────────────────────────────────────────────────────

def test_rejects_a_budget_a_uint8_field_cannot_carry(cube):
    with pytest.raises(ValueError, match="uint8"):
        cube_kinematic_levels(cube, [_seed(cube, 12, 16, 16)], [],
                              reach_max=MAX_LEVEL_REACH + 1.0)


@pytest.mark.parametrize("bad", [0.0, -1.0, float("nan"), float("inf")])
def test_rejects_a_non_positive_or_non_finite_budget(cube, bad):
    with pytest.raises(ValueError):
        cube_kinematic_levels(cube, [_seed(cube, 12, 16, 16)], [], reach_max=bad)


def test_rejects_an_empty_seed_list(cube):
    with pytest.raises(ValueError, match="seed"):
        cube_kinematic_levels(cube, [], [], reach_max=REACH_MAX)


def test_a_blank_sub_seed_is_skipped_not_fatal(cube):
    """Same tolerance the multi-seed grow already has: ⌥-clicking empty sky
    removes nothing rather than failing the whole selection."""
    blank = cube.copy()
    blank[0, 0, 0] = np.nan
    lv = cube_kinematic_levels(blank, [_seed(cube, 12, 16, 16)], [(0, 0, 0)],
                               reach_max=REACH_MAX, nsigma=2.5)
    assert (lv["sub_levels"] == lv["unreachable"]).all()


def test_a_blank_add_seed_fails_even_when_another_seed_is_good(cube):
    """A bad ADD seed must not be silently dropped just because a sibling grew.

    The multi-seed grow raises for it, so tolerating it here would let the two
    paths disagree — and would hand back a plausible-looking field built from the
    other seeds while the user's click did nothing.
    """
    blank = cube.copy()
    blank[12, 16, 16] = np.nan
    with pytest.raises(ValueError):
        cube_kinematic_levels(blank, [(12, 16, 16), (12, 32, 32)], [],
                              reach_max=REACH_MAX, nsigma=2.5)
    # ...and it is the same verdict the direct path gives for those seeds.
    with pytest.raises(ValueError):
        cube_kinematic_select_multi(blank, [(12, 16, 16), (12, 32, 32)], [],
                                    reach=8.0, nsigma=2.5)


def test_levels_are_computed_in_float64(cube):
    """The quantisation must see SciPy's own distances: narrowing to float32 first
    can move a value just above an integer budget down onto it before ceil()."""
    lv = cube_kinematic_levels(cube, [_seed(cube, 12, 16, 16)], [],
                               reach_max=REACH_MAX, nsigma=2.5)
    # Exactness against the float64 direct path is the observable consequence.
    for reach in (3, 7, 11, 19):
        direct = cube_kinematic_select(cube, _seed(cube, 12, 16, 16), reach=float(reach),
                                       nsigma=2.5, margin_reach=REACH_MAX)
        np.testing.assert_array_equal(levels_to_mask(lv, reach, cube.shape), direct)


def test_a_blank_primary_seed_is_reported(cube):
    blank = cube.copy()
    blank[12, 16, 16] = np.nan
    with pytest.raises(ValueError, match="blank"):
        cube_kinematic_levels(blank, [_seed(cube, 12, 16, 16)], [], reach_max=REACH_MAX)
