"""Display-LOD geometry of a block-averaged velocity field.

Two review findings:

* a thin axis (shorter than the reduction factor) used to make the block
  average return the ORIGINAL array while the caller still multiplied the
  spacing, so a (2, 128, 128) grid kept its dimensions but its spacing went
  from 1 to 4;
* the reduced grid kept the original origin, but the first block mean is the
  value at the centre of the first block, (factor - 1) / 2 cells further in,
  so the whole reduced field was shifted by half a block.

The grid is point-centred (``origin`` is the first sample, ``spacing`` the
distance between samples), so a block mean of f samples sits at
``origin + (f - 1) / 2 * spacing`` and the reduced spacing is ``f * spacing``.
"""
from __future__ import annotations

import numpy as np
import pytest
from astropy.io import fits

from app.velocity_field import (
    VelocityField,
    downsample,
    velocity_grid_worker,
)


def _field(shape, spacing=(1.0, 1.0, 1.0), origin=(0.0, 0.0, 0.0)):
    """vx = x, vy = y, vz = z in WORLD coordinates: a linear field, so a block
    mean equals the field at the block centre and any misplacement shows."""
    nz, ny, nx = shape
    z, y, x = np.mgrid[0:nz, 0:ny, 0:nx].astype(np.float64)
    wx = origin[0] + x * spacing[0]
    wy = origin[1] + y * spacing[1]
    wz = origin[2] + z * spacing[2]
    return VelocityField(wx.astype(np.float32), wy.astype(np.float32),
                         wz.astype(np.float32), spacing, origin, "km/s", "test", False)


def test_thin_axis_keeps_spacing_consistent_with_shape():
    f = _field((2, 128, 128))
    d = downsample(f, max_dim=32)
    nz, ny, nx = d.shape
    assert (ny, nx) == (32, 32)          # long axes ARE reduced
    assert nz == 2                       # the thin axis is left alone
    assert d.spacing == pytest.approx((4.0, 4.0, 1.0))
    # The last sample is the centre of the last block (samples 124..127).
    assert (nx - 1) * d.spacing[0] + d.origin[0] == pytest.approx(125.5)


def test_reduced_samples_sit_at_their_block_centres():
    f = _field((16, 16, 16), spacing=(2.0, 3.0, 5.0), origin=(-10.0, 4.0, 7.0))
    d = downsample(f, max_dim=8)         # factor 2 on every axis
    assert d.spacing == pytest.approx((4.0, 6.0, 10.0))
    # First block mean of a linear field = value at the block centre.
    assert d.origin == pytest.approx((-10.0 + 1.0, 4.0 + 1.5, 7.0 + 2.5))
    assert float(d.vx[0, 0, 0]) == pytest.approx(d.origin[0])
    assert float(d.vy[0, 0, 0]) == pytest.approx(d.origin[1])
    assert float(d.vz[0, 0, 0]) == pytest.approx(d.origin[2])
    # And every sample: the field value equals its world position.
    nz, ny, nx = d.shape
    k, j, i = 3, 5, 7
    assert float(d.vx[k, j, i]) == pytest.approx(d.origin[0] + i * d.spacing[0])
    assert float(d.vy[k, j, i]) == pytest.approx(d.origin[1] + j * d.spacing[1])
    assert float(d.vz[k, j, i]) == pytest.approx(d.origin[2] + k * d.spacing[2])


def test_short_axis_still_reduced_when_two_samples_survive():
    f = _field((6, 64, 64))
    d = downsample(f, max_dim=16)        # factor 4 overall, 6 // 4 = 1 sample
    # The short axis is reduced only as far as two samples survive (factor 3).
    assert d.shape == (2, 16, 16)
    assert d.spacing == pytest.approx((4.0, 4.0, 3.0))
    assert float(d.vz[0, 0, 0]) == pytest.approx(d.origin[2])
    assert float(d.vz[1, 0, 0]) == pytest.approx(d.origin[2] + d.spacing[2])


def test_grid_worker_reports_per_axis_factors(tmp_path):
    f = _field((2, 128, 128))
    data = np.stack([f.vx, f.vy, f.vz]).astype(np.float32)
    path = tmp_path / "thin.fits"
    fits.PrimaryHDU(data).writeto(path)
    p = velocity_grid_worker(str(path), None, max_dim=32)
    assert (p["nx"], p["ny"], p["nz"]) == (32, 32, 2)
    assert p["downsample_factors"] == [4, 4, 1]      # x, y, z
    assert p["downsample_factor"] == 4               # largest, as before
    assert p["spacing"] == pytest.approx([4.0, 4.0, 1.0])
    # Header-less load: unit spacing, grid centred on 0 (first sample -63.5).
    assert p["origin"] == pytest.approx([-63.5 + 1.5, -63.5 + 1.5, -0.5])
