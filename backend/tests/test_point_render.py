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
import sys
if sys.platform != "win32":
    # A window refused (macOS without a window server: a sandbox, a login
    # session over ssh) aborts in VTK: the probe ends with a plain exit
    # instead, so the system files no crash report and shows no dialog.
    import ctypes, signal
    libc = ctypes.CDLL(None)
    libc.signal.restype = ctypes.c_void_p
    libc.signal.argtypes = [ctypes.c_int, ctypes.c_void_p]
    for sig in (signal.SIGABRT, signal.SIGSEGV, signal.SIGBUS):
        libc.signal(sig, ctypes.cast(libc._exit, ctypes.c_void_p))
import vtk
# The backend's own offscreen window: on a Mac it hides this process from the
# Dock before AppKit starts; a bare vtkRenderWindow would leave a Python tile
# behind in the Dock after every run of the suite.
from app.render_session._window import new_offscreen_window, offscreen_kind
kind = offscreen_kind()
if kind is None:
    sys.exit(1)
w = new_offscreen_window(kind)
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
    from pathlib import Path
    backend = Path(__file__).resolve().parent.parent   # so the probe imports app
    probe = subprocess.run([sys.executable, "-c", _RENDER_PROBE], capture_output=True,
                           timeout=60, cwd=backend)
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


def test_a_camera_given_whole_is_the_views(tmp_path):
    s, run, loop = _session(_table(tmp_path))
    try:
        camera = {"position": [3.0, 4.0, 12.0], "focal_point": [0.5, 0.5, 0.5],
                  "view_up": [0.0, 1.0, 0.0], "view_angle": 25.0, "parallel": True,
                  "parallel_scale": 2.5}
        run(s.apply_command({"type": "set_camera", **camera}))
        got = s._camera_on_thread()
        assert np.allclose(got["position"], camera["position"])
        assert np.allclose(got["focal_point"], camera["focal_point"])
        assert got["view_angle"] == pytest.approx(25.0)
        assert got["parallel"] and got["parallel_scale"] == pytest.approx(2.5)
        with pytest.raises(ValueError):
            s._apply_command_on_thread({"type": "set_camera", "position": [0, 0, 1]})
        with pytest.raises(ValueError):
            s._apply_command_on_thread({"type": "set_camera", "position": [1, 1, 1],
                                        "focal_point": [1, 1, 1], "view_up": [0, 1, 0]})
        for bad in ({"position": 5, "focal_point": [0, 0, 0], "view_up": [0, 1, 0]},
                    {**camera, "view_angle": [1, 2]},
                    {"position": [0, 0, 5], "focal_point": [0, 0, 0], "view_up": [0, 0, 1]}):
            with pytest.raises(ValueError):
                s._apply_command_on_thread({"type": "set_camera", **bad})
        assert np.allclose(s._camera_on_thread()["position"], camera["position"])
    finally:
        run(s.close())
        loop.close()


# ── Points far from zero (issue #206) ───────────────────────────────────────


def _far_table(tmp_path, x, y=None, z=None, name="far"):
    """A float64 VBT: X, Y, Z (zeros when not given)."""
    n = len(x)
    cols = [np.asarray(x, float), np.zeros(n) if y is None else np.asarray(y, float),
            np.zeros(n) if z is None else np.asarray(z, float)]
    np.stack(cols).astype("<f8").tofile(tmp_path / f"{name}.bin")
    (tmp_path / f"{name}.bin.head").write_text(f"double\n3\n{n}\nlittle\nX\nY\nZ\n")
    return str(tmp_path / f"{name}.bin.head")


def _table_positions(s, poly=None):
    """The drawn points in the table's coordinates: their float32 local
    coordinates through the actor's matrix, in float64."""
    from vtk.util.numpy_support import vtk_to_numpy
    poly = poly or s._polydata
    local = vtk_to_numpy(poly.GetPoints().GetData()).astype(float)
    m = np.array([[s._actor.GetUserMatrix().GetElement(i, j) for j in range(4)]
                  for i in range(4)])
    return local @ m[:3, :3].T + m[:3, 3]


