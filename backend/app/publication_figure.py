"""Rendering of the publication figure (``POST /v1/products/publication_figure``).

The route and the request model are in ``routers/products.py``, the loader in
``products_advanced.worker_publication_figure``; this module draws. It is the one
place that decides what a figure looks like, for the file and for the low
resolution preview the desktop dialog shows while its options change (issue #41):
both are the same call, the preview only with a smaller picture.

The figure is built on ``matplotlib.figure.Figure``, not ``pyplot``, so it keeps
no global state and may run on a worker thread next to other requests; a lock
still serialises renders because font lookup is shared.
"""
from __future__ import annotations

import base64
import io
import math
import struct
import threading
from typing import Any

import numpy as np

from . import figure_overlays as ov

FORMATS = ("png", "pdf", "svg", "eps")
STRETCHES = ("linear", "sqrt", "squared", "log", "asinh", "sinh", "power")
GENERIC_FONTS = ("sans-serif", "serif", "monospace")
MAX_FIGURE_PIXELS = 120_000_000
# A picture larger than this on a side is decimated for a preview: the preview
# only has to show the layout, and imshow of a 20000-pixel image costs seconds.
PREVIEW_DECIMATE_ABOVE = 4096

_LOCK = threading.Lock()


def format_of(basename: str) -> str:
    """The file format a name asks for (its extension), ``png`` when it has none."""
    ext = basename.rsplit(".", 1)[-1].lower() if "." in basename else ""
    return ext if ext in FORMATS else "png"


def resolve_cmap(name: str):
    """A matplotlib colour map for ``name``, case-insensitively: the viewer's
    registry spells ``Inferno`` and ``CoolWarm`` where matplotlib has ``inferno``
    and ``coolwarm``. Raises ValueError, naming the map, when none matches."""
    import matplotlib  # noqa: PLC0415
    # The desktop names with a matplotlib stand-in (Glow is hot, Default is
    # inferno) are the table the volume renderer already uses.
    from .render_session._common import _DESKTOP_TO_MPL  # noqa: PLC0415
    names = list(matplotlib.colormaps)
    for candidate in (name, _DESKTOP_TO_MPL.get(name, ""),
                      *(n for n in names if n.lower() == name.lower())):
        if candidate in matplotlib.colormaps:
            return matplotlib.colormaps[candidate]
    raise ValueError(f"Unknown colour map '{name}'.")


def supported_cmaps(names: list[str]) -> list[str]:
    """The ``names`` that ``resolve_cmap`` knows, in the order given: the desktop
    asks with its colour-map registry, so its figure dialog offers only maps the
    figure can set."""
    out = []
    for n in names:
        try:
            resolve_cmap(n)
        except ValueError:
            continue
        out.append(n)
    return out


def check_font(family: str) -> str:
    """``family`` when matplotlib can set text in it. A name that is not installed
    would fall back silently to the default, and a figure for a journal must not
    say Times and print DejaVu: it is refused instead."""
    if family in GENERIC_FONTS:
        return family
    from matplotlib import font_manager  # noqa: PLC0415
    try:
        font_manager.findfont(family, fallback_to_default=False)
    except ValueError:
        raise ValueError(
            f"Font '{family}' is not installed on the backend: use sans-serif, serif or "
            "monospace, or a family that is.") from None
    return family


def _overlay_color(cmap, requested: str) -> str:
    """White over a dark colour map, black over a light one, unless asked."""
    if requested and requested != "auto":
        from matplotlib.colors import is_color_like  # noqa: PLC0415
        if not is_color_like(requested):
            raise ValueError(f"Unknown overlay colour '{requested}'.")
        return requested
    lum = np.asarray(cmap(np.linspace(0.0, 0.5, 16)))[:, :3] @ np.array([0.299, 0.587, 0.114])
    return "white" if float(lum.mean()) < 0.5 else "black"


def _png_size(png: bytes) -> tuple[int, int]:
    return struct.unpack(">II", png[16:24]) if png[:8] == b"\x89PNG\r\n\x1a\n" else (0, 0)


