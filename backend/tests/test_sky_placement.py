"""Where a sky catalogue's sources go (issues #265, #260): a source with no
distance is not placed and is counted, a measured distance wins under every
cosmology, redshift space takes the model's H0, and the VR routes place a
catalogue with the desktop window's frame, distance mode and cosmology."""
from __future__ import annotations

import numpy as np
import pandas as pd
import pytest

from app import catalogue_positions as cp

_C = 2.99792458e5


def _frame():
    # 2MRS-like: M31 and M81 have z < 0, one row has z = 0, one no redshift,
    # one is closer than cz = 1000 km/s, one has a measured distance.
    return pd.DataFrame({
        "NAME": ["M31", "M81", "zero", "blank", "near", "far", "measured"],
        "RA": [10.68, 148.89, 30.0, 40.0, 50.0, 60.0, 70.0],
        "DEC": [41.27, 69.07, 0.0, 5.0, -5.0, 10.0, -10.0],
        "Z": [-0.001001, -0.000113, 0.0, np.nan, 500.0 / _C, 0.03, 0.02],
        "DIST": [np.nan, np.nan, np.nan, np.nan, np.nan, np.nan, 85.0],
    })


def test_a_source_with_no_distance_is_not_placed_and_is_counted():
    f = _frame()
    for mode in cp.DISTANCE_MODES:
        p = cp.placement(f, distance_mode=mode)
        assert np.isnan(p.distances[:4]).all()           # no 300 Mpc shell
        assert np.isfinite(p.distances[4:]).all()
        assert [cp.REASONS.get(int(r)) for r in p.reason[:4]] == [
            "z ≤ 0", "z ≤ 0", "z = 0 (not measured)", "no redshift"]
        assert p.counts()["placed"] == 3 and p.counts()["unplaced"] == 4
    # 0 < cz < 1000 km/s from the redshift: placed, and flagged.
    p = cp.placement(f)
    assert p.uncertain.tolist() == [False] * 4 + [True, False, False]
    assert p.counts()["uncertain"] == 1


@pytest.mark.parametrize("mode", cp.DISTANCE_MODES)
def test_a_catalogue_of_positions_only_is_on_the_celestial_sphere(mode):
    # No redshift column and no distance column at all: every row on the
    # sphere (not a distance), none unplaced; a z column changes that.
    f = pd.DataFrame({"RA": [10.0, 200.0, 300.0], "DEC": [5.0, -30.0, 60.0], "MAG": [9, 10, 11]})
    p = cp.placement(f, distance_mode=mode, cosmology="WMAP9")
    assert p.sky_sphere and (p.distances == cp.SKY_SPHERE_RADIUS_MPC).all()
    assert p.counts() == {"placed": 3, "unplaced": 0, "uncertain": 0, "sky_sphere": True}
    xyz = cp.sky_positions(f, "RA", "DEC", frame_name="galactic", distance_mode=mode)
    assert np.allclose(np.linalg.norm(xyz, axis=1), cp.SKY_SPHERE_RADIUS_MPC)
    with_z = f.assign(Z=[0.01, -0.001, np.nan])
    q = cp.placement(with_z, distance_mode=mode)
    assert not q.sky_sphere and q.counts()["unplaced"] == 2


def test_a_negative_redshift_with_a_distance_column_is_placed():
    f = pd.DataFrame({"Z": [-0.001], "DIST": [0.78]})
    assert cp.distances_mpc(f, cosmology="WMAP9")[0] == pytest.approx(0.78)


@pytest.mark.parametrize("model", ["Planck18", "Planck15", "WMAP9"])
def test_a_measured_distance_wins_under_every_cosmology(model):
    f = _frame()
    d = cp.distances_mpc(f, cosmology=model)
    assert d[6] == pytest.approx(85.0)
    assert cp.placement(f, cosmology=model).origin[6] == cp.FROM_COLUMN


def test_redshift_space_takes_the_models_h0():
    from astropy.cosmology import WMAP9
    f = pd.DataFrame({"Z": [0.02]})
    assert cp.hubble_constant("WMAP9") == pytest.approx(WMAP9.H0.value)
    d = cp.distances_mpc(f, distance_mode="redshift_space", cosmology="WMAP9")
    assert d[0] == pytest.approx(_C * 0.02 / 69.32, rel=1e-6)
    d18 = cp.distances_mpc(f, distance_mode="redshift_space")
    assert d18[0] == pytest.approx(_C * 0.02 / 67.66)


def test_a_custom_cosmology_is_refused():
    with pytest.raises(ValueError, match="custom cosmology"):
        cp.placement(_frame(), cosmology="Custom")


def test_unplaced_rows_never_reach_a_sample():
    from app.point_sample import sample_points
    from app.point_table import table_from_frame
    t = table_from_frame(_frame(), {"system": "sky", "ra": "RA", "dec": "DEC"},
                         {"frame": "galactic"})
    assert t.sky_settings["unplaced"] == 4 and t.sky_settings["frame"] == "galactic"
    s = sample_points(t, 100)
    assert len(s.positions) == 3
    assert all(np.isfinite(s.bounds))


