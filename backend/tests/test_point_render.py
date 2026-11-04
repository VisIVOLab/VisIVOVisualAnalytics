"""Server-side rendering of a VBT point table (issue #83): the point session
draws the table where it lies, as the desktop's point viewer would, and the
render route starts one for a point table."""
from __future__ import annotations

import asyncio
import io

import numpy as np
import pytest

pytest.importorskip("vtk")

from PIL import Image   # noqa: E402

from app.render_session import points   # noqa: E402
from app.render_session.points import PointRenderSession, gaussian_splat   # noqa: E402


def _table(tmp_path, n=4000):
    """Two clusters, at x = -10 and x = +10, with a 'mass' that tells them
    apart (1 and 100)."""
    rng = np.random.default_rng(1)
    half = n // 2
    x = np.concatenate([rng.normal(-10, 1, half), rng.normal(10, 1, half)])
    y, z = rng.normal(0, 1, n), rng.normal(0, 1, n)
    mass = np.concatenate([np.full(half, 1.0), np.full(half, 100.0)])
    data = np.stack([x, y, z, mass]).astype("<f4")
    binary = tmp_path / "snap.bin"
    data.tofile(binary)
    (tmp_path / "snap.bin.head").write_text(f"float\n4\n{n}\nlittle\nX\nY\nZ\nMass\n")
    return str(tmp_path / "snap.bin.head")


_RENDER_PROBE = """
import vtk
w = vtk.vtkRenderWindow()
w.SetOffScreenRendering(1)
w.SetSize(8, 8)
w.AddRenderer(vtk.vtkRenderer())
w.Render()
"""


@pytest.fixture(scope="module")
def can_render():
    """Drawing needs a GL context: a CI runner without a GPU or a display
    crashes the process creating one, so ask a child process first."""
    import subprocess
    import sys
    probe = subprocess.run([sys.executable, "-c", _RENDER_PROBE], capture_output=True,
                           timeout=60)
    if probe.returncode != 0:
        pytest.skip("no offscreen OpenGL here (no GPU or display)")


def _run(coro):
    return asyncio.get_event_loop_policy().new_event_loop().run_until_complete(coro)


def _session(path):
    s = PointRenderSession("t" * 32, path)
    loop = asyncio.new_event_loop()

    def run(coro):
        return loop.run_until_complete(coro)
    run(s.initialize())
    return s, run, loop


def _image(jpeg: bytes) -> np.ndarray:
    """The pixels lit by a point (the background is a dark grey)."""
    return np.asarray(Image.open(io.BytesIO(jpeg)).convert("L"), dtype=float) > 80


