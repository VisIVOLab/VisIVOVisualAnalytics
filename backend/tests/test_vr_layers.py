"""The VR page's other data types (issue #190): point tables sampled for a
headset and drawn in stereo by the point session, catalogue sources placed in
a cube's pixels, and a velocity field's streamlines traced as the desktop
traces them."""
from __future__ import annotations

import asyncio
import math

import numpy as np
import pytest
from astropy.io import fits
from astropy.wcs import WCS

from app import cube_catalogue as cc
from app import point_sample as ps


# ── Point samples ────────────────────────────────────────────────────────────


def _vbt(tmp_path, n=5000, name="snap"):
    rng = np.random.default_rng(3)
    x, y, z = rng.uniform(-10, 10, n), rng.uniform(0, 5, n), rng.uniform(100, 102, n)
    mass = np.exp(rng.normal(0, 1, n))
    mass[7] = np.nan
    data = np.stack([x, y, z, mass]).astype("<f4")
    data.tofile(tmp_path / f"{name}.bin")
    (tmp_path / f"{name}.bin.head").write_text(f"float\n4\n{n}\nlittle\nX\nY\nZ\nMass\n")
    return str(tmp_path / f"{name}.bin.head"), data


def test_prefix_order_is_a_permutation_whose_prefixes_are_even():
    for n in (1, 2, 7, 1000, 4097):
        o = ps.prefix_order(n)
        assert sorted(o.tolist()) == list(range(n))
    o = ps.prefix_order(1024)
    # The first 2^k rows are every (1024 / 2^k)-th row.
    for k in (1, 3, 6):
        assert sorted(o[: 2 ** k].tolist()) == list(range(0, 1024, 1024 // 2 ** k))


def test_sample_quantises_positions_and_colours_and_keeps_rows_whole(tmp_path):
    from app.point_table import open_point_table
    head, data = _vbt(tmp_path)
    t = open_point_table(head)
    s = ps.sample_points(t, 1000, colour_field="Mass", colour_log=True)
    assert len(s.positions) == 1000 and s.total_rows == 5000 and s.step == pytest.approx(5.0)
    rows = ps.even_rows(5000, 1000)[ps.prefix_order(1000)]
    lo, hi = np.array(s.bounds[0::2]), np.array(s.bounds[1::2])
    for i in (0, 1, 17, 999):
        xyz = lo + s.positions[i] / 65535.0 * (hi - lo)
        assert np.allclose(xyz, data[:3, rows[i]], atol=(hi - lo).max() / 65535 * 0.51)
        m = data[3, rows[i]]
        if np.isfinite(m):
            code = s.colour[i]
            v = 10 ** ps.dequantise(code, *np.log10(s.colour_range))
            assert v == pytest.approx(max(m, s.colour_range[0]), rel=2e-4)
    body = s.encode()
    assert len(body) == 1000 * 8
    h = ps.headers(s)
    assert h["X-Visivo-Count"] == "1000" and h["X-Visivo-Color-Log"] == "1"


def test_sample_refuses_unknown_columns(tmp_path):
    from app.point_table import open_point_table
    t = open_point_table(_vbt(tmp_path)[0])
    with pytest.raises(ValueError):
        ps.sample_points(t, 10, colour_field="Nope")
    with pytest.raises(ValueError):
        ps.sample_points(t, 10, axes=("X", "Y", "Nope"))


@pytest.mark.asyncio
async def test_points_routes(client, auth_headers, tmp_path, opened_cube):
    head, _ = _vbt(tmp_path, name="route")
    r = await client.post("/v1/vbt/open", json={"path": head}, headers=auth_headers)
    did = r.json()["dataset_id"]
    info = (await client.post("/v1/points/info", json={"dataset_id": did}, headers=auth_headers)).json()
    assert info["num_rows"] == 5000 and info["axes"] == ["X", "Y", "Z"] and "Mass" in info["fields"]
    r = await client.post("/v1/points/sample_bin", json={"dataset_id": did, "max_points": 2000,
                                                          "color_field": "Mass"}, headers=auth_headers)
    assert r.status_code == 200 and len(r.content) == 2000 * 8
    assert r.headers["x-visivo-count"] == "2000" and r.headers["x-visivo-color-field"] == "Mass"
    r = await client.post("/v1/points/sample_bin", json={"dataset_id": opened_cube, "max_points": 10},
                          headers=auth_headers)
    assert r.status_code == 422


# ── Stereo frames of a point session ─────────────────────────────────────────


def test_data_to_model_is_checked_and_the_default_is_the_pages_box():
    from app.render_session._stereo_points import default_data_to_model, parse_data_to_model
    m = default_data_to_model([0, 10, -2, 2, 5, 6])
    assert np.allclose(m @ [0, -2, 5, 1], [-0.5, -0.2, -0.05, 1])
    assert np.allclose(m @ [10, 2, 6, 1], [0.5, 0.2, 0.05, 1])
    good = m.T.ravel().tolist()
    assert np.allclose(parse_data_to_model(good), m)
    bad = np.diag([1.0, 2.0, 1.0, 1.0]).T.ravel().tolist()
    with pytest.raises(ValueError):
        parse_data_to_model(bad)


def test_point_eye_reaches_the_clip_coordinates_of_the_page():
    """A data point lands where the page's projection @ view @ model @
    data_to_model puts it, through the scaled-back camera VTK is given."""
    from app.render_session._stereo import projection_from_fov
    from app.render_session._stereo_points import default_data_to_model, point_eye
    bounds = [-5, 15, 0, 4, 100, 103]
    d2m = default_data_to_model(bounds)
    model = np.eye(4)
    model[:3, :3] *= 0.6
    model[:3, 3] = [0.1, 1.4, -0.8]
    view = np.eye(4)
    view[:3, 3] = [-0.03, -1.6, 0.0]
    proj = projection_from_fov({"left": -0.9, "right": 0.8, "up": 0.85, "down": -0.95,
                                "near": 0.05, "far": 100})
    e = point_eye(view, proj, model @ d2m, bounds)
    for p in ([0, 1, 101, 1], [15, 4, 103, 1], [-5, 0, 100, 1]):
        page = proj @ view @ model @ d2m @ np.array(p, float)
        vtk_side = e["projection"] @ e["view"] @ np.array(p, float)
        assert np.allclose(page[:2] / page[3], vtk_side[:2] / vtk_side[3], atol=1e-9)


def test_point_sessions_are_stereo_capable():
    from app.routers.render import _supports_stereo

    class Remote:
        _factory = "app.render_session.points:PointRenderSession"
    assert _supports_stereo(Remote())


def test_a_point_session_draws_a_stereo_frame_where_the_page_draws_its_points(tmp_path):
    pytest.importorskip("vtk")
    import subprocess
    import sys
    from pathlib import Path
    from tests.test_point_render import _RENDER_PROBE
    backend = Path(__file__).resolve().parent.parent
    probe = subprocess.run([sys.executable, "-c", _RENDER_PROBE], capture_output=True,
                           timeout=60, cwd=backend)
    if probe.returncode != 0:
        pytest.skip("no offscreen OpenGL here (no GPU or display)")
    from app.render_session._stereo import parse_frame
    from app.render_session._stereo_points import default_data_to_model
    from app.render_session.points import PointRenderSession
    from PIL import Image
    import io
    # One tight cluster at x = +8 (of -10..10): the right half of the image.
    n = 3000
    rng = np.random.default_rng(5)
    x = np.concatenate([rng.normal(8, 0.2, n), [-10.0, 10.0]])
    y = np.concatenate([rng.normal(0, 0.2, n), [-10.0, 10.0]])
    z = np.concatenate([rng.normal(0, 0.2, n), [-10.0, 10.0]])
    np.stack([x, y, z]).astype("<f4").tofile(tmp_path / "c.bin")
    (tmp_path / "c.bin.head").write_text(f"float\n3\n{n + 2}\nlittle\nX\nY\nZ\n")
    s = PointRenderSession("p" * 32, str(tmp_path / "c.bin.head"))
    loop = asyncio.new_event_loop()
    try:
        loop.run_until_complete(s.initialize())
        loop.run_until_complete(s.apply_command({"type": "set_point_style", "mode": "points", "size": 4}))
        d2m = default_data_to_model([-10, 10, -10, 10, -10, 10])
        model = np.eye(4)
        model[:3, 3] = [0, 0, -1.5]        # the box 1.5 m ahead
        view = np.eye(4).T.ravel().tolist()
        proj = [1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, -1.002, -1, 0, 0, -0.2002, 0]
        pose = {"type": "xr_pose", "pose_id": 7, "model": model.T.ravel().tolist(),
                "views": [{"eye": e, "view": view, "projection": proj, "width": 128, "height": 96}
                          for e in ("left", "right")],
                "depth": {"scale": 4}, "data_to_model": d2m.T.ravel().tolist()}
        loop.run_until_complete(s.apply_command(pose))
        frame = loop.run_until_complete(s.render_frame())
        header, jpeg, depth = parse_frame(frame.to_bytes())
        assert header["pose_id"] == 7 and header["points"] == n + 2
        img = np.asarray(Image.open(io.BytesIO(jpeg)).convert("L"), dtype=float)
        left = img[:, :128]
        lit = np.argwhere(left > 80)
        assert len(lit) > 0
        # x = +8 of 20 -> model 0.4 at 1.5 m with a 90° view: column 64 + 0.4/1.5*64.
        assert abs(np.median(lit[:, 1]) - (64 + 0.4 / 1.5 * 64)) < 4
        assert header["depth"] is not None and header["depth"]["source"] == "surface"
        loop.run_until_complete(s.apply_command({"type": "xr_stop"}))
        assert not s._xr_active()
    finally:
        loop.run_until_complete(s.close())
        loop.close()


# ── Catalogue sources in a cube ──────────────────────────────────────────────


def _car_cube(tmp_path, ctype=("GLON-CAR", "GLAT-CAR"), spec=("VRAD", 80000.0, 500.0, "m/s")):
    h = fits.Header()
    h["NAXIS"] = 3
    h["CTYPE1"], h["CRVAL1"], h["CRPIX1"], h["CDELT1"], h["CUNIT1"] = ctype[0], 30.0, 40.5, -6 / 3600, "deg"
    h["CTYPE2"], h["CRVAL2"], h["CRPIX2"], h["CDELT2"], h["CUNIT2"] = ctype[1], 0.5, 30.5, 6 / 3600, "deg"
    h["CTYPE3"], h["CRVAL3"], h["CRPIX3"], h["CDELT3"], h["CUNIT3"] = spec[0], spec[1], 1.0, spec[2], spec[3]
    h["RESTFRQ"] = 1.420405752e9
    path = tmp_path / f"cube_{ctype[0][:4]}_{spec[0]}.fits"
    fits.PrimaryHDU(np.zeros((20, 60, 80), np.float32), header=h).writeto(path, overwrite=True)
    return str(path), WCS(h)


def test_sources_land_on_their_pixel_in_a_glon_car_cube(tmp_path):
    import pandas as pd
    path, w = _car_cube(tmp_path)
    geo = cc.cube_geometry(path)
    assert geo.shape == (80, 60, 20) and cc.celestial_frame(geo.cel) == "galactic"
    truth = np.array([[3.2, 4.7, 2.0], [71.6, 50.25, 17.5], [40.0, 30.0, 9.0]])
    world = w.all_pix2world(truth, 0)
    # ICRS input on a Galactic cube: converted, then projected (CAR).
    from astropy.coordinates import SkyCoord
    icrs = SkyCoord(world[:, 0], world[:, 1], unit="deg", frame="galactic").icrs
    df = pd.DataFrame({"ra": icrs.ra.deg, "dec": icrs.dec.deg, "vlsr": world[:, 2] / 1000.0,
                       "name": ["a", "b", "c"]})
    out = cc.place_table(df, {"system": "sky", "ra": "ra", "dec": "dec"}, geo.cel, geo.wcs,
                         geo.shape, "name")
    got = np.array([[s["x"], s["y"], s["z"]] for s in out.sources])
    assert np.allclose(got, truth, atol=1e-6)
    assert out.spectral["used"] and [s["label"] for s in out.sources] == ["a", "b", "c"]


def test_spectral_columns_in_frequency_and_in_another_convention(tmp_path):
    import astropy.units as u
    path, w = _car_cube(tmp_path)
    geo = cc.cube_geometry(path)
    z = np.array([0.0, 7.5, 19.0])
    v = w.spectral.all_pix2world(z[:, None], 0)[:, 0] * u.m / u.s          # radio velocity
    rest = 1.420405752e9 * u.Hz
    freq = v.to(u.Hz, u.doppler_radio(rest))
    zz, note = cc.spectral_pixels(geo.wcs, freq.value, "freq")
    assert np.allclose(zz, z, atol=1e-6) and "freq" in note
    vopt = freq.to(u.km / u.s, u.doppler_optical(rest))
    zz, _ = cc.spectral_pixels(geo.wcs, vopt.value, "v_opt")
    assert np.allclose(zz, z, atol=1e-6)
    zz, _ = cc.spectral_pixels(geo.wcs, v.to(u.m / u.s).value, "v_app")    # the axis's own unit
    assert np.allclose(zz, z, atol=1e-9)
    zz, _ = cc.spectral_pixels(geo.wcs, v.to(u.km / u.s).value, "v_app", unit="km/s")
    assert np.allclose(zz, z, atol=1e-9)


def test_vlkb_sources_are_ellipses_built_in_pixels_as_on_the_desktop(tmp_path):
    path, w = _car_cube(tmp_path)
    geo = cc.cube_geometry(path)
    lon, lat = w.celestial.all_pix2world([[20.0, 15.0]], 0)[0]
    rows = [{"glonft": str(lon), "glatft": str(lat), "fwhmaft": "36", "fwhmbft": "18", "paft": "30",
             "glon250": "-9999", "glat250": "-9999", "fwhma250": "24", "fwhmb250": "24", "pa250": "0",
             "designationft": "HIGALBM30.0+0.5"}]
    out = cc.place_vlkb_sources(rows, geo.cel, geo.shape, ("ft", "250"))
    assert len(out.sources) == 2
    ft = out.sources[0]
    assert ft["x"] == pytest.approx(20.0, abs=1e-6) and ft["y"] == pytest.approx(15.0, abs=1e-6)
    # Semi-major 18" = 3 px along +x rotated 30° counter-clockwise.
    e = np.array(ft["ellipse"])
    assert e[0] == pytest.approx([20 + 3 * math.cos(math.radians(30)), 15 + 3 * math.sin(math.radians(30))])
    # The 250 µm band has no own centroid (-9999): the band-merged one.
    assert out.sources[1]["x"] == pytest.approx(20.0, abs=1e-6)


def test_filament_contours_are_parsed_and_projected(tmp_path):
    path, w = _car_cube(tmp_path)
    geo = cc.cube_geometry(path)
    pts = w.celestial.all_pix2world([[10.0, 10.0], [20.0, 12.0], [30.0, 20.0]], 0)
    contour = "_".join(f"{p[0]:.9f}" for p in pts) + "_," + "_".join(f"{p[1]:.9f}" for p in pts) + "_"
    out = cc.place_contours([{"idfil": "42", "contour": f'"{contour}"'}], geo.cel, geo.shape)
    assert out.polylines[0]["label"] == "filament 42"
    assert np.allclose(out.polylines[0]["points"], [[10, 10], [20, 12], [30, 20]], atol=1e-5)
    assert cc.parse_contour("no comma") == []


@pytest.mark.asyncio
async def test_catalogue_pixels_route(client, auth_headers, tmp_path, monkeypatch):
    path, w = _car_cube(tmp_path, ctype=("RA---TAN", "DEC--TAN"))
    sky = w.celestial.all_pix2world([[5.0, 6.0], [70.0, 55.0], [500.0, 500.0]], 0)
    csv = tmp_path / "cat.csv"
    csv.write_text("name,ra,dec\n" + "\n".join(f"s{i},{float(a)!r},{float(b)!r}" for i, (a, b) in enumerate(sky)) + "\n")
    cube = (await client.post("/v1/datasets/open", json={"path": path}, headers=auth_headers)).json()
    cat = (await client.post("/v1/catalogue/open", json={"path": str(csv), "format": "csv"},
                             headers=auth_headers)).json()
    r = await client.post("/v1/cube/catalogue_pixels", json={"dataset_id": cube["dataset_id"],
                          "catalogue_id": cat["dataset_id"]}, headers=auth_headers)
    j = r.json()
    assert r.status_code == 200 and j["frame"] == "icrs"
    assert j["n_sources"] == 2 and j["n_outside"] == 1, {k: v for k, v in j.items() if k != "sources"}
    assert [round(s["x"], 6) for s in j["sources"]] == [5.0, 70.0]
    # VLKB, without the network: the rows the service would give.
    from app.routers import vr_layers
    lon, lat = 30.0, 0.5
    monkeypatch.setattr(vr_layers, "_vlkb_rows", lambda url, adql, n: ([
        {"idfil": "1", "contour": "0_1_,0_1_"}] if "filaments" in adql else [], False))
    g, _ = _car_cube(tmp_path)
    gid = (await client.post("/v1/datasets/open", json={"path": g}, headers=auth_headers)).json()["dataset_id"]
    r = await client.post("/v1/cube/catalogue_pixels", json={"dataset_id": gid, "source": "vlkb_filaments"},
                          headers=auth_headers)
    assert r.status_code == 200 and r.json()["n_polylines"] == 0 and r.json()["n_outside"] == 1
    del lon, lat


def test_vlkb_query_is_the_desktops():
    from app.routers.vr_layers import _vlkb_query
    q = _vlkb_query("vlkb_filaments", (29.9, 30.1, 0.4, 0.6))
    assert q.startswith("SELECT idfil,contour FROM filaments.filaments WHERE glon BETWEEN 29.900000")
    assert "glat BETWEEN 0.400000 AND 0.600000" in q
    q = _vlkb_query("vlkb_sources", (29.9, 30.1, 0.4, 0.6))
    assert q.startswith("SELECT * FROM compactsources.sed_view_final WHERE glonft BETWEEN")


# ── Velocity fields ──────────────────────────────────────────────────────────


def _flow(tmp_path, n=24):
    g = np.linspace(-1, 1, n)
    z, y, x = np.meshgrid(g, g, g, indexing="ij")
    v = np.stack([-x + 0.5 * y, -y - 0.5 * x, -0.3 * z]).astype(np.float32) * 100
    p = tmp_path / "flow.fits"
    fits.PrimaryHDU(v).writeto(p, overwrite=True)
    return str(p)


def _desktop_lines(raw, nx, ny, nz, spacing, origin, seeds, seed):
    """The desktop's own pipeline (vtkWindowVectorField::makeStreamActor), on
    the grid placed by the spacing and origin the grid route reports (the
    desktop places it so, before and after per-axis factors). Written from
    the C++, not from the backend's module."""
    import vtk
    from vtk.util import numpy_support
    img = vtk.vtkImageData()
    img.SetDimensions(nx, ny, nz)
    img.SetSpacing(*[float(v) for v in spacing])
    img.SetOrigin(*[float(v) for v in origin])
    vec = np.frombuffer(raw, "<f4").reshape(-1, 3).copy()
    a = numpy_support.numpy_to_vtk(vec, deep=True)
    a.SetName("velocity")
    img.GetPointData().SetVectors(a)
    b = img.GetBounds()
    ext = max(b[1] - b[0], b[3] - b[2], b[5] - b[4])
    src = vtk.vtkPointSource()
    src.SetCenter(0.5 * (b[0] + b[1]), 0.5 * (b[2] + b[3]), 0.5 * (b[4] + b[5]))
    src.SetRadius(0.3 * ext)
    src.SetNumberOfPoints(seeds)
    rs = vtk.vtkMinimalStandardRandomSequence()
    rs.SetSeed(seed)
    src.SetRandomSequence(rs)
    t = vtk.vtkStreamTracer()
    t.SetInputData(img)
    t.SetSourceConnection(src.GetOutputPort())
    t.SetIntegratorTypeToRungeKutta45()
    t.SetIntegrationDirectionToBoth()
    t.SetMaximumPropagation(2.0 * ext)
    t.SetMaximumNumberOfSteps(2000)
    t.Update()
    return numpy_support.vtk_to_numpy(t.GetOutput().GetPoints().GetData())


@pytest.mark.parametrize("max_dim", [0, 10])
def test_streamlines_are_the_desktops_on_the_same_grid(tmp_path, max_dim):
    pytest.importorskip("vtk")
    from app.velocity_field import velocity_grid_worker
    from app.velocity_lines import decode, flow_lines_worker
    path = _flow(tmp_path)
    res = flow_lines_worker(path, 50.0, max_dim, 30, 1000, 4)
    grid = velocity_grid_worker(path, 50.0, max_dim)
    ref = _desktop_lines(grid["raw_bytes"], grid["nx"], grid["ny"], grid["nz"],
                         grid["spacing"], grid["origin"], 30, 4)
    pts, _, offsets, apos, _ = decode(res)
    assert res["lines"] > 0 and len(pts) == len(ref)
    assert np.allclose(np.sort(pts, axis=0), np.sort(ref, axis=0), atol=1e-4)
    assert offsets[-1] == len(pts) and len(apos) == res["arrows"]
    assert res["grid"]["spacing"] == [float(v) for v in grid["spacing"]]


@pytest.mark.asyncio
async def test_velocity_lines_route(client, auth_headers, tmp_path):
    pytest.importorskip("vtk")
    path = _flow(tmp_path)
    vid = (await client.post("/v1/velocity/open", json={"path": path, "box_mpc": 50.0},
                             headers=auth_headers)).json()["dataset_id"]
    r = await client.post("/v1/velocity/lines_bin", json={"dataset_id": vid, "n_seeds": 20,
                                                           "max_arrows": 500}, headers=auth_headers)
    assert r.status_code == 200
    h = r.headers
    n, lines, arrows = int(h["x-visivo-line-points"]), int(h["x-visivo-lines"]), int(h["x-visivo-arrows"])
    assert len(r.content) == n * 16 + (lines + 1) * 4 + arrows * 24
    assert [float(v) for v in h["x-visivo-origin"].split(",")] == [-25.0, -25.0, -25.0]
    r = await client.post("/v1/velocity/lines_bin", json={"dataset_id": "nope"}, headers=auth_headers)
    assert r.status_code == 404


# ── Review fixes ─────────────────────────────────────────────────────────────


def test_prefix_order_spreads_every_prefix():
    """Any prefix of m rows of n leaves no gap wider than 2 n / m."""
    for n in (1000, 4097, 10_000):
        o = ps.prefix_order(n)
        assert o.dtype == np.int32
        for m in (3, 10, 77, 500, 999):
            pts = np.sort(np.r_[o[:m], -1, n])
            assert np.diff(pts).max() <= 2 * n / m + 1, (n, m)


def test_filters_run_before_the_budget(tmp_path):
    from app.point_table import open_point_table
    head, data = _vbt(tmp_path)
    t = open_point_table(head)
    s = ps.sample_points(t, 1000, filters=[{"field": "Y", "op": ">", "value": 2.5}])
    assert len(s.positions) == 1000          # the budget, not a share of it
    lo, hi = np.array(s.bounds[0::2]), np.array(s.bounds[1::2])
    assert lo[1] > 2.5


def test_the_colour_scale_is_chosen_in_the_same_request(tmp_path):
    from app.point_table import open_point_table
    t = open_point_table(_vbt(tmp_path)[0])
    s = ps.sample_points(t, 500, colour_field="Mass", colour_log=None)
    assert s.colour_log == (math.log10(s.data_range[1] / s.data_range[0]) > ps.AUTO_LOG_DECADES)
    s = ps.sample_points(t, 500, colour_field="X", colour_log=None)
    assert s.colour_log is False                 # X reaches negative values


@pytest.mark.asyncio
async def test_malformed_filters_and_unreadable_tables_are_422(client, auth_headers, tmp_path):
    head, _ = _vbt(tmp_path, name="bad")
    did = (await client.post("/v1/vbt/open", json={"path": head}, headers=auth_headers)).json()["dataset_id"]
    for f in ([{"field": "X", "op": ">", "value": None}], [{"field": "Nope", "op": ">", "value": 1}],
              [{"field": "X", "op": "~", "value": 1}]):
        r = await client.post("/v1/points/sample_bin", json={"dataset_id": did, "filters": f},
                              headers=auth_headers)
        assert r.status_code == 422, f
    (tmp_path / "bad.bin").unlink()
    r = await client.post("/v1/points/info", json={"dataset_id": did}, headers=auth_headers)
    assert r.status_code == 422


def _vopt_cube(tmp_path, cunit="m/s", restfrq=1.420405752e9):
    h = fits.Header()
    h["NAXIS"] = 3
    h["CTYPE1"], h["CRVAL1"], h["CRPIX1"], h["CDELT1"], h["CUNIT1"] = "RA---TAN", 150.0, 20.5, -4 / 3600, "deg"
    h["CTYPE2"], h["CRVAL2"], h["CRPIX2"], h["CDELT2"], h["CUNIT2"] = "DEC--TAN", 20.0, 20.5, 4 / 3600, "deg"
    scale = 1000.0 if cunit == "m/s" else 1.0
    h["CTYPE3"], h["CRVAL3"], h["CRPIX3"], h["CDELT3"], h["CUNIT3"] = "VOPT", 2900 * scale, 1.0, 10 * scale, cunit
    h["SPECSYS"] = "BARYCENT"
    if restfrq:
        h["RESTFRQ"] = restfrq
    path = tmp_path / f"vopt_{cunit.replace('/', '')}.fits"
    fits.PrimaryHDU(np.zeros((30, 40, 40), np.float32), header=h).writeto(path, overwrite=True)
    return str(path)


def test_v_app_without_a_unit_is_in_the_axis_unit(tmp_path):
    import pandas as pd
    geo = cc.cube_geometry(_vopt_cube(tmp_path))
    # SoFiA's v_app, in the axis's m/s, no unit kept (a CSV).
    df = pd.DataFrame({"ra": [150.0], "dec": [20.0], "v_app": [3000e3]})
    out = cc.place_table(df, {"system": "sky", "ra": "ra", "dec": "dec"}, geo.cel, geo.wcs, geo.shape)
    assert out.spectral["used"] and out.sources[0]["z"] == pytest.approx(10.0)


def test_a_velocity_of_no_convention_is_the_axis_own_and_said(tmp_path):
    import pandas as pd
    geo = cc.cube_geometry(_vopt_cube(tmp_path))
    df = pd.DataFrame({"ra": [150.0], "dec": [20.0], "velocity": [3000.0]})
    out = cc.place_table(df, {"system": "sky", "ra": "ra", "dec": "dec"}, geo.cel, geo.wcs, geo.shape)
    assert out.sources[0]["z"] == pytest.approx(10.0)        # optical, as the axis
    assert any("no velocity convention" in w for w in out.warnings)
    # Asked as radio: converted through the rest frequency, another channel.
    out = cc.place_table(df, {"system": "sky", "ra": "ra", "dec": "dec"}, geo.cel, geo.wcs,
                         geo.shape, spectral_convention="radio")
    assert abs(out.sources[0]["z"] - 10.0) > 1


def test_a_unit_in_the_column_name_and_in_an_ipac_header(tmp_path):
    import pandas as pd
    from app.dependencies import _catalogue_load_dataframe
    geo = cc.cube_geometry(_vopt_cube(tmp_path))
    df = pd.DataFrame({"ra": [150.0], "dec": [20.0], "v_opt (m/s)": [3000e3]})
    out = cc.place_table(df, {"system": "sky", "ra": "ra", "dec": "dec"}, geo.cel, geo.wcs, geo.shape)
    assert out.spectral["field"] == "v_opt (m/s)" and out.sources[0]["z"] == pytest.approx(10.0)
    tbl = tmp_path / "c.tbl"
    tbl.write_text("|ra      |dec     |v_opt   |\n|double  |double  |double  |\n"
                   "|deg     |deg     |m/s     |\n 150.0    20.0     3000000.0\n")
    frame = _catalogue_load_dataframe(tbl, "ipac")
    assert frame.attrs["units"]["v_opt"] == "m/s"
    out = cc.place_table(frame, {"system": "sky", "ra": "ra", "dec": "dec"}, geo.cel, geo.wcs, geo.shape)
    assert out.sources[0]["z"] == pytest.approx(10.0)


def test_a_misread_spectral_column_is_read_again_and_said(tmp_path):
    import pandas as pd
    geo = cc.cube_geometry(_vopt_cube(tmp_path))
    # m/s values read as km/s: every source a thousand times too far; read
    # a thousand times smaller, both land on their channels, and it is said.
    df = pd.DataFrame({"ra": [150.0, 150.001], "dec": [20.0, 20.001], "v_opt": [3000e3, 3010e3]})
    out = cc.place_table(df, {"system": "sky", "ra": "ra", "dec": "dec"}, geo.cel, geo.wcs, geo.shape)
    assert [round(s["z"], 6) for s in out.sources] == [10.0, 11.0]
    assert out.spectral["used"] and any("read as v_opt / 1000" in w for w in out.warnings)


def test_a_column_no_reading_places_goes_on_the_sky_only(tmp_path):
    import pandas as pd
    geo = cc.cube_geometry(_vopt_cube(tmp_path))
    df = pd.DataFrame({"ra": [150.0, 150.001], "dec": [20.0, 20.001], "v_opt": [-9e9, -8e9]})
    out = cc.place_table(df, {"system": "sky", "ra": "ra", "dec": "dec"}, geo.cel, geo.wcs, geo.shape)
    assert len(out.sources) == 2 and all(s["z"] is None for s in out.sources)
    assert not out.spectral["used"] and any("placed on the sky only" in w for w in out.warnings)


def test_a_partial_spectral_overlap_is_kept(tmp_path):
    """A wide-band survey over a narrow-band cube: 3 of 10 sources in the
    channels keep them; the 7 others are outside, and said."""
    import pandas as pd
    geo = cc.cube_geometry(_vopt_cube(tmp_path))      # 2900..3190 km/s, 30 channels
    v = [3000.0, 3050.0, 3100.0, 500.0, 1000.0, 1500.0, 2000.0, 5000.0, 6000.0, 8000.0]
    df = pd.DataFrame({"ra": [150.0] * 10, "dec": [20.0] * 10, "v_opt": v})
    out = cc.place_table(df, {"system": "sky", "ra": "ra", "dec": "dec"}, geo.cel, geo.wcs, geo.shape)
    assert out.spectral["used"]
    assert sorted(round(s["z"], 6) for s in out.sources) == [10.0, 15.0, 20.0]
    assert out.n_outside == 7 and any("7 of 10 sources lie outside" in w for w in out.warnings)


@pytest.mark.asyncio
async def test_an_ipac_catalogue_is_placed_in_its_own_unit(client, auth_headers, tmp_path):
    path = _vopt_cube(tmp_path)
    tbl = tmp_path / "survey.tbl"
    tbl.write_text("\\ a survey\n|name |ra      |dec     |v_opt   |\n|char |double  |double  |double  |\n"
                   "|     |deg     |deg     |m/s     |\n a     150.0    20.0     3000000.0\n")
    cube = (await client.post("/v1/datasets/open", json={"path": path}, headers=auth_headers)).json()
    cat = (await client.post("/v1/catalogue/open", json={"path": str(tbl), "format": "ipac"},
                             headers=auth_headers)).json()
    assert cat["valid"], cat
    j = (await client.post("/v1/cube/catalogue_pixels", json={"dataset_id": cube["dataset_id"],
         "catalogue_id": cat["dataset_id"]}, headers=auth_headers)).json()
    assert j["spectral"]["used"] and "m / s" in j["spectral"]["note"]
    assert j["sources"][0]["z"] == pytest.approx(10.0)
    assert not any("read as" in w for w in j["warnings"])


@pytest.mark.asyncio
async def test_a_catalogues_point_table_goes_with_its_dataset(client, auth_headers, tmp_path):
    import gc
    import weakref
    from app.routers import vr_layers
    csv = tmp_path / "pts.csv"
    csv.write_text("x,y,z,m\n" + "\n".join(f"{i},{i % 7},{i % 5},{i}" for i in range(200)) + "\n")
    cat = (await client.post("/v1/catalogue/open", json={"path": str(csv), "format": "csv"},
                             headers=auth_headers)).json()
    did = cat["dataset_id"]
    r = await client.post("/v1/points/sample_bin", json={"dataset_id": did, "max_points": 100},
                          headers=auth_headers)
    assert r.status_code == 200
    from app.sessions import REGISTRY
    entry = next(s.datasets[did] for s in list(REGISTRY._sessions.values()) if did in s.datasets)
    spec, table = entry["_point_table"]
    assert table.frame is None                       # the frame is the entry's alone
    ref = weakref.ref(table)
    del table, entry
    r = await client.delete(f"/v1/datasets/{did}", headers=auth_headers)
    assert r.json()["closed"]
    gc.collect()
    assert ref() is None                              # gone with its dataset
    assert not hasattr(vr_layers, "_TABLES")          # no cache outside the entries


def test_sample_peak_memory_per_row(tmp_path):
    """Working memory of a sample with a log colour column, measured: about
    40 bytes a row (0.4 GB at 10 M rows; it was 88 with float64 temporaries)."""
    import tracemalloc
    from app.point_table import open_point_table
    n = 1_000_000
    rng = np.random.default_rng(1)
    data = np.stack([rng.normal(size=n), rng.normal(size=n), rng.normal(size=n),
                     np.exp(rng.normal(size=n))]).astype("<f4")
    data.tofile(tmp_path / "big.bin")
    (tmp_path / "big.bin.head").write_text(f"float\n4\n{n}\nlittle\nX\nY\nZ\nMass\n")
    t = open_point_table(str(tmp_path / "big.bin.head"))
    tracemalloc.start()
    s = ps.sample_points(t, n, colour_field="Mass", colour_log=True)
    body = s.encode()
    peak = tracemalloc.get_traced_memory()[1]
    tracemalloc.stop()
    assert len(body) == 8 * n
    assert peak / n < 50, peak / n


def test_spectral_frames_are_checked(tmp_path):
    geo = cc.cube_geometry(_vopt_cube(tmp_path))
    assert cc.frame_warnings(geo.wcs, "v_opt", None) == []
    assert any("LSRK" in w for w in cc.frame_warnings(geo.wcs, "v_opt", "LSRK"))
    assert any("LSR" in w for w in cc.frame_warnings(geo.wcs, "vlsr", None))
    path, _ = _car_cube(tmp_path)          # no SPECSYS: nothing to say
    assert cc.frame_warnings(cc.cube_geometry(path).wcs, "vlsr", None) == []


def test_the_vlkb_box_across_l_0_and_with_the_desktops_margin(tmp_path):
    h = fits.Header()
    h["NAXIS"] = 2
    h["CTYPE1"], h["CRVAL1"], h["CRPIX1"], h["CDELT1"], h["CUNIT1"] = "GLON-CAR", 0.0, 50.5, -0.01, "deg"
    h["CTYPE2"], h["CRVAL2"], h["CRPIX2"], h["CDELT2"], h["CUNIT2"] = "GLAT-CAR", 0.0, 30.5, 0.01, "deg"
    path = tmp_path / "gc.fits"
    fits.PrimaryHDU(np.zeros((60, 100), np.float32), header=h).writeto(path)
    geo = cc.cube_geometry(str(path))
    box = cc.galactic_footprint(geo.cel, *geo.shape[:2])
    # The image spans l = -0.5..+0.5, b = -0.3..+0.3 (pixel edges); the
    # desktop's margin is 10 % plus 0.01 degree.
    assert box[0] == pytest.approx(-0.5 - 0.11, abs=1e-6) and box[1] == pytest.approx(0.5 + 0.11, abs=1e-6)
    assert box[2] == pytest.approx(-0.3 - 0.07, abs=1e-6) and box[3] == pytest.approx(0.3 + 0.07, abs=1e-6)
    where = cc.vlkb_where("glon", "glat", box)
    assert where.startswith("(glon BETWEEN 359.390000 AND 360 OR glon BETWEEN 0 AND 0.610000)")
    from app.routers.vr_layers import _vlkb_query
    assert "(glonft BETWEEN 359.390000 AND 360 OR glonft BETWEEN 0 AND 0.610000)" in \
        _vlkb_query("vlkb_sources", box)


@pytest.mark.asyncio
async def test_lines_take_the_desktops_box(client, auth_headers, tmp_path):
    pytest.importorskip("vtk")
    path = _flow(tmp_path)
    vid = (await client.post("/v1/velocity/open", json={"path": path, "box_mpc": 50.0},
                             headers=auth_headers)).json()["dataset_id"]
    for box, origin, label in ((None, -25.0, 50.0), (0, -11.5, 0.0), (100.0, -50.0, 100.0)):
        body = {"dataset_id": vid, "n_seeds": 5, "max_arrows": 100}
        if box is not None:
            body["box_mpc"] = box
        r = await client.post("/v1/velocity/lines_bin", json=body, headers=auth_headers)
        assert r.status_code == 200
        assert float(r.headers["x-visivo-origin"].split(",")[0]) == pytest.approx(origin)
        assert float(r.headers["x-visivo-box-mpc"]) == label
    r = await client.post("/v1/velocity/lines_bin", json={"dataset_id": vid, "box_mpc": -1},
                          headers=auth_headers)
    assert r.status_code == 422


def test_xr_errors_carry_a_code():
    from app.render_session._stereo import XRFrame
    import json
    m = json.loads(XRFrame({"pose_id": 3}, error="x", code="out_of_core").error_message())
    assert m["code"] == "out_of_core"
    assert "code" not in json.loads(XRFrame({"pose_id": 3}, error="x").error_message())


def test_a_pose_that_cannot_be_drawn_is_an_error_frame(monkeypatch):
    from app.render_session._stereo_points import PointStereoMixin

    class Poly:
        def GetNumberOfPoints(self):
            return 10

    class Fake(PointStereoMixin):
        _polydata = Poly()

        def _render_point_eyes_on_thread(self, pose):
            raise ValueError("degenerate placement")

    f = Fake()

    class Pose:
        pose_id, t_client = 9, None
    f._xr_pose = Pose()
    frame = f._render_xr_frame_on_thread()
    assert frame.error and "degenerate" in frame.error and frame.header["pose_id"] == 9


# ── Final review ─────────────────────────────────────────────────────────────


def _narrow_vopt_cube(tmp_path):
    """The reviewer's case (r190/t7.py): a narrow VOPT band, 27000-27980 km/s."""
    h = fits.Header()
    h["NAXIS"] = 3
    for k, v in dict(CTYPE1="RA---SIN", CTYPE2="DEC--SIN", CRVAL1=10.0, CRVAL2=-30.0,
                     CDELT1=-0.001, CDELT2=0.001, CRPIX1=20.0, CRPIX2=20.0, CUNIT1="deg",
                     CUNIT2="deg", CTYPE3="VOPT", CUNIT3="km/s", CRVAL3=27000.0, CDELT3=20.0,
                     CRPIX3=1.0, SPECSYS="BARYCENT", RESTFRQ=1.420405752e9).items():
        h[k] = v
    p = tmp_path / "c7.fits"
    fits.PrimaryHDU(np.zeros((50, 40, 40), np.float32), header=h).writeto(p, overwrite=True)
    return str(p)


@pytest.mark.parametrize("convention", [None, "optical"])
def test_a_stated_convention_is_never_read_again(tmp_path, convention):
    """Three galaxies rightly below the band (v_opt 24950-25100 km/s) are not
    re-read as radio to land inside the cube: on the sky only, and said."""
    import pandas as pd
    geo = cc.cube_geometry(_narrow_vopt_cube(tmp_path))
    df = pd.DataFrame({"ra": [10.0] * 3, "dec": [-30.0, -29.999, -29.998],
                       "v_opt": [24950.0, 25000.0, 25100.0], "name": ["a", "b", "c"]})
    out = cc.place_table(df, {"system": "sky", "ra": "ra", "dec": "dec"}, geo.cel, geo.wcs,
                         geo.shape, "name", spectral_convention=convention)
    assert [s["z"] for s in out.sources] == [None, None, None]
    assert not out.spectral["used"]
    assert any("placed on the sky only" in w for w in out.warnings)
    assert not any("radio" in w for w in out.warnings)


def test_a_declared_unit_is_never_read_again(tmp_path):
    import pandas as pd
    geo = cc.cube_geometry(_vopt_cube(tmp_path))
    # m/s values in a column that says km/s: believed, so off the band.
    df = pd.DataFrame({"ra": [150.0, 150.001], "dec": [20.0, 20.001],
                       "v_opt (km/s)": [3000e3, 3010e3]})
    out = cc.place_table(df, {"system": "sky", "ra": "ra", "dec": "dec"}, geo.cel, geo.wcs, geo.shape)
    assert all(s["z"] is None for s in out.sources) and not out.spectral["used"]
    assert not any("read as" in w for w in out.warnings)


def test_samples_wait_in_the_coroutine_and_tables_build_per_dataset():
    from app.routers import vr_layers

    async def slots():
        return vr_layers._sample_slots(), vr_layers._sample_slots()
    a, b = asyncio.new_event_loop().run_until_complete(slots())
    assert a is b and isinstance(a, asyncio.Semaphore) and a._value == vr_layers.SAMPLE_CONCURRENCY
    e1, e2 = {}, {}
    assert vr_layers._build_lock(e1) is vr_layers._build_lock(e1)
    assert vr_layers._build_lock(e1) is not vr_layers._build_lock(e2)
    assert not hasattr(vr_layers, "_TABLES_LOCK")


# ── PR #198 review ───────────────────────────────────────────────────────────


def test_points_far_from_zero_keep_their_separations(tmp_path):
    """X = 1e9, 1e9 + 1, 1e9 + 2: float32 made them one point (codes 0, 0, 0)."""
    from app.point_table import open_point_table
    x = np.array([1e9, 1e9 + 1, 1e9 + 2], dtype="<f8")
    data = np.stack([x, [0.0, 1.0, 2.0], [5.0, 6.0, 7.0]]).astype("<f8")
    data.tofile(tmp_path / "far.bin")
    (tmp_path / "far.bin.head").write_text("double\n3\n3\nlittle\nX\nY\nZ\n")
    s = ps.sample_points(open_point_table(str(tmp_path / "far.bin.head")), 10)
    assert s.bounds[:2] == [1e9, 1e9 + 2]
    order = ps.prefix_order(3)
    codes = np.empty(3, dtype=np.uint16)
    codes[order] = s.positions[:, 0]              # back to file order
    assert codes.tolist() == [0, 32768, 65535]


def test_points_with_a_non_finite_axis_are_left_out_of_the_bounds(tmp_path):
    from app.point_table import open_point_table
    data = np.array([[0.0, 1.0, 2.0, 1e6], [0.0, 1.0, 2.0, np.nan], [0.0, 0.0, 0.0, 0.0]], dtype="<f4")
    data.tofile(tmp_path / "nan.bin")
    (tmp_path / "nan.bin.head").write_text("float\n3\n4\nlittle\nX\nY\nZ\n")
    s = ps.sample_points(open_point_table(str(tmp_path / "nan.bin.head")), 10)
    assert len(s.positions) == 3 and s.bounds[:2] == [0.0, 2.0]


@pytest.mark.parametrize("unit, scale", [("hourangle", 15.0), ("h", 15.0), ("rad", 180 / math.pi)])
def test_sky_columns_are_read_in_their_unit(tmp_path, unit, scale):
    """RA in hours: 1 h is 15 degrees, not 1 (it landed 15 times too near on a CAR cube)."""
    import pandas as pd
    h = fits.Header()
    h["NAXIS"] = 2
    h["CTYPE1"], h["CRVAL1"], h["CRPIX1"], h["CDELT1"], h["CUNIT1"] = "RA---CAR", 0.0, 1.0, -0.1, "deg"
    h["CTYPE2"], h["CRVAL2"], h["CRPIX2"], h["CDELT2"], h["CUNIT2"] = "DEC--CAR", 0.0, 1.0, 0.1, "deg"
    path = tmp_path / "car.fits"
    fits.PrimaryHDU(np.zeros((40, 400), np.float32), header=h).writeto(path)
    geo = cc.cube_geometry(str(path))
    ra_deg = 345.0                                  # x = (360 - 345) / 0.1 = 150
    df = pd.DataFrame({"ra": [ra_deg / scale], "dec": [0.5 / (scale if unit == "rad" else 1.0)]})
    df.attrs["units"] = {"ra": unit, "dec": "rad" if unit == "rad" else "deg"}
    out = cc.place_table(df, {"system": "sky", "ra": "ra", "dec": "dec"}, geo.cel, geo.wcs, geo.shape)
    assert out.sources[0]["x"] == pytest.approx(150.0, abs=1e-6)
    assert out.sources[0]["y"] == pytest.approx(5.0, abs=1e-6)
    assert out.sources[0]["lon"] == pytest.approx(ra_deg)


def test_a_sky_column_in_no_angle_unit_is_refused(tmp_path):
    import pandas as pd
    path, _ = _car_cube(tmp_path)
    geo = cc.cube_geometry(path)
    df = pd.DataFrame({"glon": [30.0], "glat": [0.5]})
    df.attrs["units"] = {"glon": "km/s"}
    with pytest.raises(ValueError, match="not an angle"):
        cc.place_table(df, {"system": "galactic", "glon": "glon", "glat": "glat"}, geo.cel,
                       geo.wcs, geo.shape)
    df.attrs["units"] = {"glon": "furlongs"}
    with pytest.raises(ValueError, match="not an angle unit"):
        cc.place_table(df, {"system": "galactic", "glon": "glon", "glat": "glat"}, geo.cel,
                       geo.wcs, geo.shape)
    # A bracket in a name that is no unit leaves the column in degrees.
    df = pd.DataFrame({"glon (J2000)": [30.0], "glat": [0.5]})
    out = cc.place_table(df, {"system": "galactic", "glon": "glon (J2000)", "glat": "glat"},
                         geo.cel, geo.wcs, geo.shape)
    assert out.sources and out.sources[0]["lon"] == 30.0


@pytest.mark.parametrize("unit, value", [("DEG", 345.0), ("Deg", 345.0), ("RAD", math.radians(345.0)),
                                         ("HOURS", 23.0), ("H", 23.0), ("ARCSEC", 345.0 * 3600)])
def test_fits_unit_spellings_are_read_in_any_case(unit, value):
    assert cc.angle_degrees(np.array([value]), "ra", unit)[0] == pytest.approx(345.0)


@pytest.mark.asyncio
async def test_a_fits_table_in_capital_deg_is_placed(client, auth_headers, tmp_path):
    """TUNIT = 'DEG' (FITS spelling) was refused with 422."""
    from astropy.table import Table
    path, w = _car_cube(tmp_path)
    lon, lat = w.celestial.all_pix2world([[12.0, 13.0]], 0)[0]
    from astropy.coordinates import SkyCoord
    icrs = SkyCoord(lon, lat, unit="deg", frame="galactic").icrs
    t = Table({"RA": [icrs.ra.deg], "DEC": [icrs.dec.deg]})
    fp = tmp_path / "cat.fits"
    t.write(fp)
    with fits.open(fp, mode="update") as hdul:
        hdul[1].header["TUNIT1"] = "DEG"
        hdul[1].header["TUNIT2"] = "DEG"
    cube = (await client.post("/v1/datasets/open", json={"path": path}, headers=auth_headers)).json()
    cat = (await client.post("/v1/catalogue/open", json={"path": str(fp), "format": "fits"},
                             headers=auth_headers)).json()
    assert cat["valid"], cat
    r = await client.post("/v1/cube/catalogue_pixels", json={"dataset_id": cube["dataset_id"],
                          "catalogue_id": cat["dataset_id"]}, headers=auth_headers)
    assert r.status_code == 200, r.text
    s = r.json()["sources"][0]
    assert s["x"] == pytest.approx(12.0, abs=1e-6) and s["y"] == pytest.approx(13.0, abs=1e-6)


def test_a_point_tables_sky_positions_take_the_units_too():
    """The same catalogue drawn as points: RA in hours and in degrees agree."""
    import pandas as pd
    from app.catalogue_positions import sky_positions
    deg = pd.DataFrame({"ra": [345.0, 10.0], "dec": [-20.0, 30.0], "z": [0.01, 0.02]})
    hours = pd.DataFrame({"ra": [23.0, 10.0 / 15.0], "dec": [-20.0, 30.0], "z": [0.01, 0.02]})
    hours.attrs["units"] = {"ra": "h", "dec": "deg"}
    a, b = sky_positions(deg, "ra", "dec"), sky_positions(hours, "ra", "dec")
    assert np.allclose(a, b, equal_nan=True)
    assert np.isfinite(a).any()


def test_points_far_from_zero_keep_their_separations_when_the_first_row_is_dropped(tmp_path):
    """The first row (1e9 away from the rest) has no Y: the offsets of the
    rows kept are not taken from it."""
    from app.point_table import open_point_table
    data = np.array([[1e9, 0.0, 1.0, 2.0], [np.nan, 0.0, 1.0, 2.0], [0.0, 0.0, 0.0, 0.0]],
                    dtype="<f8")                     # X, Y, Z of 4 rows
    data.tofile(tmp_path / "drop.bin")
    (tmp_path / "drop.bin.head").write_text("double\n3\n4\nlittle\nX\nY\nZ\n")
    s = ps.sample_points(open_point_table(str(tmp_path / "drop.bin.head")), 10)
    assert len(s.positions) == 3 and s.bounds[:2] == [0.0, 2.0]
    order = ps.prefix_order(3)
    codes = np.empty(3, dtype=np.uint16)
    codes[order] = s.positions[:, 0]
    assert codes.tolist() == [0, 32768, 65535]


def test_an_ipac_catalogue_in_hours_is_drawn_as_points_where_degrees_put_it(tmp_path):
    """A point table of an IPAC catalogue whose RA header unit is 'h' lands
    where the same catalogue in degrees does."""
    import json as _json
    from app.point_table import open_point_table
    (tmp_path / "h.tbl").write_text(
        "|   ra    |   dec   |   z    |\n"
        "| double  | double  | double |\n"
        "|   h     |   deg   |        |\n"
        "  23.0      -20.0     0.01\n"
        "  0.5        30.0     0.02\n")
    (tmp_path / "d.csv").write_text("ra,dec,z\n345.0,-20.0,0.01\n7.5,30.0,0.02\n")
    coords = {"system": "sky", "ra": "ra", "dec": "dec"}
    h = open_point_table(_json.dumps({"paths": [str(tmp_path / "h.tbl")], "format": "ipac",
                                      "coordinates": coords}))
    d = open_point_table(_json.dumps({"paths": [str(tmp_path / "d.csv")], "format": "csv",
                                      "coordinates": coords}))
    a = np.stack([h.columns[k] for k in ("@x", "@y", "@z")], axis=1)
    b = np.stack([d.columns[k] for k in ("@x", "@y", "@z")], axis=1)
    assert np.isfinite(a).all()
    np.testing.assert_allclose(a, b, rtol=1e-9)


@pytest.mark.parametrize("x, y, kept, lo, hi", [
    ([-3e38, 3e38, 0.0], [0.0, 1.0, 2.0], [-3e38, 3e38, 0.0], -3e38, 3e38),  # offsets past float32
    ([1e39, 0.0, 1.0, 2.0], [np.nan, 0.0, 1.0, 2.0], [0.0, 1.0, 2.0], 0.0, 2.0),
    ([1e39, -1e39, 0.0, 4.0], [0.0, 1.0, 2.0, 3.0], [0.0, 4.0], 0.0, 4.0),  # values past float32
    ([-1.7e308, 1.7e308, 0.0, 4.0], [0.0, 1.0, 2.0, 3.0], [0.0, 4.0], 0.0, 4.0),
    ([1e9, 1e9 + 1, 1e9 + 2], [0.0, 1.0, 2.0], [1e9, 1e9 + 1, 1e9 + 2], 1e9, 1e9 + 2),
])
def test_positions_are_judged_on_their_float64_value(x, y, kept, lo, hi):
    """A position is kept when its float64 value is finite and within float32's
    range (3.4e38): beyond it nothing can draw it (the page and the remote
    point session work in float32), so it is dropped as a NaN is. Within it,
    an offset that overflows float32 falls back to fractions of the range."""
    from app.point_table import PointTable
    cols = {"X": np.array(x, dtype=np.float64), "Y": np.array(y, dtype=np.float64),
            "Z": np.zeros(len(x))}
    s = ps.sample_points(PointTable(list(cols), cols, len(x), ("X", "Y", "Z")), 10)
    n = len(kept)
    assert len(s.positions) == n
    assert s.bounds[:2] == [lo, hi]
    codes = np.empty(n, dtype=np.uint16)
    codes[ps.prefix_order(n)] = s.positions[:, 0]
    m = max(abs(lo), abs(hi))
    expected = [round((v / m - lo / m) / (hi / m - lo / m) * 65535) for v in kept]
    assert codes.tolist() == expected


def test_a_sorted_table_with_a_dropped_first_row_is_read_once(monkeypatch):
    """X = Y = 0..9 with Z[0] NaN: row 0 is dropped. The reference of each axis
    is the first row finite on the axes read so far, and a reference just
    outside the rows kept is no reason to read a column again."""
    from app.point_table import PointTable
    calls = []
    real = ps.read_normalised
    monkeypatch.setattr(ps, "read_normalised", lambda *a: calls.append(1) or real(*a))
    z = np.arange(10, dtype=np.float64)
    z[0] = np.nan
    cols = {"X": np.arange(10.0), "Y": np.arange(10.0), "Z": z}
    s = ps.sample_points(PointTable(list(cols), cols, 10, ("X", "Y", "Z")), 100)
    assert calls == []
    assert len(s.positions) == 9 and s.bounds == [1.0, 9.0, 1.0, 9.0, 1.0, 9.0]
    # Still read again when the reference is far from the rows kept.
    x = np.arange(10.0)
    x[0] = 1e9
    cols = {"X": x, "Y": np.arange(10.0), "Z": z}
    s = ps.sample_points(PointTable(list(cols), cols, 10, ("X", "Y", "Z")), 100)
    assert len(s.positions) == 9 and s.bounds[:2] == [1.0, 9.0]


def test_a_tiny_span_keeps_its_resolution():
    """Offsets of a span under 1e-30 would be subnormal float32: the axis is
    read as fractions of its range instead."""
    from app.point_table import PointTable
    x = np.array([0.0, 1e-40, 2e-40])
    cols = {"X": x, "Y": np.arange(3.0), "Z": np.zeros(3)}
    s = ps.sample_points(PointTable(list(cols), cols, 3, ("X", "Y", "Z")), 10)
    assert s.bounds[:2] == [0.0, 2e-40]
    codes = np.empty(3, dtype=np.uint16)
    codes[ps.prefix_order(3)] = s.positions[:, 0]
    assert codes.tolist() == [0, 32768, 65535]


def test_a_colour_column_far_from_zero_keeps_its_colours():
    """The colour was read in float32: 1e9, 1e9 + 1, 1e9 + 2 were one colour
    (an empty data range)."""
    from app.point_table import PointTable
    c = np.array([1e9, 1e9 + 1, 1e9 + 2])
    cols = {"X": np.arange(3.0), "Y": np.arange(3.0), "Z": np.arange(3.0), "C": c}
    s = ps.sample_points(PointTable(list(cols), cols, 3, ("X", "Y", "Z")), 10, colour_field="C")
    assert s.data_range == (1e9, 1e9 + 2)
    codes = np.empty(3, dtype=np.uint16)
    codes[ps.prefix_order(3)] = s.colour
    assert codes.tolist() == [0, 32768, 65535]
    # On a log scale too (forced: the auto rule keeps a range this narrow linear).
    s = ps.sample_points(PointTable(list(cols), cols, 3, ("X", "Y", "Z")), 10,
                         colour_field="C", colour_log=True)
    codes[ps.prefix_order(3)] = s.colour
    assert codes.tolist()[0] == 0 and codes.tolist()[2] == 65535 and 0 < codes.tolist()[1] < 65535


def test_a_colour_range_wider_than_the_largest_double_keeps_its_codes():
    """-1.7e308 .. 1.7e308: cmax - cmin is infinite in float64, and every code
    came out 0. The values are scaled by the largest magnitude first."""
    from app.point_table import PointTable
    c = np.array([-1.7e308, 0.0, 1.7e308])
    cols = {"X": np.arange(3.0), "Y": np.arange(3.0), "Z": np.arange(3.0), "C": c}
    s = ps.sample_points(PointTable(list(cols), cols, 3, ("X", "Y", "Z")), 10, colour_field="C")
    assert s.data_range == (-1.7e308, 1.7e308)
    codes = np.empty(3, dtype=np.uint16)
    codes[ps.prefix_order(3)] = s.colour
    assert codes.tolist() == [0, 32768, 65535]
