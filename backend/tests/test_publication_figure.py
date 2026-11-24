"""The publication figure's options, overlays, formats and preview (issue #41).

The route is the one UC 1.3 had; these tests ask for each thing the dialog can
ask for and check it is on the picture, by comparing it with the same figure
without it (pixels) or by reading the file (PDF, SVG, EPS).
"""
import base64
import io
import time
from pathlib import Path

import numpy as np
import pytest
import pytest_asyncio
from astropy.io import fits

ROUTE = "/v1/products/publication_figure"
CRVAL = (150.0, 2.0)


@pytest.fixture(scope="module")
def beam_image(tmp_path_factory) -> Path:
    """200 x 160 image: a Gaussian blob on noise, SIN sky grid of 1 arcsec pixels,
    a 10 x 5 arcsec beam at 30 degrees, BUNIT Jy/beam."""
    rng = np.random.default_rng(7)
    yy, xx = np.mgrid[0:160, 0:200]
    data = 0.01 * rng.standard_normal((160, 200)) + np.exp(-((xx - 100) ** 2 + (yy - 80) ** 2) / 800.0)
    hdu = fits.PrimaryHDU(data.astype(np.float32))
    h = hdu.header
    h.update({"CTYPE1": "RA---SIN", "CTYPE2": "DEC--SIN", "CRVAL1": CRVAL[0], "CRVAL2": CRVAL[1],
              "CRPIX1": 100.5, "CRPIX2": 80.5, "CDELT1": -1 / 3600, "CDELT2": 1 / 3600,
              "CUNIT1": "deg", "CUNIT2": "deg", "BUNIT": "Jy/beam",
              "BMAJ": 10 / 3600, "BMIN": 5 / 3600, "BPA": 30.0})
    p = tmp_path_factory.mktemp("fig") / "blob.fits"
    hdu.writeto(p)
    return p


@pytest.fixture
async def ds(client, opened_image, beam_image):
    h = opened_image["headers"]
    r = await client.post("/v1/datasets/open", json={"path": str(beam_image)}, headers=h)
    assert r.json()["valid"], r.json()
    return {"id": r.json()["dataset_id"], "headers": h}


async def _fig(client, ds, **opts):
    r = await client.post(ROUTE, headers=ds["headers"], json={"dataset_id": ds["id"], **opts})
    assert r.status_code == 200, r.text
    return r.json()


def _pixels(body) -> np.ndarray:
    import matplotlib.image as mpimg  # noqa: PLC0415
    return mpimg.imread(io.BytesIO(base64.b64decode(body["png_base64"])), format="png")[..., :3]


async def _preview(client, ds, **opts):
    """A preview of exactly the requested size (no cropping) so two can be compared."""
    body = await _fig(client, ds, preview=True, crop=False, **opts)
    assert body["valid"], body["error"]
    return body, _pixels(body)


def _changed(a: np.ndarray, b: np.ndarray) -> np.ndarray:
    return np.abs(a - b).max(axis=2) > 0.05


@pytest.mark.asyncio
@pytest.mark.parametrize("opts", [
    {},
    {"stretch": "log", "title": "Blob"},
    {"stretch": "asinh", "cmap": "Inferno"},
    {"stretch": "power", "gamma": 2.0, "cmap": "CoolWarm"},
    {"stretch": "squared", "cmap": "Purples_r"},
    {"stretch": "sinh"},
    {"vmin": -0.02, "vmax": 0.5},
    {"title": "T", "xlabel": "East", "ylabel": "North", "colorbar_label": "S (Jy/beam)"},
    {"font_family": "serif", "title": "T", "title_size": 20, "label_size": 14, "tick_size": 8,
     "colorbar_label_size": 16},
    {"font_family": "monospace"},
    {"width_in": 4.0, "height_in": 4.0, "dpi": 100},
    {"add_grid": False, "add_colorbar": False},
    {"frame": "galactic"},
    {"frame": "icrs", "tick_format": "degrees"},
    {"tick_format": "sexagesimal"},
    {"colorbar_position": "bottom"},
    {"colorbar_position": "top"},
    {"colorbar_position": "left"},
    {"overlay_color": "yellow", "beam": True, "scale_bar": True, "compass": True},
    {"crop": False},
])
async def test_every_option_renders(client, ds, opts):
    body = await _fig(client, ds, **opts)
    assert body["valid"], body["error"]
    assert body["format"] == "png"
    assert Path(body["output_path"]).stat().st_size > 2000
    assert body["has_wcs"] is True


