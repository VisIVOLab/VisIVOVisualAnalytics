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


@pytest.mark.asyncio
async def test_svg_keeps_its_text_as_text_and_pdf_embeds_truetype(client, ds):
    """Journals and editors need to edit the labels: matplotlib's default turns every
    glyph of an SVG into an outline and writes Type 3 fonts into a PDF."""
    svg = await _fig(client, ds, output_basename="text41.svg", title="Blob title", xlabel="East")
    text = Path(svg["output_path"]).read_text()
    assert "<text" in text and "Blob title" in text and "East" in text
    pdf = await _fig(client, ds, output_basename="text41.pdf", title="Blob title")
    data = Path(pdf["output_path"]).read_bytes()
    assert b"/CIDFontType2" in data and b"/Type3" not in data   # TrueType outlines
    # the setting is the save's, not the process's
    import matplotlib  # noqa: PLC0415
    assert matplotlib.rcParams["svg.fonttype"] == "path"


@pytest.fixture
async def ds_nan(client, opened_image, beam_image, tmp_path):
    """The blob with its left strip blank (NaN), as outside the disc of a map."""
    with fits.open(beam_image) as hdul:
        data = hdul[0].data.copy()
        header = hdul[0].header.copy()
    data[:, :40] = np.nan
    p = tmp_path / "blank.fits"
    fits.PrimaryHDU(data, header).writeto(p)
    h = opened_image["headers"]
    r = await client.post("/v1/datasets/open", json={"path": str(p)}, headers=h)
    assert r.json()["valid"], r.json()
    return {"id": r.json()["dataset_id"], "headers": h}


@pytest.mark.asyncio
@pytest.mark.parametrize("stretch", ["linear", "log"])
async def test_blank_pixels_are_not_the_top_colour(client, ds_nan, stretch):
    """astropy's clip dropped the mask, so a NaN came out as the top colour (black in
    Greys); it is the page's white."""
    _, px = await _preview(client, ds_nan, cmap="Greys", stretch=stretch, vmin=-0.05, vmax=1.0,
                           add_colorbar=False, add_grid=False, width_in=6, height_in=4.8,
                           preview_px=600)
    h, w = px.shape[:2]
    # The axes span about 0.125..0.9 of the width and 0.11..0.88 of the height: the
    # blank strip is the left fifth of them, the blob (dark) is at their centre.
    strip = px[int(0.3 * h):int(0.7 * h), int(0.15 * w):int(0.2 * w)]
    assert strip.mean() > 0.95, strip.mean()
    centre = px[int(0.45 * h):int(0.55 * h), int(0.5 * w):int(0.55 * w)]
    assert centre.mean() < 0.6, centre.mean()


@pytest.mark.asyncio
async def test_blank_pixels_in_every_format(client, ds_nan):
    for ext in ("pdf", "svg", "eps"):
        body = await _fig(client, ds_nan, output_basename=f"blank41.{ext}", cmap="Greys")
        assert body["valid"], body["error"]


@pytest.mark.parametrize("position", ["bottom", "top"])
@pytest.mark.parametrize("tick_size", [10.0, 14.0])
@pytest.mark.parametrize("stretch", ["linear", "log", "squared", "asinh", "sqrt"])
def test_horizontal_colour_bar_labels_do_not_overlap(position, tick_size, stretch):
    from astropy.visualization import (AsinhStretch, ImageNormalize, LinearStretch, LogStretch,
                                       PowerStretch, SqrtStretch)
    from matplotlib.backends.backend_agg import FigureCanvasAgg
    from matplotlib.figure import Figure
    from app.publication_figure import _shorten_horizontal_ticks
    fig = Figure(figsize=(6.4, 4.8), dpi=100)
    canvas = FigureCanvasAgg(fig)
    ax = fig.add_subplot(1, 1, 1)
    im = ax.imshow(np.linspace(-1.4e-4, 1.75e-4, 100).reshape(10, 10),
                   norm=ImageNormalize(vmin=-1.4e-4, vmax=1.75e-4, stretch={
                       "linear": LinearStretch(), "log": LogStretch(), "squared": PowerStretch(2.0),
                       "asinh": AsinhStretch(), "sqrt": SqrtStretch()}[stretch]))
    cbar = fig.colorbar(im, ax=ax, location=position, fraction=0.046, pad=0.14)
    cbar.ax.tick_params(labelsize=tick_size)
    _shorten_horizontal_ticks(cbar, fig, tick_size, "sans-serif")
    canvas.draw()
    renderer = canvas.get_renderer()
    labels = [t for t in cbar.ax.get_xticklabels() if t.get_text() and t.get_visible()]
    boxes = [t.get_window_extent(renderer) for t in labels]
    boxes.append(cbar.ax.xaxis.get_offset_text().get_window_extent(renderer))
    assert len(labels) >= 2
    for i, a in enumerate(boxes):
        for b in boxes[i + 1:]:
            assert not a.overlaps(b), (a, b)
    # short: nothing like -0.00015 is printed
    assert all(len(t.get_text()) <= 6 for t in labels), [t.get_text() for t in labels]


