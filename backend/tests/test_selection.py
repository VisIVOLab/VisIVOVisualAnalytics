"""Tests for the Kinematic Lasso geodesic 3-D selection (app/compute/selection.py)."""
import numpy as np
import pytest

from app.compute.selection import cube_kinematic_select


def _blob_cube(nz=40, ny=40, nx=40, centers=((20, 20, 20),), amp=12.0, width=3.5,
               noise=0.1, seed=0):
    rng = np.random.default_rng(seed)
    zz, yy, xx = np.mgrid[0:nz, 0:ny, 0:nx]
    data = rng.normal(0.0, noise, (nz, ny, nx)).astype(np.float64)
    for (cz, cy, cx) in centers:
        r2 = (zz - cz) ** 2 + (yy - cy) ** 2 + (xx - cx) ** 2
        data += amp * np.exp(-r2 / (2.0 * width ** 2))
    return data


def test_select_grabs_the_blob_not_the_corner():
    data = _blob_cube(centers=((20, 20, 20),))
    mask = cube_kinematic_select(data, (20, 20, 20), reach=6.0, lambda_v=1.0)
    assert mask.shape == data.shape
    assert mask[20, 20, 20]            # the seed / blob core is selected
    assert not mask[0, 0, 0]           # a far corner (pure noise) is not
    n = int(mask.sum())
    assert 20 < n < data.size // 4     # a compact object, not the whole cube


def test_select_isolates_one_of_two_separated_blobs():
    data = _blob_cube(centers=((10, 10, 10), (30, 30, 30)), width=3.0)
    mask = cube_kinematic_select(data, (10, 10, 10), reach=6.0, lambda_v=1.0)
    assert mask[10, 10, 10]            # the clicked blob
    assert not mask[30, 30, 30]        # the other blob is NOT swept in (a real gap)


def test_lambda_v_controls_velocity_extent():
    """A bright filament ALONG the velocity axis (z): a low velocity weight
    (cheap z steps) selects more of it than a high weight."""
    nz, ny, nx = 60, 24, 24
    rng = np.random.default_rng(1)
    data = rng.normal(0.0, 0.1, (nz, ny, nx)).astype(np.float64)
    data[10:50, 12, 12] += 12.0        # bright line in z at (y,x)=(12,12)
    seed = (10, 12, 12)
    low = cube_kinematic_select(data, seed, reach=8.0, lambda_v=0.2)
    high = cube_kinematic_select(data, seed, reach=8.0, lambda_v=5.0)
    # Cheaper velocity steps → the selection reaches further along z.
    assert int(low.sum()) > int(high.sum())
    zlo = np.where(low.any(axis=(1, 2)))[0]
    zhi = np.where(high.any(axis=(1, 2)))[0]
    assert (zlo.max() - zlo.min()) >= (zhi.max() - zhi.min())


def test_reach_grows_the_selection():
    data = _blob_cube(centers=((20, 20, 20),), width=5.0)
    small = cube_kinematic_select(data, (20, 20, 20), reach=3.0, lambda_v=1.0)
    big = cube_kinematic_select(data, (20, 20, 20), reach=15.0, lambda_v=1.0)
    assert int(big.sum()) >= int(small.sum())


def test_roi_radius_bounds_the_selection():
    """A tiny ROI confines the selection near the seed regardless of reach."""
    data = _blob_cube(centers=((20, 20, 20),), width=6.0)
    mask = cube_kinematic_select(data, (20, 20, 20), reach=100.0, lambda_v=1.0,
                                 roi_radius=3)
    zz, yy, xx = np.where(mask)
    assert xx.max() - xx.min() <= 6    # ≤ 2*roi_radius
    assert zz.max() - zz.min() <= 6


def test_seed_outside_raises():
    data = _blob_cube()
    with pytest.raises(ValueError):
        cube_kinematic_select(data, (99, 0, 0), reach=6.0)


def test_nan_acts_as_barrier():
    """NaN voxels block the grow (they are unobserved / masked)."""
    data = _blob_cube(centers=((20, 20, 20),), width=4.0)
    data[20, 20, 25:] = np.nan          # a NaN wall on the +x side of the seed
    mask = cube_kinematic_select(data, (20, 20, 20), reach=10.0, lambda_v=1.0)
    assert not mask[20, 20, 27]         # cannot cross the NaN wall