def _log_stretch(lo: float, hi: float):
    """The window's logarithm on the range [lo, hi] (both positive): colours evenly
    spaced in log10 of the value, ``log(x / lo) / log(hi / lo)``, as VTK's log10
    lookup table gives them. astropy's own ``LogStretch`` is a different curve
    (log(1 + a t) / log(1 + a) of the linear position), which is why it is not used."""
    from astropy.visualization import BaseStretch  # noqa: PLC0415
    span, decades = hi - lo, math.log(hi / lo)

    class _Log10Stretch(BaseStretch):
        def __call__(self, values, clip=True, out=None):
            v = np.asarray(values, dtype=float)
            x = np.maximum(lo + v * span, lo)
            res = np.log(x / lo) / decades
            if clip:
                res = np.clip(res, 0.0, 1.0)
            if out is not None:
                out[...] = res
                return out
            return res

        @property
        def inverse(self):
            class _Inverse(BaseStretch):
                def __call__(self, values, clip=True, out=None):
                    res = (lo * np.exp(np.asarray(values, dtype=float) * decades) - lo) / span
                    if clip:
                        res = np.clip(res, 0.0, 1.0)
                    if out is not None:
                        out[...] = res
                        return out
                    return res
            return _Inverse()

    return _Log10Stretch()


def _log_range(lo: float, hi: float, finite: np.ndarray) -> tuple[float, float]:
    """The range of a log stretch: ``lo`` and ``hi`` with a non-positive ``lo`` raised
    to a positive floor, as the window does (radio data are negative wherever there
    is only noise). The floor is the 1st percentile of the positive pixels, the
    window's own rule, or a millionth of the maximum when there are none."""
    if not hi > 0.0:
        raise ValueError("A log stretch needs a positive maximum: use another stretch "
                         "or raise the maximum.")
    if lo > 0.0:
        return lo, hi
    pos = finite[finite > 0.0]
    floor = float(np.percentile(pos, 1.0)) if pos.size else 0.0
    if not 0.0 < floor < hi:
        floor = hi * 1e-6
    return floor, hi


def _norm(data: np.ndarray, req):
    """The colour normalisation: the range of the window (vmin/vmax) or the
    percentile clip, then the stretch. Built from astropy's ImageNormalize and
    stretch classes, which keep their names across versions (simple_norm renamed
    its range arguments). ``log`` is the window's pure logarithm (``_log_stretch``)."""
    from astropy.visualization import (  # noqa: PLC0415
        AsinhStretch, ImageNormalize, LinearStretch, PercentileInterval,
        PowerStretch, SinhStretch, SqrtStretch)
    stretches = {"linear": LinearStretch, "sqrt": SqrtStretch,
                 "asinh": AsinhStretch, "sinh": SinhStretch,
                 "squared": lambda: PowerStretch(2.0),   # the viewer's Square stretch
                 "power": lambda: PowerStretch(req.gamma)}
    if req.stretch not in stretches and req.stretch != "log":
        raise ValueError(f"Unknown stretch '{req.stretch}': use one of {', '.join(STRETCHES)}.")
    finite = data[np.isfinite(data)]
    if finite.size == 0:
        raise ValueError("The image has no finite pixel to draw.")
    if req.vmin is None and req.vmax is None:
        lo, hi = PercentileInterval(req.percent).get_limits(finite)
    else:
        lo = float(req.vmin) if req.vmin is not None else float(finite.min())
        hi = float(req.vmax) if req.vmax is not None else float(finite.max())
    if not lo < hi:
        raise ValueError("The range needs a minimum below the maximum.")
    if req.stretch == "log":
        lo, hi = _log_range(lo, hi, finite)
        return ImageNormalize(vmin=lo, vmax=hi, stretch=_log_stretch(lo, hi), clip=False)
    # No clip=True here: astropy's clip drops the mask, so a blank pixel (outside the
    # image disc) would come out as the top colour. The data are clipped to the range
    # by ``_masked`` instead, which keeps the blanks blank.
    return ImageNormalize(vmin=lo, vmax=hi, stretch=stretches[req.stretch](), clip=False)