# ── Review round on PR #540 ──────────────────────────────────────────────────


def _luma(color) -> float:
    from matplotlib.colors import to_rgb
    r, g, b = to_rgb(color)
    return 0.299 * r + 0.587 * g + 0.114 * b


@pytest.mark.parametrize("cmap_name", ["Purples", "Inferno", "Greys_r", "Gray"])
def test_text_annotation_box_is_the_opposite_tone_of_its_text(cmap_name):
    """Black text (the 'auto' colour on a light map) was on a dark box, white text on a
    dark one: the box takes the other tone, on a light colour map as on a dark one."""
    from matplotlib.figure import Figure
    from app.figure_overlays import draw_regions
    from app.publication_figure import _overlay_color, resolve_cmap
    color = _overlay_color(resolve_cmap(cmap_name), "auto")
    ax = Figure().add_subplot(1, 1, 1)
    n = draw_regions(ax, [{"shape": "text", "frame": "image", "unit": "pix", "center": [5, 5],
                           "text": "Nucleus"}], default_color=color)
    assert n == 1
    text = ax.texts[0]
    assert abs(_luma(text.get_color()) - _luma(text.get_bbox_patch().get_facecolor())) > 0.6
    assert text.get_bbox_patch().get_facecolor()[3] == 1.0   # opaque: EPS has no transparency


@pytest.mark.parametrize("color", ["black", "white", "#ffcc00", "#800000", "#00e5ff"])
def test_text_annotation_box_for_any_region_colour(color):
    from matplotlib.figure import Figure
    from app.figure_overlays import draw_regions
    ax = Figure().add_subplot(1, 1, 1)
    draw_regions(ax, [{"shape": "text", "frame": "image", "unit": "pix", "center": [5, 5],
                       "text": "x", "props": {"color": color}}])
    text = ax.texts[0]
    assert abs(_luma(text.get_color()) - _luma(text.get_bbox_patch().get_facecolor())) > 0.4


def _tilted_wcs(cdelt1=-1 / 3600, rot_deg=0.0, system="fk5", crval=(150.0, 2.0)):
    from astropy.wcs import WCS
    w = WCS(naxis=2)
    w.wcs.ctype = ("RA---TAN", "DEC--TAN") if system == "fk5" else ("GLON-TAN", "GLAT-TAN")
    w.wcs.crval = crval
    w.wcs.crpix = (100.5, 80.5)
    w.wcs.cdelt = (cdelt1, abs(cdelt1))
    t = np.radians(rot_deg)
    w.wcs.pc = [[np.cos(t), -np.sin(t)], [np.sin(t), np.cos(t)]]
    if system == "fk5":
        w.wcs.radesys, w.wcs.equinox = "FK5", 2000.0
    return w


def _reference_pixel_angle(w, x, y, pa_deg, eps=1e-3):
    """The angle of a position angle on the pixel grid, by stepping on the sky with the
    WCS alone (the method the figure used before it shared regions.py's)."""
    lng, lat = int(w.wcs.lng), int(w.wcs.lat)
    world = np.array(w.wcs_pix2world([[x, y]], 0), dtype=float)[0]
    t = np.radians(pa_deg)
    step = world.copy()
    step[lat] += eps * np.cos(t)
    step[lng] += eps * np.sin(t) / np.cos(np.radians(world[lat]))
    p = np.array(w.wcs_world2pix([step], 0), dtype=float)[0] - np.array([x, y])
    return np.degrees(np.arctan2(p[1], p[0]))