def test_the_sample_budget_and_step_count_placed_rows_only():
    # Half the rows have no distance: a budget of 10 gives 10 placed rows,
    # the step counts the placed rows, and the total says how many there are.
    from app.point_sample import headers, sample_points
    from app.point_table import table_from_frame
    n = 40
    f = pd.DataFrame({"RA": np.linspace(0, 350, n), "DEC": np.zeros(n),
                      "Z": np.where(np.arange(n) % 2 == 0, 0.02, -0.001),
                      "M": np.arange(n, dtype=float)})
    t = table_from_frame(f, {"system": "sky", "ra": "RA", "dec": "DEC"})
    assert t.drawable_count(t.axes) == 20 and t.num_rows == 40
    s = sample_points(t, 10)
    assert len(s.positions) == 10
    assert s.total_rows == 40 and s.placed_rows == 20 and s.step == pytest.approx(2.0)
    h = headers(s)
    assert h["X-Visivo-Total-Rows"] == "40" and h["X-Visivo-Placed-Rows"] == "20"
    # With a filter too, only placed rows are candidates.
    s = sample_points(t, 100, filters=[{"field": "M", "op": ">=", "value": 10}])
    assert len(s.positions) == 15
    # Other axes than the computed positions: every row is drawable.
    assert t.drawable(("M", "M", "M")) is None
    assert len(sample_points(t, 100, axes=("M", "M", "M")).positions) == 40


def test_the_point_session_samples_placed_rows_and_sizes_splats_by_them(tmp_path, monkeypatch):
    pytest.importorskip("vtk")
    import asyncio
    import json

    from app.render_session import points as rp
    path = tmp_path / "half.csv"
    n = 40
    pd.DataFrame({"RA": np.linspace(0, 350, n), "DEC": np.zeros(n),
                  "Z": np.where(np.arange(n) % 2 == 0, 0.02, -0.001)}).to_csv(path, index=False)
    spec = json.dumps({"paths": [str(path)], "format": "csv",
                       "coordinates": {"system": "sky", "ra": "RA", "dec": "DEC"}})
    monkeypatch.setattr(rp, "MAX_POINTS", 8)
    s = rp.PointRenderSession("p" * 32, spec)
    loop = asyncio.new_event_loop()
    try:
        loop.run_until_complete(s.initialize())
        assert s._placed_rows == 20
        assert len(s._sample) == 8 and len(s._rows) == 8     # all eight placed
        assert (s._sample % 2 == 0).all()
    finally:
        loop.run_until_complete(s.close())
        loop.close()


def _sky_csv(tmp_path):
    rng = np.random.default_rng(11)
    n = 400
    z = rng.uniform(0.005, 0.08, n)
    z[:5] = [-0.001, 0.0, np.nan, -0.0002, 0.0]          # five unplaced
    f = pd.DataFrame({"NAME": [f"G{i}" for i in range(n)], "RA": rng.uniform(0, 360, n),
                      "DEC": rng.uniform(-80, 80, n), "Z": z, "MAG": rng.uniform(8, 12, n)})
    path = tmp_path / "sky.csv"
    f.to_csv(path, index=False)
    return path, f


@pytest.mark.asyncio
async def test_the_vr_sample_gives_the_desktops_positions_with_its_settings(
        client, auth_headers, tmp_path):
    from app import point_sample as ps
    path, f = _sky_csv(tmp_path)
    cat = (await client.post("/v1/catalogue/open", json={"path": str(path), "format": "csv"},
                             headers=auth_headers)).json()
    did = cat["dataset_id"]
    sky = {"frame": "supergalactic", "distance_mode": "redshift_space", "cosmology": "WMAP9"}
    info = (await client.post("/v1/points/info", json={"dataset_id": did, "sky": sky},
                              headers=auth_headers)).json()
    assert info["sky"] is True
    st = info["sky_settings"]
    assert (st["frame"], st["distance_mode"], st["cosmology"]) == (
        "supergalactic", "redshift_space", "WMAP9")
    assert st["unplaced"] == 5 and st["placed"] == len(f) - 5
    r = await client.post("/v1/points/sample_bin",
                          json={"dataset_id": did, "max_points": 10_000, "sky": sky},
                          headers=auth_headers)
    assert r.status_code == 200, r.text
    assert r.headers["x-visivo-sky"] == "supergalactic,redshift_space,WMAP9"
    assert r.headers["x-visivo-unplaced"] == "5"
    n = int(r.headers["x-visivo-count"])
    assert n == len(f) - 5                                 # the unplaced left out
    codes = np.frombuffer(r.content[: n * 6], dtype="<u2").reshape(n, 3)
    bounds = np.array([float(v) for v in r.headers["x-visivo-bounds"].split(",")])
    lo, hi = bounds[0::2], bounds[1::2]
    got = lo + codes / 65535.0 * (hi - lo)
    # What the desktop computes for the same settings, in the sample's order.
    want = cp.sky_positions(f, "RA", "DEC", frame_name="supergalactic",
                            distance_mode="redshift_space", cosmology="WMAP9")
    rows = np.arange(len(f))[np.isfinite(want).all(axis=1)]
    want = want[rows][ps.prefix_order(len(rows))]
    assert np.allclose(got, want, atol=(hi - lo).max() / 65535 * 0.51)
    # The defaults differ from it: the settings were applied, not ignored.
    default = cp.sky_positions(f, "RA", "DEC")[rows][ps.prefix_order(len(rows))]
    assert not np.allclose(got, default, atol=1.0)
    # A custom cosmology is refused, as the ray-traced view refuses it.
    r = await client.post("/v1/points/info",
                          json={"dataset_id": did, "sky": {**sky, "cosmology": "Custom"}},
                          headers=auth_headers)
    assert r.status_code == 422 and "custom cosmology" in r.json()["detail"]