@pytest.mark.asyncio
@pytest.mark.parametrize("ext, magic", [("pdf", b"%PDF"), ("svg", b"<?xml"), ("eps", b"%!PS-Adobe"),
                                        ("png", b"\x89PNG")])
async def test_formats_are_valid_files(client, ds, ext, magic):
    body = await _fig(client, ds, output_basename=f"fig41.{ext}", beam=True, title="x")
    assert body["valid"], body["error"]
    assert body["format"] == ext
    data = Path(body["output_path"]).read_bytes()
    assert data.startswith(magic)
    if ext == "svg":
        assert b"<svg" in data[:400]
    if ext == "pdf":
        assert data.rstrip().endswith(b"%%EOF")


@pytest.mark.asyncio
async def test_unknown_extension_is_png(client, ds):
    body = await _fig(client, ds, output_basename="fig41.tiff")
    assert body["output_path"].endswith(".png")


@pytest.mark.asyncio
async def test_size_and_dpi_set_the_picture(client, ds):
    body, px = await _preview(client, ds, width_in=4.0, height_in=3.0, dpi=100, preview_px=400)
    # 4 x 3 in at preview_px=400 over 4 in is 100 dpi: exactly 400 x 300 uncropped.
    assert (body["png_width"], body["png_height"]) == (400, 300)
    assert px.shape[:2] == (300, 400)


@pytest.mark.asyncio
async def test_beam_scale_bar_and_compass_are_drawn_where_asked(client, ds):
    _, plain = await _preview(client, ds)
    h, w = plain.shape[:2]
    cases = {"beam": ("beam_position", "bottom-left"), "scale_bar": ("scale_bar_position", "bottom-right"),
             "compass": ("compass_position", "top-right")}
    for name, (pos_key, corner) in cases.items():
        _, with_it = await _preview(client, ds, **{name: True, pos_key: corner})
        mask = _changed(plain, with_it)
        assert mask.sum() > 30, f"{name} left no mark"
        ys, xs = np.nonzero(mask)
        # In the half of the figure the corner names: the marks are in that quadrant.
        assert (xs.mean() < w / 2) == corner.endswith("left"), name
        assert (ys.mean() > h / 2) == corner.startswith("bottom"), name


@pytest.mark.asyncio
async def test_beam_is_the_size_of_the_header_beam(client, ds):
    """10 x 5 arcsec on 1 arcsec pixels: the ellipse is about twice as long as wide."""
    _, plain = await _preview(client, ds, width_in=8, height_in=6.4, add_colorbar=False,
                              add_grid=False, preview_px=800)
    _, with_beam = await _preview(client, ds, width_in=8, height_in=6.4, add_colorbar=False,
                                  add_grid=False, preview_px=800, beam=True, overlay_color="black")
    ys, xs = np.nonzero(_changed(plain, with_beam))
    wide, tall = np.ptp(xs) + 1, np.ptp(ys) + 1
    # Position angle 30 degrees, north up and east left: the long axis points up and to the
    # left, so the box of the 10 x 5 arcsec ellipse is 6.6 x 9.0 pixels, taller than wide.
    assert 1.15 < tall / wide < 1.6, (wide, tall)
    # 9 of the 200 image pixels, in an axes about 0.78 of the figure wide: 3.5% of it.
    assert 0.025 * plain.shape[1] < tall < 0.05 * plain.shape[1]


