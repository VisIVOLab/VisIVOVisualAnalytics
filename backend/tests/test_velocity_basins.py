"""Tests for the gravitational-basin (watershed) segmentation (F1 phase-2)."""
import numpy as np
import pytest
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


def _field_from(vx, vy, vz, spacing=(1.0, 1.0, 1.0)):
    from app.velocity_field import VelocityField
    return VelocityField(np.asarray(vx, np.float32), np.asarray(vy, np.float32),
                         np.asarray(vz, np.float32), spacing, (0.0, 0.0, 0.0),
                         "km/s", "test", False)


def _radial(n=17):
    z, y, x = np.mgrid[0:n, 0:n, 0:n].astype(np.float64)
    c = (n - 1) / 2.0
    return (c - x), (c - y), (c - z)


def test_slow_flow_is_not_mistaken_for_convergence():
    """Basins depend on the flow DIRECTION only: the same field x 0.001 (or x 1000)
    gives the same segmentation. A speed-proportional step used to make the slow
    copy 'stagnate' everywhere: 8 basins over 64 of 4913 voxels instead of 1/4913."""
    ref, n_ref = compute_basins(_field_from(*_radial(17)))
    assert n_ref == 1 and int((ref > 0).sum()) == 17 ** 3
    for scale in (1e-3, 1e3):
        lab, n = compute_basins(_field_from(*(c * scale for c in _radial(17))))
        assert n == 1, scale
        np.testing.assert_array_equal(lab, ref)


