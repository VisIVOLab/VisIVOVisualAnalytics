"""Tests for the gravitational-basin (watershed) segmentation (F1 phase-2)."""
import numpy as np
from astropy.io import fits

from app.velocity_basins import compute_basins, velocity_basins_worker
from app.velocity_field import load_velocity_field


def _write_field(tmp_path, vx, vy, vz, name="vf.fits"):
    data = np.stack([vx, vy, vz]).astype(np.float32)  # (3, nz, ny, nx)
    path = tmp_path / name
    fits.PrimaryHDU(data).writeto(path, overwrite=True)
    return str(path)


def _single_sink(n=20):
    z, y, x = np.mgrid[0:n, 0:n, 0:n].astype(np.float64)
    c = (n - 1) / 2.0
    return (c - x), (c - y), (c - z)          # every streamline flows to the centre


def _two_sinks(n=24):
    z, y, x = np.mgrid[0:n, 0:n, 0:n].astype(np.float64)
    cy = cz = (n - 1) / 2.0
    sink_x = np.where(x < n / 2, n * 0.25, n * 0.75)
    return (sink_x - x), (cy - y), (cz - z)   # two attractors split at x = n/2


def test_single_sink_one_basin(tmp_path):
    path = _write_field(tmp_path, *_single_sink(20))
    labels, n = compute_basins(load_velocity_field(path))
    assert n == 1
    assert (labels > 0).mean() > 0.9          # almost everything reaches the sink
    assert labels.shape == (20, 20, 20)


def test_two_sinks_two_basins(tmp_path):
    path = _write_field(tmp_path, *_two_sinks(24))
    labels, n = compute_basins(load_velocity_field(path))
    assert n == 2
    # the two halves should be dominated by different basin labels
    left = labels[:, :, : 24 // 2]
    right = labels[:, :, 24 // 2:]
    left_dom = np.bincount(left[left > 0]).argmax()
    right_dom = np.bincount(right[right > 0]).argmax()
    assert left_dom != right_dom


def test_pure_rotation_mostly_unassigned(tmp_path):
    # Rotation about z is a limit cycle — streamlines never converge, so almost
    # nothing should be assigned to a basin.
    n = 20
    z, y, x = np.mgrid[0:n, 0:n, 0:n].astype(np.float64)
    c = (n - 1) / 2.0
    path = _write_field(tmp_path, -(y - c), (x - c), np.zeros_like(x))
    labels, _ = compute_basins(load_velocity_field(path))
    assert (labels > 0).mean() < 0.3


def test_outward_flow_no_false_boundary_basins(tmp_path):
    # Pure outward flow (a repeller) under +1 (attraction): every path runs to the
    # boundary and is clipped — none should be assigned (no false boundary basin).
    n = 20
    z, y, x = np.mgrid[0:n, 0:n, 0:n].astype(np.float64)
    c = (n - 1) / 2.0
    path = _write_field(tmp_path, (x - c), (y - c), (z - c))
    labels, _ = compute_basins(load_velocity_field(path), direction=1)
    assert (labels > 0).mean() < 0.3
    # ...but with direction=-1 it converges to the central repeller → one basin.
    labels_r, n_r = compute_basins(load_velocity_field(path), direction=-1)
    assert n_r >= 1 and (labels_r > 0).mean() > 0.5


def test_basins_worker_downsamples_and_serialises(tmp_path):
    path = _write_field(tmp_path, *_single_sink(40))
    p = velocity_basins_worker(path, None, basin_max_dim=16, direction=1)
    assert max(p["nx"], p["ny"], p["nz"]) <= 16
    assert p["n_basins"] >= 1
    assert len(p["raw_bytes"]) == p["nx"] * p["ny"] * p["nz"] * 4   # int32


def test_basins_router(tmp_path):
    import os
    os.environ.setdefault("VISIVO_TOKEN", "test-token-visivo-12345")
    from fastapi.testclient import TestClient
    from app.main import app
    auth = {"X-Visivo-Token": "test-token-visivo-12345"}
    client = TestClient(app, raise_server_exceptions=True)
    path = _write_field(tmp_path, *_single_sink(20))
    dsid = client.post("/v1/velocity/open", json={"path": path}, headers=auth).json()["dataset_id"]
    r = client.post("/v1/velocity/basins_bin",
                    json={"dataset_id": dsid, "basin_max_dim": 16}, headers=auth)
    assert r.status_code == 200, r.text
    assert r.headers["X-Visivo-Scalar-Type"] == "int32"
    assert int(r.headers["X-Visivo-N-Basins"]) >= 1
    w = int(r.headers["X-Visivo-Width"]); h = int(r.headers["X-Visivo-Height"])
    d = int(r.headers["X-Visivo-Depth"])
    assert len(r.content) == w * h * d * 4