@pytest.mark.asyncio
async def test_missing_beam_and_missing_wcs_are_said_not_hidden(client, opened_image):
    h = opened_image["headers"]
    r = await client.post(ROUTE, headers=h, json={
        "dataset_id": opened_image["dataset_id"], "beam": True, "preview": True})
    body = r.json()
    assert body["valid"], body["error"]
    assert any("BMAJ" in n for n in body["notes"]), body["notes"]


@pytest.mark.asyncio
async def test_scale_bar_label_is_a_round_length(client, ds):
    from app.figure_overlays import nice_length
    assert nice_length(41.0) == 20.0
    assert nice_length(9.9) == 5.0
    assert nice_length(0.37) == 0.2
    assert nice_length(0.0) == 0.0
    _, plain = await _preview(client, ds)
    _, long_bar = await _preview(client, ds, scale_bar=True, scale_bar_length=60.0)
    _, short_bar = await _preview(client, ds, scale_bar=True, scale_bar_length=20.0)
    # A 60 arcsec bar on 1 arcsec pixels is a third of the 200-pixel width.
    def span(img):
        _, xs = np.nonzero(_changed(plain, img))
        return np.ptp(xs)
    assert span(long_bar) > 2 * span(short_bar) * 0.8


@pytest.mark.asyncio
async def test_scale_bar_too_long_is_reported(client, ds):
    body = await _fig(client, ds, preview=True, scale_bar=True, scale_bar_length=5000.0)
    assert body["valid"]
    assert any("Scale bar" in n for n in body["notes"])


def _line(points, color="#ff0000", width=2.0, dash=False):
    props = {"color": color, "width": width}
    if dash:
        props["dash"] = True
    return {"shape": "polygon", "frame": "image", "unit": "pix", "points": points, "props": props}


@pytest.mark.asyncio
async def test_contour_overlay_is_drawn_in_its_colour(client, ds):
    # A contour of the blob (a circle of radius 40 px about (101, 81), 1-based).
    t = np.linspace(0, 2 * np.pi, 120, endpoint=False)
    ring = [[101 + 40 * np.cos(a), 81 + 40 * np.sin(a)] for a in t]
    _, plain = await _preview(client, ds, add_colorbar=False, add_grid=False)
    body, withc = await _preview(client, ds, add_colorbar=False, add_grid=False,
                                 regions=[_line(ring, color="#ff0000", width=3.0)])
    assert body["regions_drawn"] == 1
    mask = _changed(plain, withc)
    assert mask.sum() > 200
    r, g, b = (withc[..., i][mask] for i in range(3))
    assert np.median(r) > 0.8 and np.median(g) < 0.3 and np.median(b) < 0.3   # drawn in red
    # And centred on the image: the ring's pixels surround the middle of the figure.
    ys, xs = np.nonzero(mask)
    assert abs(xs.mean() - plain.shape[1] / 2) < 0.1 * plain.shape[1]


@pytest.mark.asyncio
async def test_many_contour_lines_and_every_shape(client, ds):
    regions = [_line([[10 + i, 10], [10 + i, 100]], width=1.0, dash=(i % 2 == 0)) for i in range(300)]
    regions[0]["shape"] = "line"
    regions += [
        {"shape": "circle", "frame": "image", "unit": "pix", "center": [60, 60], "radius": 10},
        {"shape": "ellipse", "frame": "image", "unit": "pix", "center": [80, 60],
         "semi_axes": [12, 5], "angle_deg": 30},
        {"shape": "box", "frame": "image", "unit": "pix", "center": [120, 60], "size": [20, 10],
         "angle_deg": 15},
        {"shape": "annulus", "frame": "image", "unit": "pix", "center": [60, 110], "radii": [5, 10]},
        {"shape": "point", "frame": "image", "unit": "pix", "center": [90, 110]},
        {"shape": "text", "frame": "image", "unit": "pix", "center": [100, 120], "text": "Nucleus"},
        {"shape": "vector", "frame": "image", "unit": "pix", "center": [30, 30], "length": 25,
         "angle_deg": 45},
    ]
    body = await _fig(client, ds, preview=True, regions=regions)
    assert body["valid"], body["error"]
    assert body["regions_drawn"] == len(regions)
    assert body["notes"] == []