def test_two_attractors_split_at_any_speed_scale():
    n = 24
    ref, n_ref = compute_basins(_field_from(*_two_sinks(n)))
    assert n_ref == 2
    for scale in (1e-3, 1e3):
        lab, nb = compute_basins(_field_from(*(c * scale for c in _two_sinks(n))))
        assert nb == 2, scale
        np.testing.assert_array_equal(lab, ref)
    # The split is at x = n/2: each half drains to its own attractor.
    left, right = ref[:, :, : n // 2], ref[:, :, n // 2:]
    assert np.unique(left[left > 0]).size == 1
    assert np.unique(right[right > 0]).size == 1
    assert np.unique(left[left > 0])[0] != np.unique(right[right > 0])[0]


def test_speed_varying_along_the_path_does_not_split_a_basin():
    """One attractor whose speed drops by 1e4 in the outer shell: still one basin
    covering the grid (speed is irrelevant to where a streamline goes)."""
    n = 17
    vx, vy, vz = _radial(n)
    z, y, x = np.mgrid[0:n, 0:n, 0:n].astype(np.float64)
    r = np.sqrt((x - 8) ** 2 + (y - 8) ** 2 + (z - 8) ** 2)
    g = np.where(r > 4.0, 1e-4, 1.0)
    lab, nb = compute_basins(_field_from(vx * g, vy * g, vz * g))
    assert nb == 1
    assert int((lab > 0).sum()) == n ** 3


def _centred(n=25):
    z, y, x = np.mgrid[0:n, 0:n, 0:n].astype(np.float64)
    c = (n - 1) / 2.0
    return x - c, y - c, z - c


def test_nan_hole_does_not_make_a_false_basin():
    """Paths that run into a NaN region are dead, not 'stuck in one cell': a pile
    of them on the hole's rim used to become a second, false basin."""
    x, y, z = _centred(25)
    vx, vy, vz = -x, -y, -z
    for a in (vx, vy, vz):
        a[10:15, 2:9, 9:16] = np.nan
    lab, nb = compute_basins(_field_from(vx, vy, vz))
    assert nb == 1
    assert not lab[10:15, 2:9, 9:16].any()           # inside the hole: unassigned
    assert int((lab > 0).sum()) > 12000               # the rest drains to the sink


def test_sink_next_to_a_small_nan_patch_keeps_its_basin():
    """A 1x3x3 NaN patch 1-2 cells from a sink. order=1 interpolation used to turn
    every sample touching a NaN corner into NaN, so the probes died and the sink
    was rejected (0 basins). With NaN-aware sampling and dead probes neutral,
    the sink stands and most of the grid drains to it."""
    n = 33
    x, y, z = _centred(n)
    c = 16
    for d, min_frac in ((1, 0.6), (2, 0.85)):
        vx, vy, vz = -x, -y, -z
        for a in (vx, vy, vz):
            a[c - 1:c + 2, c - 1:c + 2, c + d] = np.nan
        lab, nb = compute_basins(_field_from(vx, vy, vz))
        assert nb == 1, d
        assert (lab > 0).mean() > min_frac, d


def test_saddle_with_blank_voxels_on_its_unstable_axis_is_not_a_basin():
    """Double well on 33³ with 1x3x3 NaN patches 2 cells either side of the
    saddle along x: both unstable probes run outward and die on the blanks.
    Dying farther out than they started is an escape, so the 1089-voxel
    separatrix does not come back as a third basin."""
    n = 33
    x, y, z = _centred(n)
    c, a = 16, 8.0
    vx, vy, vz = -(x * (x * x - a * a)) / (a * a), -y, -z
    for arr in (vx, vy, vz):
        for d in (-2, 2):
            arr[c - 1:c + 2, c - 1:c + 2, c + d] = np.nan
    lab, nb = compute_basins(_field_from(vx, vy, vz))
    assert nb == 2
    assert not lab[:, :, c].any()


def test_sink_three_cells_from_a_nan_patch_keeps_its_basin():
    n = 33
    x, y, z = _centred(n)
    c = 16
    vx, vy, vz = -x, -y, -z
    for arr in (vx, vy, vz):
        arr[c - 1:c + 2, c - 1:c + 2, c + 3] = np.nan
    lab, nb = compute_basins(_field_from(vx, vy, vz))
    assert nb == 1 and (lab > 0).mean() > 0.9


def test_two_well_separatrix_is_not_a_basin():
    """v = -grad phi of a double well: the plane x = 0 between the wells is the
    stable manifold of a saddle. Its voxels stay unassigned (label 0) instead of
    forming a third basin; each well keeps its half."""
    x, y, z = _centred(25)
    a = 6.0
    lab, nb = compute_basins(_field_from(-(x * (x * x - a * a)) / (a * a), -y, -z))
    assert nb == 2
    assert not lab[:, :, 12].any()                    # the separatrix plane x = 0
    left, right = lab[:, :, :12], lab[:, :, 13:]
    assert np.unique(left).tolist() != np.unique(right).tolist()
    assert (left > 0).all() and (right > 0).all()


def test_pure_saddle_has_no_basin():
    x, y, z = _centred(25)
    for weak in (1.0, 1.0 / 50.0):                    # strong and weak unstable axis
        _, nb = compute_basins(_field_from(weak * x, -y, -z))
        assert nb == 0, weak


def test_anisotropic_sinks_and_filament_are_one_basin():
    x, y, z = _centred(25)
    for vx, vy, vz in ((-x, -y / 10, -z), (-x, -y / 50, -z), (-x / 20, -y, -z),
                       (np.zeros_like(x), -y, -z)):    # the last: a line attractor
        lab, nb = compute_basins(_field_from(vx, vy, vz))
        assert nb == 1
        assert (lab > 0).mean() > 0.99


def test_basins_worker_downsamples_and_serialises(tmp_path):
    path = _write_field(tmp_path, *_single_sink(40))
    p = velocity_basins_worker(path, None, basin_max_dim=16, direction=1)
    assert max(p["nx"], p["ny"], p["nz"]) <= 16
    assert p["n_basins"] >= 1
    assert len(p["raw_bytes"]) == p["nx"] * p["ny"] * p["nz"] * 4   # int32


def _tilted_double_well(nz=12, ny=8, nx=40):
    """Double well along x with z coupled to x: the separatrix between the wells
    tilts with the ratio of the x and z cell sizes, so on this non-cubic grid the
    labels depend on whether a box (anisotropic cells box/(n_i-1)) is given."""
    z, y, x = np.mgrid[0:nz, 0:ny, 0:nx].astype(np.float64)
    X, Y, Z = x - (nx - 1) / 2, y - (ny - 1) / 2, z - (nz - 1) / 2
    a = 10.0
    return (-(X * (X * X - a * a)) / (a * a) + 1.5 * Z), -Y, (-Z + 0.3 * X)


def test_basins_router_box_mpc_on_a_non_cubic_grid(tmp_path):
    import os
    os.environ.setdefault("VISIVO_TOKEN", "test-token-visivo-12345")
    from fastapi.testclient import TestClient
    from app.main import app
    auth = {"X-Visivo-Token": "test-token-visivo-12345"}
    client = TestClient(app, raise_server_exceptions=True)
    path = _write_field(tmp_path, *_tilted_double_well())
    dsid = client.post("/v1/velocity/open", json={"path": path}, headers=auth).json()["dataset_id"]

    def basins(**extra):
        r = client.post("/v1/velocity/basins_bin",
                        json={"dataset_id": dsid, **extra}, headers=auth)
        assert r.status_code == 200, r.text
        lab = np.frombuffer(r.content, dtype="<i4")
        sp = [float(s) for s in r.headers["X-Visivo-Spacing"].split(",")]
        org = [float(o) for o in r.headers["X-Visivo-Origin"].split(",")]
        return lab, int(r.headers["X-Visivo-N-Basins"]), sp, org

    unit, n_unit, sp_u, org_u = basins()
    assert n_unit == 2
    assert sp_u == [1.0, 1.0, 1.0]                     # opened without a box
    assert org_u == pytest.approx([-19.5, -3.5, -5.5])

    box, n_box, sp_b, org_b = basins(box_mpc=100.0)
    assert n_box == 2
    # The headers follow the box used: box/(n-1) per axis, first sample at -box/2.
    assert sp_b == pytest.approx([100.0 / 39, 100.0 / 7, 100.0 / 11])
    assert org_b == pytest.approx([-50.0, -50.0, -50.0])
    # Anisotropic cells tilt the separatrix: some voxels change basin.
    assert int((unit != box).sum()) > 100
    # Only the cell-size RATIOS matter: any other box gives the same labels.
    box4, _, _, _ = basins(box_mpc=400.0)
    np.testing.assert_array_equal(box, box4)

    # box_mpc = 0 asks for unit cells explicitly: same as a box-less open.
    zero, n_zero, sp_0, org_0 = basins(box_mpc=0.0)
    np.testing.assert_array_equal(zero, unit)
    assert sp_0 == [1.0, 1.0, 1.0] and org_0 == pytest.approx(org_u)

    for bad in (-5.0, -1e-9):
        r = client.post("/v1/velocity/basins_bin",
                        json={"dataset_id": dsid, "box_mpc": bad}, headers=auth)
        assert r.status_code == 422, bad


def test_density_router_box_mpc(tmp_path):
    """∇·v depends on the per-axis cell sizes like the basins: the request's box
    sets the spacing/origin headers and the values; 0 means unit cells; omitted
    keeps the dataset's box; negative is 422."""
    import os
    os.environ.setdefault("VISIVO_TOKEN", "test-token-visivo-12345")
    from fastapi.testclient import TestClient
    from app.main import app
    auth = {"X-Visivo-Token": "test-token-visivo-12345"}
    client = TestClient(app, raise_server_exceptions=True)
    nz, ny, nx = 12, 8, 40
    z, y, x = np.mgrid[0:nz, 0:ny, 0:nx].astype(np.float64)
    # v = (x, y, z) in index units: ∇·v = 3 with unit cells,
    # 1/sx + 1/sy + 1/sz with a box.
    path = _write_field(tmp_path, x, y, z)
    dsid = client.post("/v1/velocity/open", json={"path": path, "box_mpc": 50.0},
                       headers=auth).json()["dataset_id"]

    def density(**extra):
        r = client.post("/v1/velocity/density_bin",
                        json={"dataset_id": dsid, **extra}, headers=auth)
        assert r.status_code == 200, r.text
        div = np.frombuffer(r.content, dtype="<f4")
        sp = [float(s) for s in r.headers["X-Visivo-Spacing"].split(",")]
        org = [float(o) for o in r.headers["X-Visivo-Origin"].split(",")]
        return div, sp, org

    def expected(box):
        return 1 / (box / (nx - 1)) + 1 / (box / (ny - 1)) + 1 / (box / (nz - 1))

    d_open, sp_open, org_open = density()                 # the dataset's own box
    assert sp_open == pytest.approx([50 / 39, 50 / 7, 50 / 11])
    assert org_open == pytest.approx([-25.0, -25.0, -25.0])
    assert np.allclose(d_open, expected(50.0), rtol=1e-5)

    d_box, sp_box, org_box = density(box_mpc=100.0)       # the viewer's box
    assert sp_box == pytest.approx([100 / 39, 100 / 7, 100 / 11])
    assert org_box == pytest.approx([-50.0, -50.0, -50.0])
    assert np.allclose(d_box, expected(100.0), rtol=1e-5)

    d_unit, sp_unit, org_unit = density(box_mpc=0.0)      # unit cells explicitly
    assert sp_unit == [1.0, 1.0, 1.0]
    assert org_unit == pytest.approx([-19.5, -3.5, -5.5])
    assert np.allclose(d_unit, 3.0, rtol=1e-5)

    r = client.post("/v1/velocity/density_bin",
                    json={"dataset_id": dsid, "box_mpc": -1.0}, headers=auth)
    assert r.status_code == 422


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
    # 20³ → basin_max_dim 16: factor 2 per axis; unit spacing, centred origin
    # -9.5, so the first block centre is at -9.0.
    assert r.headers["X-Visivo-Downsample-Factors"] == "2,2,2"
    assert [float(s) for s in r.headers["X-Visivo-Spacing"].split(",")] == [2.0, 2.0, 2.0]
    assert [float(o) for o in r.headers["X-Visivo-Origin"].split(",")] == [-9.0, -9.0, -9.0]