def test_points_far_from_zero_keep_their_places(tmp_path):
    """1e9, 1e9 + 1, 1e9 + 2 were one float32 value, so one point: the
    session's points, drag sample, bounds, outline, splat basis and camera
    are in the table's own coordinates, the points still float32."""
    import vtk
    x = 1e9 + np.array([0.0, 1.0, 2.0])
    s, run, loop = _session(_far_table(tmp_path, x, y=[0.0, 0.5, 1.0],
                                       z=[-3e9, -3e9 - 1, -3e9]))
    try:
        assert s._polydata.GetPoints().GetDataType() == vtk.VTK_FLOAT
        got = _table_positions(s)
        assert got[:, 0].tolist() == x.tolist()
        assert got[:, 2].tolist() == [-3e9, -3e9 - 1, -3e9]
        assert np.array_equal(_table_positions(s, s._lod_polydata), got)
        bounds = [1e9, 1e9 + 2, 0.0, 1.0, -3e9 - 1, -3e9]
        assert s._bounds == bounds
        assert list(s._outline.GetBounds()) == bounds
        assert list(s._splat_basis[0]) == bounds
        # The camera (and a ray-traced still made from it) looks at the
        # table's own centre.
        assert s._camera_on_thread()["focal_point"] == pytest.approx(
            [1e9 + 1, 0.5, -3e9 - 0.5], abs=1e-6)
        # A filter keeps the table's coordinates.
        run(s.apply_command({"type": "set_filters",
                             "filters": [{"field": "X", "op": ">", "value": 1e9 + 0.5}]}))
        assert _table_positions(s)[:, 0].tolist() == [1e9 + 1, 1e9 + 2]
        assert s._bounds[:2] == [1e9 + 1, 1e9 + 2]
    finally:
        run(s.close())
        loop.close()


def test_rows_beyond_float32_are_not_drawn(tmp_path):
    """Regression guard: as before, and as the VR page's sample does."""
    s, run, loop = _session(_far_table(tmp_path, [1.0, 1e39, 2.0, -np.inf, 3.0],
                                       y=[0.0, 0.0, 5e38, 0.0, np.nan]))
    try:
        assert s._rows.tolist() == [0]
        assert s._bounds[:2] == [1.0, 1.0]
    finally:
        run(s.close())
        loop.close()


def test_an_axis_read_as_fractions_keeps_round_splats(tmp_path):
    """The first row (X's reference) has no Y, so X's offsets are 1e6 from
    the rows kept and X is read as fractions of its range: the map takes one
    scale on every axis (VTK scales a splat by it) and the splats keep their
    radius in the table's units."""
    s, run, loop = _session(_far_table(tmp_path, [1e6, 0.0, 1.0, 2.0],
                                       y=[np.nan, 0.0, 4.0, 1.0], z=[0.0, 1.0, 0.0, 1.0]))
    try:
        assert s._rows.tolist() == [1, 2, 3]
        assert np.allclose(np.diag(s._local_to_table)[:3], 2.0)   # X's span
        assert np.allclose(_table_positions(s), [[0, 0, 1], [1, 4, 0], [2, 1, 1]], atol=1e-6)
        assert s._bounds == [0.0, 2.0, 0.0, 4.0, 0.0, 1.0]
        run(s.apply_command({"type": "set_point_style", "mode": "gaussian", "size": 3,
                             "intensity": 35}))
        radius, _ = gaussian_splat(s._bounds, 3, 3, 35)
        assert s._mappers[("gaussian", False)].GetScaleFactor() * 2.0 == pytest.approx(radius)
    finally:
        run(s.close())
        loop.close()