@pytest.mark.parametrize("cdelt1", [-1 / 3600, 1 / 3600])
@pytest.mark.parametrize("rot", [0.0, 30.0, 50.0])
@pytest.mark.parametrize("pa", [0.0, 30.0, 90.0, 215.0])
def test_beam_angle_follows_the_sky_for_flips_and_rotations(cdelt1, rot, pa):
    """The beam's position angle is on the sky, so on the grid it turns with the WCS:
    mirrored (CDELT1 either sign) and rotated (PC) alike."""
    from app.figure_overlays import pixel_frame, sky_pixel_angle
    w = _tilted_wcs(cdelt1, rot)
    got = sky_pixel_angle(pixel_frame(w), 40.0, 30.0, pa)
    want = _reference_pixel_angle(w, 40.0, 30.0, pa)
    assert abs((got - want + 180.0) % 360.0 - 180.0) < 0.05, (got, want)


def test_pixel_scale_is_the_one_regions_uses_and_is_none_without_sky():
    from astropy.wcs import WCS
    from app.figure_overlays import pixel_frame
    assert pixel_frame(_tilted_wcs()).scale_deg == pytest.approx(1 / 3600)
    assert pixel_frame(WCS(naxis=2)) is None


def _compass_arrows(wcs, frame, shape=(160, 200)):
    """(labels, pixel angles of the north and east arrows) the compass draws."""
    from matplotlib.figure import Figure
    from matplotlib.text import Annotation
    from app.figure_overlays import draw_compass
    ax = Figure().add_subplot(1, 1, 1)
    ax.set_xlim(-0.5, shape[1] - 0.5)
    ax.set_ylim(-0.5, shape[0] - 0.5)
    notes: list[str] = []
    assert draw_compass(ax, wcs, frame, "top-right", "white", 10.0, notes), notes
    arrows = [t for t in ax.texts if isinstance(t, Annotation)]
    labels = [t.get_text() for t in ax.texts if not isinstance(t, Annotation)]
    angles = [np.degrees(np.arctan2(a.xy[1] - a.xyann[1], a.xy[0] - a.xyann[0])) for a in arrows]
    return labels, angles


def _reference_compass(wcs, native: str, shown: str, shape=(160, 200), eps_arcsec=20.0):
    """North and east of ``shown`` at the image centre on the grid of a ``native`` WCS,
    by an offset on the sky made with astropy's frames alone."""
    import astropy.units as u
    from astropy.coordinates import FK5, Galactic, SkyCoord
    frames = {"fk5": FK5(equinox="J2000"), "galactic": Galactic()}
    x, y = 0.5 * (shape[1] - 1), 0.5 * (shape[0] - 1)
    lon, lat = wcs.wcs_pix2world([[x, y]], 0)[0]
    c = SkyCoord(lon * u.deg, lat * u.deg, frame=frames[native]).transform_to(frames[shown])
    out = []
    for dlon, dlat in ((0.0, eps_arcsec), (eps_arcsec / np.cos(c.spherical.lat.rad), 0.0)):
        s = SkyCoord(c.spherical.lon + dlon * u.arcsec, c.spherical.lat + dlat * u.arcsec,
                     frame=frames[shown]).transform_to(frames[native])
        p = np.array(wcs.wcs_world2pix([[s.spherical.lon.deg, s.spherical.lat.deg]], 0)[0]) - [x, y]
        out.append(np.degrees(np.arctan2(p[1], p[0])))
    return out


@pytest.mark.parametrize("native, shown, labels", [
    ("galactic", "fk5", ["N", "E"]),      # a Galactic image whose axes are labelled RA/Dec
    ("fk5", "galactic", ["b", "l"]),      # and the other way round
    ("fk5", "fk5", ["N", "E"]),
    ("galactic", "galactic", ["b", "l"]),
])
def test_compass_follows_the_frame_of_the_axes(native, shown, labels):
    wcs = _tilted_wcs(-1 / 3600, 20.0, system=native,
                      crval=(150.0, 2.0) if native == "fk5" else (210.0, 40.0))
    names, got = _compass_arrows(wcs, "" if shown == native else shown)
    assert names == labels
    want = _reference_compass(wcs, native, shown)
    for g, w_ in zip(got, want):
        assert abs((g - w_ + 180.0) % 360.0 - 180.0) < 0.5, (got, want)
    if native != shown:
        # The two norths are apart by a real angle: this is not the native compass.
        own = _compass_arrows(wcs, "")[1]
        assert abs((own[0] - got[0] + 180.0) % 360.0 - 180.0) > 5.0, (own, got)


