"""Smooth-before-threshold on the kinematic lasso: what it buys, and what it costs.

The knob came from a measured disagreement. On WALLABY's NGC 3137 the SoFiA-2
mask holds 299,562 voxels while the lasso's 2.5-sigma connected component around
the clicked voxel holds 47,994 — 16 per cent. The gap was first blamed on the
geodesic budget and that was wrong: raising ``reach`` to 64 does reach the same
extent, but so does the component itself, because the limit was never the budget.
It is the per-voxel threshold, which cannot see a smooth faint envelope that a
smooth-and-clip finder sees easily.

So the tests here pin two things, and the second matters more than the first:
smoothing closes that gap, and smoothing must never become the default, because
on compact sources in a crowded field it destroys the precision the lasso exists
for.
"""
from __future__ import annotations

import numpy as np
import pytest


def _cube_with_envelope() -> tuple[np.ndarray, tuple[int, int, int]]:
    """A bright core inside a broad faint envelope, plus noise.

    The envelope sits below 2.5 sigma per voxel but well above it once smoothed —
    which is precisely the regime the knob exists for.
    """
    rng = np.random.default_rng(11)
    nz, ny, nx = 24, 64, 64
    data = rng.normal(0.0, 1.0, (nz, ny, nx)).astype(np.float32)
    zz, yy, xx = np.mgrid[0:nz, 0:ny, 0:nx]
    r2 = ((zz - 12) / 5.0) ** 2 + ((yy - 32) / 16.0) ** 2 + ((xx - 32) / 16.0) ** 2
    data += (12.0 * np.exp(-r2 * 4.0)).astype(np.float32)   # core, far above 2.5 sigma
    data += (1.2 * np.exp(-r2 * 0.5)).astype(np.float32)    # envelope, below it
    return data, (12, 32, 32)


class TestSmoothingIsOffByDefault:
    def test_default_is_byte_for_byte_the_unsmoothed_result(self):
        """Every published number was measured with it off; None and 0 must agree."""
        from app.compute.selection import cube_kinematic_select

        data, seed = _cube_with_envelope()
        a = cube_kinematic_select(data, seed, reach=8.0)
        b = cube_kinematic_select(data, seed, reach=8.0, smooth_zyx=None)
        c = cube_kinematic_select(data, seed, reach=8.0, smooth_zyx=(0.0, 0.0, 0.0))
        assert np.array_equal(a, b)
        assert np.array_equal(a, c)


class TestSmoothingReachesTheFaintEnvelope:
    def test_it_selects_more_than_the_raw_threshold_can(self):
        from app.compute.selection import cube_kinematic_select

        data, seed = _cube_with_envelope()
        raw = int(cube_kinematic_select(data, seed, reach=8.0).sum())
        smoothed = int(cube_kinematic_select(
            data, seed, reach=8.0, smooth_zyx=(1.0, 3.0, 3.0)).sum())
        assert smoothed > raw, (raw, smoothed)

    def test_the_threshold_is_re_measured_on_the_smoothed_data(self):
        """Smoothing lowers the noise, so reusing the raw sigma would make nsigma
        mean a different thing for every kernel — and the selection would explode
        rather than grow."""
        from app.compute.selection import cube_kinematic_select

        data, seed = _cube_with_envelope()
        n = data.size
        for sm in ((1.0, 2.0, 2.0), (1.0, 3.0, 3.0), (2.0, 4.0, 4.0)):
            sel = int(cube_kinematic_select(data, seed, reach=8.0, smooth_zyx=sm).sum())
            assert 0 < sel < n, (sm, sel)

    def test_blank_voxels_stay_barriers(self):
        """The finite mask is taken from the RAW window, so NaN cannot be smoothed
        into something selectable."""
        from app.compute.selection import cube_kinematic_select

        data, seed = _cube_with_envelope()
        data = data.copy()
        data[:, :, 48:] = np.nan
        sel = cube_kinematic_select(data, seed, reach=8.0, smooth_zyx=(1.0, 3.0, 3.0))
        assert not sel[:, :, 48:].any()


class TestWhySmoothingIsNotTheDefault:
    """The measurement that decided it — and what it does and does not say.

    Against our own, conservative SoFiA-2 mask of HCG 44 (4 sigma, reliability
    0.9), smoothing (1, 3, 3) takes completeness to ~1.0 and Dice DOWN, from
    0.84/0.83/0.74/0.66/0.69 to 0.59/0.53/0.49/0.53/0.57, because the selection
    grows two to three times that mask. Against the PUBLISHED mask of the same
    cube (Serra et al. 2013), which is itself two to three times larger than our
    SoFiA-2 run, the same smoothing raises Dice on every source, 0.61->0.73,
    0.69->0.75, 0.52->0.65, 0.47->0.73, 0.53->0.71. So the knob moves the
    selection across the range two reasonable finder configurations span; it is
    not "harmful" in itself. It stays off by default for one reason, pinned
    below: a selector's first duty is not to bleed into a neighbour, and the
    default's reliability (0.94-0.97 against either mask) is what guarantees
    that. Both harness runs are in validation/out/.
    """

    def test_two_nearby_sources_are_merged_by_a_large_kernel(self):
        from app.compute.selection import cube_kinematic_select

        rng = np.random.default_rng(5)
        nz, ny, nx = 20, 48, 96
        data = rng.normal(0.0, 1.0, (nz, ny, nx)).astype(np.float32)
        zz, yy, xx = np.mgrid[0:nz, 0:ny, 0:nx]
        # Two compact sources 22 voxels apart. The separation is chosen from a
        # measurement, not by eye: with a (2, 6, 6) kernel the smoothed selection
        # crosses the midpoint for separations up to roughly 28 voxels and stops
        # doing so by 36, so 22 sits inside the regime this test is about.
        cx_a, cx_b = 30, 52
        for cx in (cx_a, cx_b):
            r2 = ((zz - 10) / 3.0) ** 2 + ((yy - 24) / 5.0) ** 2 + ((xx - cx) / 5.0) ** 2
            data += (14.0 * np.exp(-r2 * 2.0)).astype(np.float32)
        seed = (10, 24, cx_a)
        midpoint = (cx_a + cx_b) // 2

        raw = cube_kinematic_select(data, seed, reach=8.0)
        assert not raw[:, :, midpoint:].any(), (
            "the default must keep the two sources apart")

        merged = cube_kinematic_select(data, seed, reach=8.0, smooth_zyx=(2.0, 6.0, 6.0))
        assert merged[:, :, midpoint:].sum() > 1000, (
            "this is the documented cost of smoothing: it bridges the gap between "
            "neighbouring sources, which is why it is not the default")