def test_uniform_scale_divides_the_other_axes_down():
    from app.point_sample import PlacedRows
    from app.render_session.points import uniform_scale
    xyz = np.array([[1.0, 0.5, 2.0], [3.0, 1.0, 4.0]], np.float32)
    p = PlacedRows(np.arange(2), xyz.copy(), np.array([10.0, 20.0, 30.0]),
                   np.array([1.0, 8.0, 1.0]), np.array([[1.0, 3.0], [0.5, 1.0], [2.0, 4.0]]),
                   [11.0, 13.0, 24.0, 28.0, 32.0, 34.0])
    before = xyz.astype(float) @ np.diag(p.scale) + p.origin
    q = uniform_scale(p)
    assert q.scale.tolist() == [8.0, 8.0, 8.0]
    assert np.allclose(q.xyz.astype(float) @ np.diag(q.scale) + q.origin, before)
    assert np.allclose(q.local * 8.0, [[1, 3], [4, 8], [2, 4]])
    assert uniform_scale(q) is q


def _runs(lit_columns: np.ndarray) -> int:
    """Separate runs of lit columns."""
    idx = np.flatnonzero(lit_columns)
    return 0 if not idx.size else 1 + int((np.diff(idx) > 1).sum())


def test_three_points_far_from_zero_are_drawn_apart(tmp_path, can_render):
    s, run, loop = _session(_far_table(tmp_path, 1e9 + np.array([0.0, 1.0, 2.0])))
    try:
        for cmd in ({"type": "set_show_outline", "visible": False},
                    {"type": "set_show_axes", "visible": False},
                    {"type": "set_show_colorbar", "visible": False},
                    {"type": "set_point_style", "mode": "points", "size": 6}):
            run(s.apply_command(cmd))
        assert _runs(_image(run(s.render_frame())).any(axis=0)) == 3
        run(s.apply_command({"type": "set_interactive", "value": True}))   # the drag's set
        assert _runs(_image(run(s.render_frame())).any(axis=0)) == 3
    finally:
        run(s.close())
        loop.close()


def _centres(lit: np.ndarray) -> list[float]:
    """The centre (pixels from the left edge) of each run of lit columns,
    weighted by how many pixels each column lights."""
    profile = lit.sum(axis=0).astype(float)
    idx = np.flatnonzero(profile)
    runs = np.split(idx, np.flatnonzero(np.diff(idx) > 1) + 1) if idx.size else []
    return [float(np.average(r + 0.5, weights=profile[r])) for r in runs]


def test_a_ray_traced_still_lands_on_the_sessions_points(tmp_path, can_render):
    """Issue #208: three particles at 1e9 + [0, 1, 2] (z 3e9 below 0) are
    three in a ray-traced still made from the session's camera, where the
    remote point session draws them."""
    from app.routers import particle_render as router
    path = _far_table(tmp_path, 1e9 + np.array([0.0, 1.0, 2.0]), y=[0.0, 0.5, 1.0],
                      z=[-3e9, -3e9, -3e9])
    s, run, loop = _session(path)
    try:
        for cmd in ({"type": "set_show_outline", "visible": False},
                    {"type": "set_show_axes", "visible": False},
                    {"type": "set_show_colorbar", "visible": False},
                    {"type": "set_point_style", "mode": "points", "size": 6}):
            run(s.apply_command(cmd))
        drawn = _image(run(s.render_frame()))
        camera, width, height = s._camera_on_thread(), s.view.width, s.view.height
    finally:
        run(s.close())
        loop.close()
    assert drawn.shape == (height, width)
    req = router.ParticleRenderRequest(dataset_id="d", radius_scale=0.02, width=width,
                                       height=height, cameras=[camera])
    entry = {"kind": "catalogue", "format": "vbt", "path": path}
    light = router._frame(router._prepare(entry, req), req, req.cameras[0], width, height,
                          req.tone, None)[1].sum(axis=-1)
    traced = light > 0.05 * light.max()
    points, rays = _centres(drawn), _centres(traced)
    assert len(points) == len(rays) == 3
    assert np.abs(np.array(points) - rays).max() <= 1.0
    # And the rows: y rises with x, the image counts rows from the top.
    assert np.abs(np.array(_centres(drawn.T)) - _centres(traced.T)).max() <= 1.0