def _masked(data: np.ndarray, norm) -> np.ma.MaskedArray:
    """``data`` as drawn: a pixel below or above the range is the lowest or highest
    colour (not a negative number for a stretch such as sqrt, log or power to turn
    into a hole), and a blank (NaN) pixel stays masked, so it takes the colour map's
    bad colour."""
    # One copy of the picture (a large image is held once more here, not twice): the
    # masked array owns its data, which are clipped in place.
    if data.dtype.kind != "f":
        data = data.astype(np.float32)
    out = np.ma.masked_invalid(data, copy=True)
    np.clip(out.data, norm.vmin, norm.vmax, out=out.data)
    return out


def _with_blank_colour(cmap):
    """A copy of ``cmap`` whose bad colour is transparent: blank pixels leave the
    background (white) of the page, in the raster and in the vector files, instead
    of the colour map's own bad colour or its top colour. A copy, because the
    registry's maps are shared."""
    return cmap.with_extremes(bad=(1.0, 1.0, 1.0, 0.0))


# Text stays text in the vector files: SVG keeps <text> elements (editable in a
# journal's or an editor's tools), PDF and EPS embed TrueType (42), not Type 3 glyph
# outlines. Set around the save, not in the global rc.
_VECTOR_TEXT_RC = {"svg.fonttype": "none", "pdf.fonttype": 42, "ps.fonttype": 42}


