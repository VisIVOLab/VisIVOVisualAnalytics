"""Scene positions of a sky catalogue computed on the backend (issue #83) follow
the desktop's rules (Catalogue3DParser::detail::entryDistanceMpc and
vtkWindowCatalogue3D::applyFrameToEntries), so that a catalogue drawn there
shows what the desktop would."""
from __future__ import annotations

import asyncio
import json

import numpy as np
import pandas as pd
import pytest

from app.catalogue_positions import distances_mpc, planck18_comoving_mpc, sky_positions


def test_the_local_planck18_matches_astropy_to_a_permille():
    from astropy.cosmology import Planck18
    z = np.array([0.01, 0.1, 0.5, 1.0, 3.0])
    ours = planck18_comoving_mpc(z)
    ref = Planck18.comoving_distance(z).to("Mpc").value
    assert np.allclose(ours, ref, rtol=1e-3)
    assert planck18_comoving_mpc(np.array([0.0, -1.0, np.nan])).tolist() == [0.0, 0.0, 0.0]


def test_a_large_catalogue_gets_the_same_distances_from_the_grid():
    # More sources than grid nodes: interpolated, within 100 pc.
    from astropy.cosmology import WMAP9

    from app.catalogue_positions import _GRID_NODES, _planck18_trapezoid_mpc
    z = np.random.default_rng(5).uniform(0.0, 3.0, _GRID_NODES * 3)
    z[:3] = [0.0, np.nan, 3.0]
    ours = planck18_comoving_mpc(z)
    direct = np.zeros_like(z)
    direct[2:] = _planck18_trapezoid_mpc(z[2:])
    assert np.max(np.abs(ours - direct)) < 1e-4
    frame = pd.DataFrame({"Z": z})
    w = distances_mpc(frame, cosmology="WMAP9")
    assert np.max(np.abs(w[2:] - WMAP9.comoving_distance(z[2:]).to("Mpc").value)) < 1e-4
    assert w[0] == 300.0 and w[1] == 300.0
    # One far outlier does not coarsen the grid for the nearby sources.
    z[2] = 1000.0
    near = (z > 0) & (z < 0.2)
    assert np.max(np.abs(planck18_comoving_mpc(z)[near]
                         - _planck18_trapezoid_mpc(z[near]))) < 1e-3


def test_distances_follow_the_desktops_order():
    frame = pd.DataFrame({
        "z": [0.05, 0.05, np.nan, 0.0, 0.05],
        "D_L": [np.nan, 210.0, 50.0, np.nan, np.nan],
    })
    d = distances_mpc(frame)
    # A redshift only: the Planck18 comoving distance.
    assert d[0] == pytest.approx(planck18_comoving_mpc(np.array([0.05]))[0])
    # A luminosity distance and a redshift: D_L / (1 + z), before the redshift.
    assert d[1] == pytest.approx(210.0 / 1.05)
    # A distance without a redshift: as is; nothing at all: 300 Mpc.
    assert d[2] == 50.0 and d[3] == 300.0
    # Redshift space: c z / H0 for every source with a redshift.
    rs = distances_mpc(frame, distance_mode="redshift_space")
    assert rs[1] == pytest.approx(2.99792458e5 * 0.05 / 67.66)
    assert rs[2] == 50.0
    # Another cosmology overrides the distance column where a redshift exists.
    from astropy.cosmology import WMAP9
    w = distances_mpc(frame, cosmology="WMAP9")
    assert w[1] == pytest.approx(WMAP9.comoving_distance(0.05).to("Mpc").value)
    assert w[2] == 50.0


def test_positions_on_the_sky_and_in_other_frames():
    frame = pd.DataFrame({"RA": [0.0, 90.0, 266.405], "DEC": [0.0, 0.0, -28.936],
                          "DIST": [100.0, 100.0, 100.0]})
    xyz = sky_positions(frame, "RA", "DEC")
    assert np.allclose(xyz[0], [100, 0, 0]) and np.allclose(xyz[1], [0, 100, 0], atol=1e-9)
    # The Galactic centre, in the galactic frame: along +x.
    gal = sky_positions(frame, "RA", "DEC", frame_name="galactic")
    assert gal[2][0] == pytest.approx(100.0, abs=0.01)
    assert np.allclose(np.linalg.norm(gal, axis=1), 100.0)
    sg = sky_positions(frame, "RA", "DEC", frame_name="supergalactic")
    assert np.allclose(np.linalg.norm(sg, axis=1), 100.0)
    with pytest.raises(ValueError):
        sky_positions(frame, "RA", "DEC", frame_name="icrs")


def _sky_csv(tmp_path, n=2000):
    rng = np.random.default_rng(3)
    frame = pd.DataFrame({"NAME": [f"G{i}" for i in range(n)],
                          "RA": rng.uniform(150, 160, n), "DEC": rng.uniform(-5, 5, n),
                          "REDSHIFT": rng.uniform(0.01, 0.1, n),
                          "FLUX": 10 ** rng.uniform(-1, 3, n)})
    path = tmp_path / "sky.csv"
    frame.to_csv(path, index=False)
    return path, frame