def test_compass_in_the_ecliptic_frame_is_named_n_e_and_turned():
    wcs = _tilted_wcs(-1 / 3600, 0.0)
    names, got = _compass_arrows(wcs, "ecliptic")
    assert names == ["N", "E"]
    assert abs((got[0] - _compass_arrows(wcs, "")[1][0] + 180.0) % 360.0 - 180.0) > 5.0


@pytest.mark.asyncio
async def test_ecliptic_frame_is_asked_for_and_drawn(client, ds):
    body = await _fig(client, ds, output_basename="ecl41.svg", frame="ecliptic", compass=True)
    assert body["valid"], body["error"]
    text = Path(body["output_path"]).read_text()
    assert "Ecliptic longitude" in text and "Ecliptic latitude" in text
    assert "Right Ascension" not in text
    # The ticks are the ecliptic's: a different picture from the native one.
    _, native = await _preview(client, ds)
    _, ecl = await _preview(client, ds, frame="ecliptic")
    assert _changed(native, ecl).sum() > 100


@pytest.mark.asyncio
@pytest.mark.parametrize("frame, expect", [("native", ""), ("galactic", "galactic"),
                                           ("ecliptic", "ecliptic"), ("icrs", "")])
async def test_compass_is_asked_for_the_frame_of_the_axes(client, ds, monkeypatch, frame, expect):
    """The figure hands the compass the frame the ticks are in ("" for the image's own,
    here ICRS), so the arrows and their names are those of the axes."""
    from app import figure_overlays as ov
    seen = []
    real = ov.draw_compass
    monkeypatch.setattr(ov, "draw_compass", lambda ax, wcs, fr, *a, **k: (seen.append(fr),
                                                                          real(ax, wcs, fr, *a, **k))[1])
    body = await _fig(client, ds, preview=True, frame=frame, compass=True)
    assert body["valid"], body["error"]
    assert seen == [expect] and body["notes"] == []


def test_scale_bar_unit_follows_the_size_of_the_image():
    from app.figure_overlays import auto_unit
    assert [auto_unit(d) for d in (30 / 3600, 59.9 / 3600, 1 / 60, 59 / 60, 1.0, 20.0)] == [
        "arcsec", "arcsec", "arcmin", "arcmin", "deg", "deg"]


@pytest.mark.parametrize("cdelt_arcsec, expect", [
    (1.0, "20″"),           # 200 px of 1": a fifth is 40"
    (20.0, "10′"),          # 200 px of 20": a fifth is 800" = 13.3'
    (0.5 * 3600.0, "20°"),  # 200 px of half a degree: a fifth is 20 degrees
])
def test_scale_bar_round_length_picks_its_unit(cdelt_arcsec, expect):
    from matplotlib.figure import Figure
    from app.figure_overlays import draw_scale_bar
    wcs = _tilted_wcs(-cdelt_arcsec / 3600)
    ax = Figure().add_subplot(1, 1, 1)
    ax.set_xlim(-0.5, 199.5)
    ax.set_ylim(-0.5, 159.5)
    notes: list[str] = []
    assert draw_scale_bar(ax, wcs, 0.0, "auto", "bottom-right", "white", 10.0, notes), notes
    assert [t.get_text() for t in ax.texts] == [expect]


def test_scale_bar_unit_picked_by_the_user_is_kept():
    from matplotlib.figure import Figure
    from app.figure_overlays import draw_scale_bar
    ax = Figure().add_subplot(1, 1, 1)
    ax.set_xlim(-0.5, 199.5)
    ax.set_ylim(-0.5, 159.5)
    assert draw_scale_bar(ax, _tilted_wcs(-20 / 3600), 0.0, "arcsec", "bottom-right", "white", 10.0, [])
    assert ax.texts[0].get_text().endswith("″")


def _req(**kw):
    from types import SimpleNamespace
    base = dict(stretch="log", vmin=None, vmax=None, percent=99.5, gamma=1.0)
    return SimpleNamespace(**{**base, **kw})