@pytest.mark.asyncio
async def test_the_vr_routes_put_a_catalogue_of_positions_on_the_sphere(client, auth_headers, tmp_path):
    path = tmp_path / "positions.csv"
    pd.DataFrame({"RA": [10.0, 200.0, 300.0], "DEC": [5.0, -30.0, 60.0],
                  "MAG": [9.0, 10.0, 11.0]}).to_csv(path, index=False)
    did = (await client.post("/v1/catalogue/open", json={"path": str(path), "format": "csv"},
                             headers=auth_headers)).json()["dataset_id"]
    sky = {"frame": "supergalactic"}
    info = (await client.post("/v1/points/info", json={"dataset_id": did, "sky": sky},
                              headers=auth_headers)).json()
    st = info["sky_settings"]
    assert st["sky_sphere"] is True and st["placed"] == 3 and st["unplaced"] == 0
    r = await client.post("/v1/points/sample_bin", json={"dataset_id": did, "sky": sky},
                          headers=auth_headers)
    assert r.status_code == 200 and r.headers["x-visivo-count"] == "3"


@pytest.mark.asyncio
async def test_the_hub_show_and_the_launch_carry_the_sky(client, auth_headers):
    r = await client.post("/v1/sessions/new", json={}, headers=auth_headers)
    h = {**auth_headers, "X-Visivo-Session": r.json()["session_id"]}
    r = await client.post("/v1/vr/headset/hello",
                          json={"client": "page-1", "transport": "pair", "label": "Quest"}, headers=h)
    assert r.status_code == 200, r.text
    sky = {"frame": "galactic", "distance_mode": "comoving", "cosmology": "WMAP9"}
    r = await client.post("/v1/vr/headset/command", json={
        "type": "show", "dataset_id": "cat_1", "path": "/d/2mrs.csv", "kind": "points", "sky": sky},
        headers=h)
    assert r.status_code == 200, r.text
    cmds = (await client.get("/v1/vr/headset/commands", params={"client": "page-1"},
                             headers=h)).json()["commands"]
    assert cmds[-1]["sky"] == sky
    bad = await client.post("/v1/vr/headset/command", json={
        "type": "show", "dataset_id": "cat_1", "path": "/d/2mrs.csv", "sky": {"frame": "icrs"}},
        headers=h)
    assert bad.status_code == 422
    cube = {"dataset_id": "cat_1", "path": "/d/2mrs.csv", "kind": "points", "box": None, "sky": sky}
    r = await client.post("/v1/vr/launch", json={"cube": cube}, headers=h)
    assert r.status_code == 200, r.text
    out = (await client.post("/v1/vr/launch/exchange", json={"ticket": r.json()["ticket"]})).json()
    assert out["cube"]["sky"] == sky


def test_the_cz_flag_compares_the_cz_the_file_shows():
    # Issue #298: a file's cz = 1000.0 becomes z = cz / c, and c z comes back
    # a hair under 1000. With a cz column its value is compared; without one
    # c z rounded to CZ_PRECISION_KMS, so 1000.0 is not below 1000.
    z_at_1000 = 1000.0 / _C
    while _C * z_at_1000 >= cp.UNCERTAIN_CZ_KMS:
        z_at_1000 = np.nextafter(z_at_1000, 0.0)
    assert _C * z_at_1000 < cp.UNCERTAIN_CZ_KMS        # the raw product would flag it
    assert cp.recession_velocity_kms(z_at_1000) == 1000.0
    z_only = pd.DataFrame({"Z": [z_at_1000, 999.99 / _C, 0.03]})
    for mode in cp.DISTANCE_MODES:
        assert cp.placement(z_only, distance_mode=mode).uncertain.tolist() == [False, True, False]
    # The cz column wins where a row has one; a blank falls back to c z.
    with_cz = pd.DataFrame({"Z": [990.0 / _C, 1001.0 / _C, z_at_1000],
                            "cz": [1000.0, 999.0, np.nan]})
    p = cp.placement(with_cz)
    assert p.uncertain.tolist() == [False, True, False]
    assert p.counts()["uncertain"] == 1
    assert cp.cz_kms(with_cz).tolist() == [1000.0, 999.0, 1000.0]
    # A bare VELOCITY or V is not a cz column.
    assert cp._first(pd.DataFrame({"Z": [0.1], "VELOCITY": [1.0], "V": [12.0]}),
                     cp._VELOCITY_ALIASES) == ""