@pytest.mark.asyncio
async def test_sky_region_lands_where_its_world_position_says(client, ds):
    """A circle at the reference position (the image centre) is drawn in the middle."""
    circle = {"shape": "circle", "frame": "fk5", "center": list(CRVAL), "radius": 20.0, "unit": "arcsec",
              "props": {"color": "#00ff00", "width": 3}}
    _, plain = await _preview(client, ds, add_colorbar=False, add_grid=False)
    body, withc = await _preview(client, ds, add_colorbar=False, add_grid=False, regions=[circle])
    assert body["regions_drawn"] == 1
    ys, xs = np.nonzero(_changed(plain, withc))
    assert abs(xs.mean() - plain.shape[1] / 2) < 0.03 * plain.shape[1]
    assert abs(ys.mean() - plain.shape[0] / 2) < 0.05 * plain.shape[0]


@pytest.mark.asyncio
async def test_bad_overlays_do_not_fail_the_figure(client, ds):
    body = await _fig(client, ds, preview=True, regions=[
        {"shape": "circle", "frame": "image", "unit": "pix", "center": [60, 60]},   # no radius
        {"shape": "circle", "frame": "image", "unit": "pix", "center": [60, 60], "radius": 9,
         "props": {"color": "not-a-colour"}}])
    assert body["valid"], body["error"]
    assert body["regions_drawn"] == 1
    assert any("not drawn" in n for n in body["notes"])


@pytest.mark.asyncio
async def test_preview_is_small_fast_and_writes_no_file(client, ds):
    t0 = time.perf_counter()
    body = await _fig(client, ds, preview=True, dpi=600, width_in=20, height_in=16, beam=True)
    took = time.perf_counter() - t0
    assert body["valid"], body["error"]
    assert body["output_path"] == "" and body["workspace_filename"] == ""
    assert max(body["png_width"], body["png_height"]) <= 850   # preview_px 800, plus the margins
    assert len(base64.b64decode(body["png_base64"])) < 400_000
    assert took < 3.0, took


@pytest.mark.asyncio
@pytest.mark.parametrize("opts, fragment", [
    ({"cmap": "nope"}, "colour map"),
    ({"stretch": "bogus"}, "stretch"),
    ({"font_family": "Definitely Not A Font"}, "not installed"),
    ({"vmin": 2.0, "vmax": 1.0}, "minimum below the maximum"),
    ({"overlay_color": "notacolour"}, "colour"),
    ({"width_in": 40, "height_in": 40, "dpi": 1200}, "megapixels"),
    ({"scale_bar": True, "scale_bar_unit": "arcsec", "scale_bar_length": 1.0, "regions": [
        {"shape": "polygon", "frame": "image", "unit": "pix", "points": [[1, 1]] * 3_000_001}]},
     "million vertices"),
])
async def test_bad_options_are_said(client, ds, opts, fragment):
    body = await _fig(client, ds, **opts)
    assert body["valid"] is False
    assert fragment in body["error"], body["error"]


@pytest.mark.asyncio
@pytest.mark.parametrize("opts", [{"frame": "fk4"}, {"colorbar_position": "middle"}, {"dpi": 5},
                                  {"beam_position": "center"}, {"width_in": 0.1}])
async def test_out_of_range_values_are_422(client, ds, opts):
    r = await client.post(ROUTE, headers=ds["headers"], json={"dataset_id": ds["id"], **opts})
    assert r.status_code == 422