def test_the_table_is_drawn_and_filters_cut_it(tmp_path, can_render):
    s, run, loop = _session(_table(tmp_path))
    try:
        assert len(s._rows) == 4000
        assert (s.view.x, s.view.y, s.view.z) == ("X", "Y", "Z")
        run(s.apply_command({"type": "set_show_outline", "visible": False}))
        run(s.apply_command({"type": "set_show_axes", "visible": False}))
        both = _image(run(s.render_frame()))
        h, w = both.shape
        left, right = both[:, : w // 2].sum(), both[:, w // 2:].sum()
        assert left > 0 and right > 0
        # Only the cluster at +10 passes; the camera stays, so it is on the right.
        run(s.apply_command({"type": "set_filters",
                             "filters": [{"field": "X", "op": ">", "value": "0"}]}))
        assert len(s._rows) == 2000
        one = _image(run(s.render_frame()))
        assert one[:, : w // 2].sum() < 0.05 * left
        assert one[:, w // 2:].sum() > 0.5 * right
    finally:
        run(s.close())
        loop.close()


def test_colour_by_a_column_reports_its_range(tmp_path):
    s, run, loop = _session(_table(tmp_path))
    try:
        assert s.levels()["data_min"] is None     # solid colour
        run(s.apply_command({"type": "set_color", "field": "Mass", "lut": "Viridis"}))
        lv = s.levels()
        assert (lv["data_min"], lv["data_max"]) == (1.0, 100.0)
        assert s._mappers[("points", False)].GetScalarVisibility() == 1
        assert s._mappers[("points", False)].GetScalarRange() == (1.0, 100.0)
        run(s.apply_command({"type": "set_color", "field": "Mass", "lut": "Viridis",
                             "min": 10.0, "max": 50.0}))
        assert s._mappers[("points", False)].GetScalarRange() == (10.0, 50.0)
        with pytest.raises(ValueError):
            s._apply_command_on_thread({"type": "set_color", "field": "Nope"})
        assert s.view.color_field == "Mass"
    finally:
        run(s.close())
        loop.close()


def test_drag_draws_a_sample_and_release_the_whole(tmp_path, monkeypatch):
    monkeypatch.setattr(points, "LOD_POINTS", 500)
    s, run, loop = _session(_table(tmp_path))
    try:
        assert s._lod_polydata.GetNumberOfPoints() == 500
        run(s.apply_command({"type": "set_interactive", "value": True}))
        assert s._actor.GetMapper().GetInput() is s._lod_polydata
        run(s.apply_command({"type": "set_interactive", "value": False}))
        assert s._actor.GetMapper().GetInput() is s._polydata
        run(s.apply_command({"type": "set_point_style", "mode": "gaussian", "size": 3,
                             "intensity": 35}))
        assert s._actor.GetMapper() is s._mappers[("gaussian", False)]
    finally:
        run(s.close())
        loop.close()


def test_splats_after_points_cover_the_cloud(tmp_path, can_render):
    """Drawn as points, then as splats: the splats cover the clusters. With
    the points mapper's vertex buffer shifted and scaled (a million points or
    more), the splats were drawn as a single dot."""
    s, run, loop = _session(_table(tmp_path, n=1_000_000))
    try:
        points_px = _image(run(s.render_frame())).sum()
        run(s.apply_command({"type": "set_point_style", "mode": "gaussian", "size": 3,
                             "intensity": 35}))
        assert _image(run(s.render_frame())).sum() > points_px
    finally:
        run(s.close())
        loop.close()


def test_a_large_table_is_drawn_from_an_even_sample(tmp_path, monkeypatch):
    monkeypatch.setattr(points, "MAX_POINTS", 1000)
    s, run, loop = _session(_table(tmp_path))
    try:
        assert len(s._rows) == 1000
        assert s._rows[0] == 0 and s._rows[-1] == 3999
        # Both clusters are in the sample.
        assert (s._columns["X"][s._rows] < 0).sum() == pytest.approx(500, abs=2)
    finally:
        run(s.close())
        loop.close()


def test_gaussian_splats_follow_the_desktop_rules():
    radius, opacity = gaussian_splat((0, 100, 0, 50, 0, 10), 1000, 3, 35)
    assert radius == pytest.approx((100 * 50 * 10 / 1000) ** (1 / 3) * 3 / 8)
    # A cube: the former VBT viewer's longest side / ∛N.
    assert gaussian_splat((0, 10, 0, 10, 0, 10), 1000, 3, 35)[0] == pytest.approx(10 / 10 * 3 / 8)
    # Half the points in half the box (a filter's slab): the same splats.
    assert gaussian_splat((0, 5, 0, 10, 0, 10), 500, 3, 35)[0] == pytest.approx(3 / 8)
    depth = 100 / (100 * 50 * 10 / 1000) ** (1 / 3)
    assert opacity == pytest.approx(min(0.9, 20 / depth) * 0.35)
    # A cube: 20 / ∛N, and a filter's slab of it keeps the opacity.
    assert gaussian_splat((0, 64, 0, 64, 0, 64), 64 ** 3, 3, 35)[1] == pytest.approx(20 / 64 * 0.35)
    assert gaussian_splat((0, 32, 0, 64, 0, 64), 64 ** 3 // 2, 3, 35)[1] == pytest.approx(20 / 64 * 0.35)
    assert gaussian_splat((0, 1, 0, 1, 0, 1), 10 ** 9, 1, 1)[1] == 0.05


def test_the_render_route_starts_a_point_session(tmp_path, monkeypatch):
    """create_session hands a point table to the point session's factory."""
    from app.routers import render as render_router
    CreateRenderSessionRequest = render_router.CreateRenderSessionRequest

    seen = {}

    class _Sess:
        session_id = "s" * 32

        class view:
            width, height = 800, 600

        async def render_frame(self):
            return b"jpeg"

        def levels(self):
            return {"threshold": None, "data_min": 1.0, "data_max": 2.0, "pending": False}

    class _Manager:
        async def start(self):
            pass

        async def create_session(self, **kw):
            seen.update(kw)
            return _Sess()

    class _VisivoSession:
        datasets = {"vbt_1": {"kind": "vbt", "head_path": "/data/snap.bin.head",
                              "header": {"kind": "point"}}}

    monkeypatch.setattr(render_router, "server_side_rendering_available", lambda: True)
    monkeypatch.setattr(render_router, "get_render_manager", lambda: _Manager())
    reply = asyncio.new_event_loop().run_until_complete(render_router.create_session(
        CreateRenderSessionRequest(dataset_id="vbt_1"), token="tok",
        session=_VisivoSession()))
    assert reply.valid and reply.data_max == 2.0
    assert seen["dataset_path"] == "/data/snap.bin.head"
    assert seen["factory"] == "app.render_session.points:PointRenderSession"


def test_a_bad_command_is_refused_and_changes_nothing(tmp_path):
    s, run, loop = _session(_table(tmp_path))
    try:
        run(s.apply_command({"type": "set_point_fields", "x": "Y", "y": "Nope"}))
        assert (s.view.x, s.view.y) == ("X", "Y")
        run(s.apply_command({"type": "set_color", "field": "Mass", "min": "abc"}))
        assert s.view.color_field is None
        run(s.apply_command({"type": "set_filters",
                             "filters": [{"field": "X", "op": ">", "value": "abc"}]}))
        assert s.view.filters == [] and len(s._rows) == 4000
    finally:
        run(s.close())
        loop.close()


def test_resending_the_same_fields_rebuilds_nothing(tmp_path, monkeypatch):
    s, run, loop = _session(_table(tmp_path))
    try:
        calls = []
        monkeypatch.setattr(s, "_rebuild_points_on_thread", lambda: calls.append(1))
        run(s.apply_command({"type": "set_point_fields", "x": "X", "y": "Y", "z": "Z"}))
        run(s.apply_command({"type": "set_filters", "filters": []}))
        assert calls == []
    finally:
        run(s.close())
        loop.close()


def test_a_drag_keeps_each_set_on_its_own_mapper(tmp_path):
    """Swapping a mapper's input re-uploads every point: each set has its own."""
    s, run, loop = _session(_table(tmp_path))
    try:
        full = s._mappers[("points", False)]
        run(s.apply_command({"type": "set_interactive", "value": True}))
        assert s._actor.GetMapper() is s._mappers[("points", True)]
        run(s.apply_command({"type": "set_interactive", "value": False}))
        assert s._actor.GetMapper() is full and full.GetInput() is s._polydata
    finally:
        run(s.close())
        loop.close()


def test_levels_follow_a_colour_change_in_the_session_process(tmp_path):
    """In its own process the session answers levels after the commands sent
    before: a colour change, not answered itself, is not lost."""
    from app.render_session._process import RenderSessionProcess

    async def scenario():
        s = RenderSessionProcess("p" * 32, _table(tmp_path),
                                 factory="app.render_session.points:PointRenderSession")
        await s.initialize()
        try:
            await s.apply_command({"type": "set_color", "field": "Mass", "lut": "Viridis"})
            await s.refresh_levels()
            before = s.levels()
            # The camera a ray-traced still of this view is rendered from
            # follows the view.
            await s.apply_command({"type": "rotate", "d_azimuth": 90.0})
            await s.refresh_levels()
            return before, s.levels()["camera"]
        finally:
            await s.close()

    levels, turned = asyncio.new_event_loop().run_until_complete(scenario())
    assert (levels["data_min"], levels["data_max"]) == (1.0, 100.0)
    camera = levels["camera"]
    assert set(camera) >= {"position", "focal_point", "view_up", "view_angle", "parallel"}
    assert turned["focal_point"] == pytest.approx(camera["focal_point"])
    assert turned["position"] != pytest.approx(camera["position"])


def test_a_point_table_opens_as_a_cartesian_catalogue_in_place(tmp_path):
    """The 3-D catalogue viewer reads a VBT point table (issue #83): one
    column per field, a view of the binary, X/Y/Z as cartesian coordinates."""
    from pathlib import Path

    from app.dependencies import _catalogue_metadata

    head = Path(_table(tmp_path))
    frame, meta = _catalogue_metadata(head, "vbt")
    assert list(frame.columns) == ["X", "Y", "Z", "Mass"] and len(frame) == 4000
    # A read-only view of the mapped binary, not a copy.
    mass = frame["Mass"].to_numpy()
    assert not mass.flags.owndata and not mass.flags.writeable
    coords = meta["coordinate_fields"]
    assert coords["system"] == "cartesian" and (coords["x"], coords["z"]) == ("X", "Z")
    assert meta["format"] == "vbt"
    # By its binary too.
    assert len(_catalogue_metadata(Path(str(head)[:-5]), "vbt")[0]) == 4000


def test_the_render_route_draws_a_vbt_opened_as_a_catalogue(monkeypatch):
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

    class _VisivoSession:
        datasets = {"cat_1": {"kind": "catalogue", "format": "vbt",
                              "path": "/data/snap.bin.head"},
                    "cat_2": {"kind": "catalogue", "format": "csv", "path": "/data/c.csv"}}

    monkeypatch.setattr(render_router, "server_side_rendering_available", lambda: True)
    monkeypatch.setattr(render_router, "get_render_manager", lambda: _Manager())
    run = asyncio.new_event_loop().run_until_complete
    run(render_router.create_session(render_router.CreateRenderSessionRequest(dataset_id="cat_1"),
                                     token="tok", session=_VisivoSession()))
    assert seen["dataset_path"] == "/data/snap.bin.head"
    assert seen["factory"].endswith(":PointRenderSession")
    from fastapi import HTTPException
    with pytest.raises(HTTPException) as err:
        run(render_router.create_session(
            render_router.CreateRenderSessionRequest(dataset_id="cat_2"),
            token="tok", session=_VisivoSession()))
    assert err.value.status_code == 422


def test_the_clients_own_colours_are_drawn(tmp_path):
    """The catalogue viewer sends its colour function point by point (its ramp,
    scale and range), so the backend draws its colours."""
    s, run, loop = _session(_table(tmp_path))
    try:
        points = [[1.0, 0.0, 0.0, 1.0], [100.0, 1.0, 0.0, 0.0]]
        run(s.apply_command({"type": "set_color", "field": "Mass", "points": points,
                             "log": True, "min": 1.0, "max": 100.0}))
        # Sampled into a table, as the desktop colours: a log one here.
        lut = s._mappers[("points", False)].GetLookupTable()
        assert lut.GetNumberOfTableValues() == 1024 and lut.GetScale() == 1
        def colour(value):
            rgb = [0.0, 0.0, 0.0]
            lut.GetColor(value, rgb)
            return rgb
        assert np.allclose(colour(1.0), (0.0, 0.0, 1.0), atol=0.01)
        assert np.allclose(colour(100.0), (1.0, 0.0, 0.0), atol=0.01)
        assert np.allclose(colour(10.0), (0.5, 0.0, 0.5), atol=0.01)
        with pytest.raises(ValueError):
            s._apply_command_on_thread({"type": "set_color", "field": "Mass", "points": "x"})
    finally:
        run(s.close())
        loop.close()


def test_a_vbt_without_xyz_names_opens_on_its_first_three_columns(tmp_path):
    from pathlib import Path

    from app.dependencies import _catalogue_metadata

    n = 10
    np.arange(4 * n, dtype="<f4").tofile(tmp_path / "p.bin")
    # L and B are a luminosity and a field here, not galactic l and b.
    (tmp_path / "p.bin.head").write_text(f"float\n4\n{n}\nlittle\npos0\npos1\nL\nB\n")
    _, meta = _catalogue_metadata(Path(tmp_path / "p.bin.head"), "vbt")
    coords = meta["coordinate_fields"]
    assert coords["system"] == "cartesian" and not coords["glon"]
    assert (coords["x"], coords["y"], coords["z"]) == ("pos0", "pos1", "L")


def test_numeric_filters_take_only_the_page(tmp_path):
    """The query on a table read in place filters with a mask and copies only
    the page asked for; same rows, same total as the general path."""
    import pandas as pd

    from app.dependencies import _apply_tabular_query
    from app.schemas import TabularFilter, TabularSortSpec

    frame = pd.DataFrame({"X": np.arange(100.0), "M": np.arange(100.0) % 7})
    filters = [TabularFilter(field="X", op=">=", value="10"),
               TabularFilter(field="M", op="=", value="3")]
    page, total = _apply_tabular_query(frame, filters, None, limit=4, offset=2)
    expected = frame[(frame.X >= 10) & (frame.M == 3)]
    assert total == len(expected)
    assert list(page.index) == list(expected.index[2:6])
    # A sort, or a text operator, takes the general path: still right.
    page, total = _apply_tabular_query(frame, filters, TabularSortSpec(field="X",
                                       direction="desc"), limit=2, offset=0)
    assert list(page.X) == list(expected.X[::-1][:2]) and total == len(expected)


def test_a_filter_does_not_restyle_the_splats(tmp_path):
    s, run, loop = _session(_table(tmp_path))
    try:
        run(s.apply_command({"type": "set_point_style", "mode": "gaussian", "size": 3,
                             "intensity": 35}))
        mapper = s._mappers[("gaussian", False)]
        radius = mapper.GetScaleFactor()
        run(s.apply_command({"type": "set_filters",
                             "filters": [{"field": "X", "op": ">", "value": "0"}]}))
        run(s.apply_command({"type": "set_point_style", "mode": "gaussian", "size": 3,
                             "intensity": 36}))
        assert mapper.GetScaleFactor() == pytest.approx(radius)
    finally:
        run(s.close())
        loop.close()