def test_log_stretch_is_the_windows_pure_logarithm():
    """Colours evenly spaced in log10 of the value over the range, as the window's
    VTK log table, and matplotlib's LogNorm: astropy's LogStretch is another curve."""
    from matplotlib.colors import LogNorm
    from app.publication_figure import _norm
    data = np.array([[1e-3, 5e-3, 2e-2], [0.1, 0.5, 1.0]])
    norm = _norm(data, _req(vmin=1e-3, vmax=1.0))
    ref = LogNorm(vmin=1e-3, vmax=1.0)
    v = np.array([1e-3, 3e-3, 1e-2, 0.07, 0.3, 1.0])
    assert np.allclose(np.asarray(norm(v)), np.asarray(ref(v)), atol=1e-9)
    # The inverse is what the colour bar's ticks are read from.
    assert np.allclose(norm.inverse(np.linspace(0, 1, 5)), ref.inverse(np.linspace(0, 1, 5)),
                       rtol=1e-9)


def test_log_stretch_floors_a_range_that_reaches_zero_or_below():
    """Radio data are negative wherever there is only noise: the minimum is raised to a
    positive floor, as the window does, and the pixels below it take the lowest colour."""
    from app.publication_figure import _norm
    rng = np.random.default_rng(3)
    data = np.abs(rng.standard_normal((50, 50))) * 0.01
    data[0, 0] = 1.0
    data = data - 0.003     # some pixels negative
    norm = _norm(data, _req(vmin=-0.02, vmax=1.0))
    assert norm.vmin > 0.0 and norm.vmax == 1.0
    assert float(norm(norm.vmin)) == pytest.approx(0.0, abs=1e-12)
    assert float(norm(1.0)) == pytest.approx(1.0)
    norm2 = _norm(data, _req())    # percentile mode: the lower clip is below zero here
    assert norm2.vmin > 0.0
    with pytest.raises(ValueError, match="positive maximum"):
        _norm(-np.abs(data) - 1e-3, _req(vmin=-1.0, vmax=-1e-3))


@pytest.mark.asyncio
@pytest.mark.parametrize("position", ["right", "bottom"])
async def test_log_stretch_renders_with_a_colour_bar_and_a_signed_range(client, ds, position):
    body = await _fig(client, ds, preview=True, stretch="log", vmin=-0.02, vmax=1.0,
                      colorbar_position=position)
    assert body["valid"], body["error"]


COLOUR_MAPS_ROUTE = "/v1/products/figure_colour_maps"


@pytest.mark.asyncio
async def test_supported_colour_maps_are_those_the_figure_can_set(client, ds):
    r = await client.post(COLOUR_MAPS_ROUTE, headers=ds["headers"],
                          json={"names": ["Inferno", "AllRed", "purples", "Glow", "NoSuchMap",
                                          "Blues_r"]})
    assert r.status_code == 200, r.text
    assert [m["name"] for m in r.json()["maps"]] == ["Inferno", "purples", "Glow", "Blues_r"]


@pytest.mark.asyncio
async def test_colour_maps_say_which_are_exact_and_which_stand_in(client, ds):
    """The viewer's Default (an HSV ramp), Glow, Temperature, Sar, VolRenRGB, Rainbow and
    CoolWarm are drawn with a close matplotlib map, not their own table: said, with the
    map used. Inferno and the Brewer maps are matplotlib's own tables."""
    names = ["Inferno", "Viridis", "Gray", "Purples_r", "Default", "Glow", "Temperature", "Sar",
             "VolRenRGB", "Rainbow", "CoolWarm"]
    r = await client.post(COLOUR_MAPS_ROUTE, headers=ds["headers"], json={"names": names})
    assert r.status_code == 200, r.text
    got = {m["name"]: m for m in r.json()["maps"]}
    assert [n for n in names if got[n]["exact"]] == ["Inferno", "Viridis", "Gray", "Purples_r"]
    assert got["Default"]["drawn_as"] == "inferno" and got["Glow"]["drawn_as"] == "hot"
    assert got["Temperature"]["drawn_as"] == "RdYlBu_r" and got["Sar"]["drawn_as"] == "afmhot"
    assert got["VolRenRGB"]["drawn_as"] == "jet" and got["Rainbow"]["drawn_as"] == "rainbow"