def test_cmap_names_of_the_viewer_registry_resolve():
    """The names the desktop colour-map pickers use (src/vtk/ColorMaps.cpp) that have a
    matplotlib counterpart resolve, in any case."""
    from app.publication_figure import resolve_cmap
    for name in ("Inferno", "Viridis", "Magma", "Plasma", "Cividis", "CoolWarm", "Gray", "Purples",
                 "Blues_r", "Greens", "Reds_r", "Greys", "Oranges_r", "hot", "cubehelix"):
        assert resolve_cmap(name) is not None, name


def test_format_of():
    from app.publication_figure import format_of
    assert [format_of(n) for n in ("a.PDF", "a.svg", "a.eps", "a.png", "a", "a.tif")] == [
        "pdf", "svg", "eps", "png", "png", "png"]


@pytest.mark.asyncio
@pytest.mark.parametrize("a, b", [
    ({}, {"frame": "galactic"}),
    ({}, {"tick_format": "degrees"}),
    ({}, {"colorbar_position": "bottom"}),
    ({"colorbar_position": "top"}, {"colorbar_position": "left"}),
    ({}, {"title": "A title"}),
    ({}, {"xlabel": "East"}),
    ({"title": "A title"}, {"title": "A title", "font_family": "monospace", "title_size": 24}),
    ({}, {"cmap": "Purples_r"}),
    ({}, {"stretch": "log"}),
    ({}, {"vmin": 0.2, "vmax": 0.9}),
    ({}, {"add_grid": False}),
    ({}, {"add_colorbar": False}),
])
async def test_each_option_changes_the_picture(client, ds, a, b):
    _, pa = await _preview(client, ds, **a)
    _, pb = await _preview(client, ds, **b)
    # The same size (the figure is not cropped), so the pictures can be compared.
    assert pa.shape == pb.shape
    assert _changed(pa, pb).sum() > 20, f"{b} left the picture as {a}"


@pytest.mark.asyncio
async def test_pixels_below_the_range_take_the_lowest_colour_not_a_hole(client, ds):
    """sqrt of a negative number is NaN: with the range above most pixels, the plane
    must still be filled with the lowest colour of the map, not left transparent."""
    _, px = await _preview(client, ds, stretch="sqrt", vmin=0.5, vmax=1.0, cmap="Greys_r",
                           add_colorbar=False, add_grid=False)
    h, w = px.shape[:2]
    # Inside the axes, far from the blob: Greys_r's lowest colour is black, a hole is white.
    patch = px[int(0.45 * h):int(0.55 * h), int(0.25 * w):int(0.30 * w)]
    assert patch.mean() < 0.15, patch.mean()


@pytest.mark.asyncio
async def test_overlays_outside_the_image_do_not_move_the_corner_marks(client, ds):
    """A line that runs far outside the image must not widen the axes (the beam would
    then be placed in the wider box and cut away)."""
    far = _line([[-500, -500], [900, 900]])
    kw = {"add_colorbar": False, "add_grid": False}
    _, no_beam = await _preview(client, ds, **kw)
    _, beam = await _preview(client, ds, beam=True, **kw)
    _, far_no_beam = await _preview(client, ds, regions=[far], **kw)
    _, far_beam = await _preview(client, ds, beam=True, regions=[far], **kw)
    ys0, xs0 = np.nonzero(_changed(no_beam, beam))
    ys1, xs1 = np.nonzero(_changed(far_no_beam, far_beam))
    assert xs0.size > 30 and xs1.size > 30
    # The beam is drawn in the same place with or without the far-away line.
    assert abs(xs0.mean() - xs1.mean()) < 3 and abs(ys0.mean() - ys1.mean()) < 3


def test_desktop_colour_maps_with_a_stand_in_resolve():
    from app.publication_figure import resolve_cmap
    assert resolve_cmap("Glow").name == "hot"
    assert resolve_cmap("Default").name == "inferno"