def test_a_sky_catalogue_is_drawn_from_its_computed_positions(tmp_path):
    pytest.importorskip("vtk")
    from app.render_session.points import PointRenderSession
    path, frame = _sky_csv(tmp_path)
    spec = json.dumps({"paths": [str(path)], "format": "csv",
                       "coordinates": {"system": "sky", "ra": "RA", "dec": "DEC"}})
    s = PointRenderSession("c" * 32, spec)
    loop = asyncio.new_event_loop()
    run = loop.run_until_complete
    try:
        run(s.initialize())
        assert (s.view.x, s.view.y, s.view.z) == ("@x", "@y", "@z")
        assert "NAME" not in s._columns and "FLUX" in s._columns   # numeric columns
        expected = sky_positions(frame, "RA", "DEC")
        drawn = np.stack([s._columns[a] for a in ("@x", "@y", "@z")], axis=1)
        assert np.allclose(drawn, expected)
        run(s.apply_command({"type": "set_sky_positions", "frame": "galactic",
                             "distance_mode": "redshift_space"}))
        moved = np.stack([s._columns[a] for a in ("@x", "@y", "@z")], axis=1)
        assert np.allclose(moved, sky_positions(frame, "RA", "DEC", frame_name="galactic",
                                                distance_mode="redshift_space"))
        # A bad frame is refused and changes nothing.
        run(s.apply_command({"type": "set_sky_positions", "frame": "icrs"}))
        assert s.view.sky_frame == "galactic"
        run(s.apply_command({"type": "set_color", "field": "FLUX", "lut": "Viridis"}))
        assert s.levels()["data_max"] == pytest.approx(frame.FLUX.max())
    finally:
        run(s.close())
        loop.close()


def test_the_render_route_draws_a_sky_catalogue(monkeypatch):
    from fastapi import HTTPException

    from app.routers import render as render_router

    seen = {}

    class _Sess:
        session_id = "s" * 32

        class view:
            width, height = 800, 600

        async def render_frame(self):
            return b"jpeg"

        def levels(self):
            return {"threshold": None, "data_min": None, "data_max": None, "pending": False}

    class _Manager:
        async def start(self):
            pass

        async def create_session(self, **kw):
            seen.update(kw)
            return _Sess()

    coords = {"system": "sky", "ra": "RA", "dec": "DEC"}

    class _VisivoSession:
        datasets = {"cat_1": {"kind": "catalogue", "format": "csv", "path": "/d/a.csv",
                              "paths": ["/d/a.csv", "/d/b.csv"], "coordinate_fields": coords},
                    "cat_2": {"kind": "catalogue", "format": "csv", "path": "/d/g.csv",
                              "coordinate_fields": {"system": "galactic"}}}

    monkeypatch.setattr(render_router, "server_side_rendering_available", lambda: True)
    monkeypatch.setattr(render_router, "get_render_manager", lambda: _Manager())
    run = asyncio.new_event_loop().run_until_complete
    run(render_router.create_session(render_router.CreateRenderSessionRequest(dataset_id="cat_1"),
                                     token="tok", session=_VisivoSession()))
    spec = json.loads(seen["dataset_path"])
    assert spec == {"paths": ["/d/a.csv", "/d/b.csv"], "format": "csv", "coordinates": coords}
    with pytest.raises(HTTPException) as err:
        run(render_router.create_session(
            render_router.CreateRenderSessionRequest(dataset_id="cat_2"),
            token="tok", session=_VisivoSession()))
    assert err.value.status_code == 422


def test_of_two_columns_with_one_name_the_last_counts_as_on_the_desktop():
    # "z" and "Z" normalise alike: the desktop's QMap keeps the last.
    frame = pd.DataFrame([[0.1, 0.5]], columns=["z", "Z"])
    assert distances_mpc(frame)[0] == pytest.approx(planck18_comoving_mpc(np.array([0.5]))[0])


def test_a_source_without_a_position_is_not_drawn(tmp_path):
    pytest.importorskip("vtk")
    from app.render_session.points import PointRenderSession
    path = tmp_path / "holes.csv"
    pd.DataFrame({"RA": [10.0, np.nan, 30.0], "DEC": [0.0, 5.0, -5.0],
                  "Z": [0.01, 0.02, 0.03]}).to_csv(path, index=False)
    spec = json.dumps({"paths": [str(path)], "format": "csv",
                       "coordinates": {"system": "sky", "ra": "RA", "dec": "DEC"}})
    s = PointRenderSession("h" * 32, spec)
    loop = asyncio.new_event_loop()
    try:
        loop.run_until_complete(s.initialize())
        assert s._polydata.GetNumberOfPoints() == 2 and list(s._rows) == [0, 2]
    finally:
        loop.run_until_complete(s.close())
        loop.close()
