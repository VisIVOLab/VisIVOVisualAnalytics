"""The isosurface sent to the viewer is bounded (issue #83): a large cube is
sampled with a stride, coarsened while the surface has too many triangles, and
the points stay in the full cube's voxel coordinates."""
from __future__ import annotations

import base64
import zlib

import numpy as np
import pytest
from astropy.io import fits

from app.compute import pv_iso


def _ball(tmp_path, n=64):
    z, y, x = np.mgrid[:n, :n, :n].astype(np.float32)
    r = np.sqrt((x - n / 2) ** 2 + (y - n / 2) ** 2 + (z - n / 2) ** 2)
    path = tmp_path / "ball.fits"
    fits.PrimaryHDU(np.exp(-r / 8.0).astype(np.float32)).writeto(path)
    return str(path)


def _points(result):
    raw = base64.b64decode(result["points_base64"])
    if result["compression"]:
        raw = zlib.decompress(raw[4:])   # Qt's qCompress: 4-byte length prefix
    return np.frombuffer(raw, dtype=np.float32).reshape(-1, 3)


def test_the_stride_keeps_the_grid_within_the_budget():
    assert pv_iso._iso_stride((100, 100, 100), 10 ** 6) == (1, 1, 1)
    assert pv_iso._iso_stride((512, 512, 512)) == (2, 2, 2)
    # A radio cube keeps its 40 channels; its long axes are sampled.
    assert pv_iso._iso_stride((40, 1024, 1024)) == (1, 2, 2)


def test_coarsening_spares_a_short_axis_then_stops():
    assert pv_iso._coarser((40, 1024, 1024), (1, 2, 2)) == (1, 4, 4)
    assert pv_iso._coarser((40, 40, 40), (1, 1, 1)) is None


def test_a_sampled_surface_stays_in_full_cube_coordinates(tmp_path, monkeypatch):
    pytest.importorskip("vtk")
    path = _ball(tmp_path)
    full = pv_iso.worker_isosurface(path, 64, 64, 64, 0.5)
    monkeypatch.setattr(pv_iso, "ISO_MAX_VOXELS", 32 ** 3)
    sampled = pv_iso.worker_isosurface(path, 64, 64, 64, 0.5)
    assert sampled["num_polys"] < full["num_polys"]
    a, b = _points(full), _points(sampled)
    # The same sphere, r = 8 ln 2 around the centre, at either resolution.
    for pts in (a, b):
        r = np.linalg.norm(pts - 32.0, axis=1)
        assert abs(float(np.median(r)) - 8 * np.log(2)) < 1.0


def test_too_many_triangles_coarsen_the_grid_then_fail(tmp_path, monkeypatch):
    pytest.importorskip("vtk")
    path = _ball(tmp_path)
    full = pv_iso.worker_isosurface(path, 64, 64, 64, 0.5)
    monkeypatch.setattr(pv_iso, "_ISO_MIN_SAMPLES", 16)
    monkeypatch.setattr(pv_iso, "ISO_MAX_POLYS", full["num_polys"] - 1)
    assert pv_iso.worker_isosurface(path, 64, 64, 64, 0.5)["num_polys"] < full["num_polys"]
    monkeypatch.setattr(pv_iso, "ISO_MAX_POLYS", 10)
    with pytest.raises(ValueError, match="raise the threshold"):
        pv_iso.worker_isosurface(path, 64, 64, 64, 0.5)