def _shorten_horizontal_ticks(cbar, fig, tick_size: float, family: str) -> None:
    """Fewer, shorter tick labels on a horizontal colour bar. Its labels sit side by
    side, so a range such as -1.4e-4..1.75e-4 printed in full (-0.00015, -0.00010) runs
    them together at the ends: the number of ticks follows the bar's length and the
    common power of ten moves to one multiplier at the end of the bar."""
    from astropy.visualization import LinearStretch  # noqa: PLC0415
    from matplotlib.ticker import FixedLocator, MaxNLocator, ScalarFormatter  # noqa: PLC0415
    length_pt = cbar.ax.get_position().width * fig.get_figwidth() * 72.0
    # A label of a short number is about six characters of 0.6 em; keep a gap as wide.
    nbins = int(max(2, min(8, length_pt // (8.0 * tick_size))))
    fmt = ScalarFormatter()   # plain text, so the family asked for applies to it
    fmt.set_powerlimits((-3, 4))
    stretch = getattr(cbar.norm, "stretch", None)
    if stretch is None or isinstance(stretch, LinearStretch):
        cbar.locator = MaxNLocator(nbins=nbins)
    else:
        # The bar's axis is stretched (log, sqrt, ...): ticks evenly spaced in data
        # would crowd at one end, so they are evenly spaced along the bar instead.
        n = max(2, min(nbins, 5))
        cbar.locator = FixedLocator(cbar.norm.inverse(np.linspace(0.0, 1.0, n)))
    cbar.formatter = fmt
    cbar.update_ticks()
    cbar.ax.xaxis.get_offset_text().set(fontsize=tick_size, fontfamily=family)


def _set_ticks(coords, native: str, req) -> None:
    """Tick label format of a WCSAxes coordinate pair (longitude, latitude)."""
    if req.tick_format == "auto":
        return
    equatorial = native in ("fk5", "icrs", "fk4")
    if req.tick_format == "degrees":
        fmts = ("d.dd", "d.dd")
    else:
        fmts = ("hh:mm:ss" if equatorial else "dd:mm:ss", "dd:mm:ss")
    for c, f in zip((coords[0], coords[1]), fmts):
        c.set_major_formatter(f)


def render(data: np.ndarray, header, wcs, beam, req, *, output_path: str | None,
           preview: bool) -> dict[str, Any]:
    """Draw the figure of ``req`` for ``data``. With ``output_path`` it is written
    there in the format of the extension; with None the PNG is returned in the
    result as ``png_base64`` (the preview)."""
    from matplotlib.figure import Figure  # noqa: PLC0415

    fmt = format_of(output_path) if output_path else "png"
    cmap = _with_blank_colour(resolve_cmap(req.cmap))
    family = check_font(req.font_family)
    color = _overlay_color(cmap, req.overlay_color)
    height, width = data.shape
    # A very large image is decimated for a preview before anything is measured on
    # it: the layout is what a preview shows, and percentiles of 400 Mpixel cost
    # seconds. The axes stay in the full image's pixels (extent).
    shown, extent = data, None
    if preview and max(data.shape) > PREVIEW_DECIMATE_ABOVE:
        step = int(np.ceil(max(data.shape) / PREVIEW_DECIMATE_ABOVE))
        shown, extent = data[::step, ::step], (-0.5, width - 0.5, -0.5, height - 0.5)
    norm = _norm(shown, req)
    notes: list[str] = []

    dpi = float(req.dpi)
    if preview:
        dpi = max(20.0, min(dpi, req.preview_px / max(req.width_in, req.height_in)))
    # The cap is on the raster that is held in memory; a vector file is as large as
    # its content, not its size.
    if fmt == "png" and req.width_in * req.height_in * dpi * dpi > MAX_FIGURE_PIXELS:
        raise ValueError(
            f"{req.width_in:g} x {req.height_in:g} in at {int(dpi)} dpi is more than "
            f"{MAX_FIGURE_PIXELS // 1_000_000} megapixels: lower the size or the dpi.")

    has_celestial = bool(wcs.has_celestial)
    native = ""
    if has_celestial:
        from .regions import native_frame_name  # noqa: PLC0415
        try:
            native = native_frame_name(wcs.celestial)
        except ValueError:
            native = ""
    frame = req.frame if req.frame != "native" else native
    overlay_frame = frame if (has_celestial and frame and frame != native) else ""

    with _LOCK:
        fig = Figure(figsize=(req.width_in, req.height_in), dpi=dpi)
        ax = fig.add_subplot(1, 1, 1, projection=wcs) if has_celestial else fig.add_subplot(1, 1, 1)
        shown_frame = overlay_frame or native
        lon_name, lat_name = {
            "galactic": ("Galactic longitude", "Galactic latitude"),
            "ecliptic": ("Ecliptic longitude", "Ecliptic latitude"),
        }.get(shown_frame, ("Right Ascension", "Declination"))
        if has_celestial:
            ax.set_xlabel(req.xlabel or lon_name, fontsize=req.label_size, fontfamily=family)
            ax.set_ylabel(req.ylabel or lat_name, fontsize=req.label_size, fontfamily=family)
            _set_ticks(ax.coords, native, req)
            for c in (ax.coords[0], ax.coords[1]):
                c.set_ticklabel(size=req.tick_size, family=family)
            if overlay_frame:
                # The native ticks give way to those of the frame asked for, which
                # are drawn on the same edges (astropy's coordinate overlay).
                # An instance, not the name: astropy has no frame called "ecliptic".
                from .regions import sky_frame  # noqa: PLC0415
                ov_axes = ax.get_coords_overlay(sky_frame(overlay_frame))
                for c in (ax.coords[0], ax.coords[1]):
                    c.set_ticks_visible(False)
                    c.set_ticklabel_visible(False)
                    c.set_axislabel("")
                ov_axes[0].set_axislabel(req.xlabel or lon_name, size=req.label_size,
                                         family=family)
                ov_axes[1].set_axislabel(req.ylabel or lat_name, size=req.label_size,
                                         family=family)
                _set_ticks(ov_axes, overlay_frame, req)
                # Longitude along the bottom and latitude up the left, where the
                # native labels were, whatever the angle between the two grids.
                for c, side in ((ov_axes[0], "b"), (ov_axes[1], "l")):
                    c.set_ticklabel(size=req.tick_size, family=family)
                    c.set_ticks_position(side)
                    c.set_ticklabel_position(side)
                    c.set_axislabel_position(side)
                    c.set_axislabel_visibility_rule("always")
                if req.add_grid:
                    ov_axes.grid(color=color, ls=":", alpha=0.5)
            elif req.add_grid:
                ax.coords.grid(True, color=color, ls=":", alpha=0.5)
        else:
            ax.set_xlabel(req.xlabel or "X (pixels)", fontsize=req.label_size, fontfamily=family)
            ax.set_ylabel(req.ylabel or "Y (pixels)", fontsize=req.label_size, fontfamily=family)
            ax.tick_params(labelsize=req.tick_size)
            for lab in (*ax.get_xticklabels(), *ax.get_yticklabels()):
                lab.set_fontfamily(family)

        im = ax.imshow(_masked(shown, norm), origin="lower", cmap=cmap, norm=norm,
                       extent=extent)
        # The limits are the image's from here on: overlays that leave it (a contour
        # of a larger map) must not widen them, and the corners the beam, scale bar
        # and compass are put in are the image's.
        ax.set_xlim(-0.5, width - 0.5)
        ax.set_ylim(-0.5, height - 0.5)
        ax.set_autoscale_on(False)
        if req.title:
            ax.set_title(req.title, fontsize=req.title_size, fontfamily=family)

        drawn_regions = 0
        if req.regions:
            drawn_regions = _draw_overlays(ax, req.regions, wcs, has_celestial, color,
                                           req.label_size, notes)
        if req.beam:
            ov.draw_beam(ax, wcs, beam, req.beam_position, color, notes)
        if req.scale_bar:
            ov.draw_scale_bar(ax, wcs, req.scale_bar_length, req.scale_bar_unit,
                              req.scale_bar_position, color, req.label_size, notes)
        if req.compass:
            ov.draw_compass(ax, wcs, overlay_frame, req.compass_position, color, req.label_size, notes)

        if req.add_colorbar:
            # The gap to the axes holds that side's tick labels and axis label.
            pad = {"right": 0.04 if has_celestial and not overlay_frame else 0.07,
                   "left": 0.16, "bottom": 0.14, "top": 0.04}[req.colorbar_position]
            cbar = fig.colorbar(im, ax=ax, location=req.colorbar_position, fraction=0.046, pad=pad)
            if req.colorbar_position == "top" and req.title:
                # The title goes above the bar, its ticks and its label.
                ax.set_title(req.title, fontsize=req.title_size, fontfamily=family,
                             pad=0.046 * req.height_in * 72
                             + 2.4 * (req.tick_size + req.colorbar_label_size) + 8)
            cbar.set_label(req.colorbar_label or str(header.get("BUNIT", "")).strip() or "value",
                           fontsize=req.colorbar_label_size, fontfamily=family)
            cbar.ax.tick_params(labelsize=req.tick_size)
            if req.colorbar_position in ("bottom", "top"):
                _shorten_horizontal_ticks(cbar, fig, req.tick_size, family)
            for lab in cbar.ax.get_yticklabels() + cbar.ax.get_xticklabels():
                lab.set_fontfamily(family)

        buf = io.BytesIO()
        kwargs: dict[str, Any] = {"dpi": dpi, "format": fmt}
        if req.crop:
            kwargs["bbox_inches"] = "tight"
        import matplotlib  # noqa: PLC0415
        with matplotlib.rc_context(_VECTOR_TEXT_RC):
            if output_path:
                fig.savefig(output_path, **kwargs)
            else:
                fig.savefig(buf, **kwargs)
        # Figure() is not in pyplot's registry, so there is nothing to close.

    out: dict[str, Any] = {"format": fmt, "notes": notes, "regions_drawn": drawn_regions,
                           "width": int(width), "height": int(height), "has_wcs": has_celestial}
    if output_path is None:
        png = buf.getvalue()
        out["png_base64"] = base64.b64encode(png).decode("ascii")
        out["png_width"], out["png_height"] = _png_size(png)
    return out


def _draw_overlays(ax, regions: list[dict[str, Any]], wcs, has_celestial: bool, color: str,
                   font_size: float, notes: list[str]) -> int:
    """Bring sky regions to image pixels (``regions.convert_regions``) and draw all."""
    from . import regions as R  # noqa: PLC0415
    if ov.count_points(regions) > ov.MAX_OVERLAY_POINTS:
        raise ValueError(
            f"The overlays have more than {ov.MAX_OVERLAY_POINTS // 1_000_000} million vertices: "
            "raise the lowest contour level or smooth the image.")
    diags: list = []
    px = R.convert_regions(regions, to_frame="image", wcs=wcs if has_celestial else None,
                           origin=1, diagnostics=diags,
                           no_wcs_reason="the image has no sky coordinates")
    if diags:
        notes.append(f"{len(diags)} overlay(s) could not be placed on the image.")
    n = ov.draw_regions(ax, px, default_color=color, font_size=font_size)
    if n < len(px):
        notes.append(f"{len(px) - n} overlay(s) were not drawn (malformed or unsupported).")
    return n