@pytest.mark.asyncio
async def test_colour_maps_offer_matplotlibs_extras_not_already_listed(client, ds):
    """hot and cubehelix are not in the viewer's registry: they come after it, and a
    name the registry already has is not offered twice."""
    r = await client.post(COLOUR_MAPS_ROUTE, headers=ds["headers"],
                          json={"names": ["Inferno", "Hot"]})
    extra = r.json()["extra"]
    assert "cubehelix" in extra and "hot" not in extra     # Hot (any case) is already listed
    r = await client.post(COLOUR_MAPS_ROUTE, headers=ds["headers"], json={"names": ["Inferno"]})
    assert {"hot", "cubehelix"} <= set(r.json()["extra"])


@pytest.mark.asyncio
async def test_an_approximated_colour_map_is_noted_on_the_figure(client, ds):
    body = await _fig(client, ds, preview=True, cmap="Glow")
    assert body["valid"], body["error"]
    assert any("'Glow'" in n and "'hot'" in n and "not the same" in n for n in body["notes"])
    assert (await _fig(client, ds, preview=True, cmap="Inferno"))["notes"] == []


def _inferno_lut(n=256):
    import matplotlib
    return np.asarray(matplotlib.colormaps["inferno"](np.linspace(0, 1, n)))[:, :3].tolist()


@pytest.mark.asyncio
async def test_the_windows_own_table_is_drawn_exactly_and_not_noted(client, ds):
    """A ``lut`` (the window's 256 colours) replaces the named map: the figure has the
    table's colours whatever the name says, with no 'close to' note."""
    plain = _pixels(await _fig(client, ds, preview=True, crop=False, cmap="inferno"))
    with_lut = await _fig(client, ds, preview=True, crop=False, cmap="Glow", lut=_inferno_lut())
    assert with_lut["valid"], with_lut["error"]
    assert with_lut["notes"] == []
    # Inferno sampled at 256 steps is inferno at 256 levels (matplotlib's own N).
    assert np.abs(_pixels(with_lut) - plain).max() < 0.02
    # And a table that is not inferno draws differently from it.
    red = [[i / 255.0, 0.0, 0.0] for i in range(256)]
    other = _pixels(await _fig(client, ds, preview=True, crop=False, cmap="inferno", lut=red))
    assert _changed(plain, other).sum() > 1000


@pytest.mark.asyncio
@pytest.mark.parametrize("lut", [[[0.0, 0.0, 0.0]], [[0.0, 0.0], [1.0, 1.0]],
                                 [[0.0, 0.0, 0.0], [1.0, 2.0, 1.0]]])
async def test_a_malformed_colour_table_is_refused(client, ds, lut):
    r = await client.post(ROUTE, headers=ds["headers"],
                          json={"dataset_id": ds["id"], "preview": True, "lut": lut})
    body = r.json()
    assert r.status_code == 422 or (r.status_code == 200 and not body["valid"]), body


def test_the_colour_map_listing_is_not_a_coroutine():
    """Blocking work, so a plain def (run on the thread pool), not an async one."""
    import inspect
    from app.routers.products import figure_colour_maps
    assert not inspect.iscoroutinefunction(figure_colour_maps)


def test_a_native_ecliptic_grid_gets_its_own_compass_not_a_turned_one():
    """ELON/ELAT axes are named ecliptic, so asking for frame=ecliptic leaves the compass
    where the grid has it instead of turning it by the ICRS-to-ecliptic angle."""
    from astropy.wcs import WCS
    w = WCS(naxis=2)
    w.wcs.ctype = ("ELON-TAN", "ELAT-TAN")
    w.wcs.crval = (120.0, 10.0)
    w.wcs.crpix = (100.5, 80.5)
    w.wcs.cdelt = (-1 / 3600, 1 / 3600)
    names, own = _compass_arrows(w, "")
    names_e, ecl = _compass_arrows(w, "ecliptic")
    assert names == names_e == ["N", "E"]
    for a, b in zip(own, ecl):
        assert abs((a - b + 180.0) % 360.0 - 180.0) < 1e-6
    # The arrows are the grid's own north and east (up and left on this grid), not an
    # ICRS north, which would be turned by the ICRS-to-ecliptic angle.
    for got, want in zip(own, (90.0, 180.0)):
        assert abs((got - want + 180.0) % 360.0 - 180.0) < 1.0, own
    # A frame it is not in still turns it.
    assert abs((_compass_arrows(w, "galactic")[1][0] - own[0] + 180.0) % 360.0 - 180.0) > 5.0
