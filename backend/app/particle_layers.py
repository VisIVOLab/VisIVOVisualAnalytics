"""Several particle components in one picture (issue #394): gas, dark matter,
stars and black holes of one snapshot, each drawn with its own colour map.

Every component is a *layer*. A particle layer is first reduced to two
floating-point maps by the ray-traced renderer's kernels
(app/particle_raytrace.py), with no absorption so the particles' order does
not matter:

- the column density ``Σ``: each particle's weight (its mass) spread over a
  Gaussian of its smoothing length ``h`` with the peak column ``m / (π h²)``,
  so a map is the mass per unit area along each ray, in the table's units,
  whatever the number of particles that carry it (the rule of issue #457:
  a run at eight times the resolution gives the same map, not a brighter one);
- the mass-weighted mean of a quantity along each ray (a temperature, a
  metallicity), when the layer is coloured by one.

Each map becomes the layer's light through the layer's own fixed window, never
an automatic one per frame: a brightness ``b`` in [0, 1] from ``Σ`` on a log,
linear or asinh scale between the window's ends, then a colour from the
layer's colour map (by ``b`` itself, or by the mean quantity with ``b`` as its
brightness, as a projected map of app/routers/particle_render.py does), times
the layer's intensity. A window left unset is measured once, on the first
frame, and handed back, so the next frames and a movie keep it.

The layers are composed back to front, in the order given:

- an *emission* layer adds its light, ``C ← C + L``: emission layers add up
  in any order (the sum of their pictures is the composite);
- an *absorption* layer is a slab in front of the layers before it, with an
  optical depth proportional to its column, ``τ = opacity · Σ / Σ_top`` (the
  ``opacity`` is the depth at the top of its window), which dims what lies
  behind it and adds its own light as a uniform slab does:
  ``C ← C e^-τ + L (1 - e^-τ) / τ`` (``L`` when ``τ`` → 0). Layers after it
  are in front of it and are not dimmed: listing the stars after the gas keeps
  dense gas from hiding them.

Marker layers (black holes) are drawn last, on top of the composite: one
circle per particle, its radius in pixels growing with the logarithm of its
mass (:func:`marker_radius`), the same at any zoom.
"""
from __future__ import annotations

import math

import numpy as np

from .particle_raytrace import _CUTOFF, _MIN_RADIUS_PX, composite, project

SCALES = ("log", "linear", "asinh")
COMPOSITES = ("emission", "absorption")
#: A hue's colour map is used from this fraction up: the brightness is the
#: column's, and the dark foot of a map would darken low values twice (as
#: the projected maps of app/routers/particle_render.py do). A layer's
#: ``hue_foot`` changes it: 0 for a map that does not start dark.
HUE_FOOT = 0.3
#: An automatic window: the top at this percentile of the lit pixels' column,
#: the bottom at LOW_PERCENTILE, at most WINDOW_DECADES below the top.
TOP_PERCENTILE = 99.9
LOW_PERCENTILE = 1.0
WINDOW_DECADES = 6.0
#: At most this many markers are drawn, the most massive.
MAX_MARKERS = 20_000


# ── Per-layer maps ───────────────────────────────────────────────────────────

def layer_maps(positions: np.ndarray, radius: np.ndarray, mass: np.ndarray,
               value: np.ndarray | None, camera, width: int, height: int) -> np.ndarray:
    """``(height, width, 2)`` float64: the column density of ``mass`` along
    each ray (mass per unit area in the units of ``positions``) and the
    mass-weighted mean of ``value`` (NaN where nothing lies or without a
    value). ``radius``: each particle's Gaussian width ``h``."""
    out = np.zeros((height, width, 2), dtype=np.float64)
    out[..., 1] = np.nan
    n = len(positions)
    if n == 0:
        return out
    radius = np.asarray(radius, dtype=np.float64)
    mass = np.asarray(mass, dtype=np.float64)
    sx, sy, sr, depth = project(positions, radius, camera, width, height)
    keep = (depth > 0) & np.isfinite(sx) & np.isfinite(sy) & (sr > 0) \
        & np.isfinite(mass) & (mass > 0) & np.isfinite(radius) & (radius > 0)
    grown = np.maximum(sr, _MIN_RADIUS_PX).astype(np.float32)
    reach = _CUTOFF * grown
    keep &= (sx + reach >= 0) & (sx - reach < width) & (sy + reach >= 0) & (sy - reach < height)
    idx = np.flatnonzero(keep).astype(np.int64)
    if not idx.size:
        return out
    peak = np.zeros(n, dtype=np.float64)
    # Renormalised for the kernel cut at _CUTOFF h, so the map holds all the
    # mass (as the projected maps of issue #393 do).
    peak[idx] = mass[idx] / (math.pi * radius[idx] ** 2 * -math.expm1(-_CUTOFF ** 2))
    unit = float(peak[idx].max())
    emission = np.zeros((n, 3), dtype=np.float32)
    emission[:, 0] = peak / unit
    q_unit = 1.0
    if value is not None:
        raw = np.asarray(value, dtype=np.float64)
        known = np.isfinite(raw)
        v = np.where(known, raw, 0.0)
        q_unit = float(np.abs(v[idx]).max()) or 1.0
        # The mean's own weight: a particle without a value does not pull it to 0.
        emission[:, 1] = (peak / unit) * (v / q_unit)
        emission[:, 2] = (peak / unit) * known
    weight = ((sr / grown) ** 2).astype(np.float32)
    light, _ = composite(sx, sy, grown, weight, emission, None, idx, width, height)
    out[..., 0] = light[..., 0].astype(np.float64) * unit
    if value is not None:
        with np.errstate(divide="ignore", invalid="ignore"):
            out[..., 1] = np.where(light[..., 2] > 0,
                                   light[..., 1].astype(np.float64) / light[..., 2] * q_unit,
                                   np.nan)
    return out


# ── A layer's light ──────────────────────────────────────────────────────────

def auto_window(column: np.ndarray, scale: str) -> tuple[float, float]:
    """A window for ``column`` measured on its lit pixels: the top at their
    TOP_PERCENTILE-th percentile; the bottom at the LOW_PERCENTILE-th (log),
    0 (linear) or their median (asinh, where the curve turns from linear to
    logarithmic), at most WINDOW_DECADES below the top."""
    lit = column[np.isfinite(column) & (column > 0)]
    if not lit.size:
        return (1.0, 10.0) if scale != "linear" else (0.0, 1.0)
    hi = float(np.percentile(lit, TOP_PERCENTILE))
    if scale == "linear":
        return 0.0, hi if hi > 0 else 1.0
    lo = float(np.percentile(lit, LOW_PERCENTILE if scale == "log" else 50.0))
    lo = max(lo, hi * 10.0 ** -WINDOW_DECADES)
    if not hi > lo:
        lo = hi / 10.0
    return lo, hi


def brightness(column: np.ndarray, window: tuple[float, float], scale: str) -> np.ndarray:
    """``b`` in [0, 1] of the column through ``window`` on ``scale``: 0 at or
    below the bottom on a log or linear scale, 1 at the top."""
    if scale not in SCALES:
        raise ValueError(f"Unknown scale {scale!r}: one of {SCALES}.")
    lo, hi = float(window[0]), float(window[1])
    if not hi > lo or (scale != "linear" and not lo > 0):
        raise ValueError(f"The window {[lo, hi]} does not suit a {scale} scale.")
    c = np.nan_to_num(np.asarray(column, dtype=np.float64), nan=0.0)
    with np.errstate(divide="ignore", invalid="ignore"):
        if scale == "log":
            b = np.where(c > 0, np.log10(np.maximum(c, 1e-300) / lo) / math.log10(hi / lo), 0.0)
        elif scale == "linear":
            b = (c - lo) / (hi - lo)
        else:
            b = np.arcsinh(c / lo) / math.asinh(hi / lo)
    return np.clip(np.nan_to_num(b, nan=0.0), 0.0, 1.0)


def quantity_window(mean: np.ndarray, log: bool) -> tuple[float, float]:
    """The 1st to 99th percentile of a mean quantity where it is defined (of
    its positive values on a log scale)."""
    v = mean[np.isfinite(mean)]
    if log:
        v = v[v > 0]
    if not v.size:
        return (1.0, 10.0) if log else (0.0, 1.0)
    lo, hi = float(np.percentile(v, 1.0)), float(np.percentile(v, 99.0))
    if not hi > lo:
        hi = lo * 10.0 if log else lo + 1.0
    return lo, hi


def quantity_fraction(mean: np.ndarray, window: tuple[float, float], log: bool) -> np.ndarray:
    lo, hi = float(window[0]), float(window[1])
    if not hi > lo or (log and not lo > 0):
        raise ValueError(f"The quantity window {[lo, hi]} does not suit "
                         f"a {'log' if log else 'linear'} scale.")
    m = np.asarray(mean, dtype=np.float64)
    with np.errstate(divide="ignore", invalid="ignore"):
        if log:
            f = np.log10(np.maximum(m, 1e-300) / lo) / math.log10(hi / lo)
        else:
            f = (m - lo) / (hi - lo)
    return np.clip(np.nan_to_num(f, nan=0.0), 0.0, 1.0)


def colour_table(colormap) -> np.ndarray:
    """(256, 3) RGB in [0, 1] of a colour map: a name the point viewer knows
    (app/render_session/_common.py) or control points ``[[t, r, g, b], …]``
    with ``t`` in [0, 1], interpolated linearly."""
    t = np.linspace(0.0, 1.0, 256)
    if isinstance(colormap, str):
        from .render_session._common import _build_color_tf   # noqa: PLC0415
        ctf = _build_color_tf(colormap, 0.0, 1.0)
        return np.array([ctf.GetColor(float(x)) for x in t], dtype=np.float64)
    points = np.asarray(colormap, dtype=np.float64)
    if points.ndim != 2 or points.shape[1] != 4 or len(points) < 2:
        raise ValueError("A colour map's points are [[t, r, g, b], …], at least two.")
    points = points[np.argsort(points[:, 0], kind="stable")]
    return np.stack([np.interp(t, points[:, 0], np.clip(points[:, k], 0.0, 1.0))
                     for k in (1, 2, 3)], axis=1)


def _lookup(table: np.ndarray, fraction: np.ndarray) -> np.ndarray:
    index = np.clip(np.rint(fraction * (len(table) - 1)), 0, len(table) - 1).astype(np.int64)
    return table[index]


def layer_light(maps: np.ndarray, table: np.ndarray, window, scale: str, intensity: float,
                quantity_window_: tuple[float, float] | None = None,
                quantity_log: bool = False, hue_foot: float = HUE_FOOT) -> np.ndarray:
    """``(height, width, 3)``: a layer's light from its maps. Without a
    quantity window the colour follows the brightness and nothing is drawn
    where it is 0; with one the hue follows the mean quantity (from
    ``hue_foot`` of the map up) and the brightness scales it."""
    b = brightness(maps[..., 0], window, scale)
    if quantity_window_ is None:
        rgb = _lookup(table, b) * (b > 0)[..., None]
    else:
        f = hue_foot + (1.0 - hue_foot) * quantity_fraction(maps[..., 1], quantity_window_,
                                                             quantity_log)
        rgb = _lookup(table, f) * b[..., None]
    return rgb * float(intensity)


def optical_depth(column: np.ndarray, window, opacity: float) -> np.ndarray:
    """An absorption layer's optical depth: ``opacity`` at the top of its
    window, proportional to the column."""
    top = float(window[1])
    if not top > 0 or opacity <= 0:
        return np.zeros(column.shape, dtype=np.float64)
    return np.nan_to_num(np.asarray(column, dtype=np.float64), nan=0.0).clip(0.0) \
        * (float(opacity) / top)


def compose(layers: list[dict], shape: tuple[int, int]) -> np.ndarray:
    """``(height, width, 3)`` linear composite of ``layers`` back to front:
    each ``{"light": (h, w, 3), "composite": "emission" | "absorption",
    "tau": (h, w) for an absorption layer}`` (see the module doc)."""
    out = np.zeros((*shape, 3), dtype=np.float64)
    for layer in layers:
        light = layer["light"]
        if layer["composite"] == "emission":
            out += light
            continue
        if layer["composite"] != "absorption":
            raise ValueError(f"Unknown composite {layer['composite']!r}: one of {COMPOSITES}.")
        tau = layer["tau"]
        lost = -np.expm1(-tau)
        with np.errstate(divide="ignore", invalid="ignore"):
            slab = np.where(tau > 1e-12, lost / tau, 1.0)
        out = out * (1.0 - lost)[..., None] + light * slab[..., None]
    return out


def finish(image: np.ndarray, gamma: float = 1.0) -> np.ndarray:
    """An 8-bit picture of a composite: a pixel above 1 in a channel is
    scaled down whole (its hue kept), then ``gamma``."""
    out = np.nan_to_num(np.asarray(image, dtype=np.float64), nan=0.0).clip(0.0)
    peak = out.max(axis=-1, keepdims=True)
    out = np.where(peak > 1.0, out / np.maximum(peak, 1e-300), out)
    if gamma and gamma != 1.0:
        out = out ** (1.0 / gamma)
    return (out * 255.0 + 0.5).astype(np.uint8)


# ── Markers ──────────────────────────────────────────────────────────────────

def mass_window(mass: np.ndarray) -> tuple[float, float]:
    """The masses' smallest and largest positive values (both 1 for none)."""
    m = np.asarray(mass, dtype=np.float64)
    m = m[np.isfinite(m) & (m > 0)]
    return (float(m.min()), float(m.max())) if m.size else (1.0, 1.0)


def marker_radius(mass: np.ndarray, window: tuple[float, float], min_px: float,
                  max_px: float) -> np.ndarray:
    """A marker's radius in pixels: ``min_px`` at the bottom of the mass
    window, ``max_px`` at its top, linear in log10 of the mass between them
    and held at the ends outside; ``max_px`` for every marker when the
    window is a single mass. The same at any zoom and in any frame."""
    m = np.asarray(mass, dtype=np.float64)
    lo, hi = float(window[0]), float(window[1])
    if not (lo > 0 and hi > lo):
        return np.full(m.shape, float(max_px))
    with np.errstate(divide="ignore", invalid="ignore"):
        t = np.log10(np.maximum(m, 1e-300) / lo) / math.log10(hi / lo)
    t = np.clip(np.nan_to_num(t, nan=0.0), 0.0, 1.0)
    return float(min_px) + (float(max_px) - float(min_px)) * t


def place_markers(positions: np.ndarray, mass: np.ndarray, camera, width: int, height: int,
                  window, min_px: float, max_px: float) -> np.ndarray:
    """``(n, 4)`` markers in view, ``[x, y, radius, mass]`` in pixels (origin
    top left), the most massive MAX_MARKERS, largest first."""
    if not len(positions):
        return np.zeros((0, 4))
    sx, sy, _, depth = project(positions, np.zeros(len(positions)), camera, width, height)
    r = marker_radius(mass, window, min_px, max_px)
    m = np.asarray(mass, dtype=np.float64)
    keep = (depth > 0) & np.isfinite(sx) & np.isfinite(sy) & np.isfinite(m) \
        & (sx + r >= 0) & (sx - r < width) & (sy + r >= 0) & (sy - r < height)
    out = np.stack([sx[keep], sy[keep], r[keep], m[keep]], axis=1).astype(np.float64)
    out = out[np.argsort(-out[:, 3], kind="stable")][:MAX_MARKERS]
    return out[np.argsort(-out[:, 2], kind="stable")]


def draw_markers(rgb: np.ndarray, markers: np.ndarray, colour, style: str = "ring",
                 opacity: float = 1.0) -> np.ndarray:
    """``rgb`` (8-bit) with ``markers`` (:func:`place_markers`) drawn on it,
    anti-aliased: a ``ring`` (a line a quarter of its radius wide, at least
    1.5 px) or a filled ``disc``."""
    out = rgb.astype(np.float64)
    h, w = out.shape[:2]
    c = np.asarray(colour, dtype=np.float64)[:3] * 255.0
    for x, y, r, _ in markers:
        half = max(1.5, r / 4.0) / 2.0 if style == "ring" else 0.0
        reach = r + half + 1.0
        x0, x1 = max(0, int(math.floor(x - reach))), min(w, int(math.ceil(x + reach)) + 1)
        y0, y1 = max(0, int(math.floor(y - reach))), min(h, int(math.ceil(y + reach)) + 1)
        if x1 <= x0 or y1 <= y0:
            continue
        gy, gx = np.mgrid[y0:y1, x0:x1]
        d = np.hypot(gx + 0.5 - x, gy + 0.5 - y)
        if style == "ring":
            a = np.clip(half + 0.5 - np.abs(d - r), 0.0, 1.0)
        else:
            a = np.clip(r + 0.5 - d, 0.0, 1.0)
        a = (a * float(opacity))[..., None]
        out[y0:y1, x0:x1] = out[y0:y1, x0:x1] * (1.0 - a) + c * a
    return (out + 0.5).clip(0, 255).astype(np.uint8)


# ── Presets ──────────────────────────────────────────────────────────────────

#: Common looks of a simulation's components, as data. ``layer``: the fields
#: of a request's layer they set (app/routers/particle_layers.py); ``gadget_type``:
#: the particle type they are meant for (a client's suggestion, not a rule).
PRESETS: tuple[dict, ...] = (
    {"name": "gas-density-hot", "label": "Gas density (hot)", "gadget_type": 0,
     "layer": {"colormap": "afmhot", "scale": "log", "composite": "emission",
               "intensity": 1.0}},
    {"name": "gas-temperature", "label": "Gas, coloured by temperature", "gadget_type": 0,
     "layer": {"colormap": "RdYlBu_r", "scale": "log", "composite": "emission",
               "intensity": 1.0, "quantity_field": "temperature", "quantity_log": True,
               "hue_foot": 0.0}},
    # Only the cool gas: hot gas (a cluster's ICM) holds no dust to absorb.
    {"name": "gas-dust", "label": "Cool gas absorbing what lies behind", "gadget_type": 0,
     "layer": {"filters": [{"field": "temperature", "op": "<", "value": 1.0e5}],
"colormap": [[0.0, 0.0, 0.0, 0.0], [1.0, 0.55, 0.3, 0.15]], "scale": "log",
               "composite": "absorption", "opacity": 1.5, "intensity": 0.4}},
    {"name": "dark-matter-blue", "label": "Dark matter (grey-blue)", "gadget_type": 1,
     "layer": {"colormap": [[0.0, 0.0, 0.0, 0.0], [0.5, 0.18, 0.26, 0.5],
                            [1.0, 0.82, 0.88, 1.0]],
               "scale": "log", "composite": "emission", "intensity": 0.8}},
    {"name": "dark-matter-grey", "label": "Dark matter (grey)", "gadget_type": 1,
     "layer": {"colormap": "Gray", "scale": "log", "composite": "emission",
               "intensity": 0.7}},
    {"name": "stars-white-yellow", "label": "Stars (white-yellow)", "gadget_type": 4,
     "layer": {"colormap": [[0.0, 0.0, 0.0, 0.0], [0.5, 0.75, 0.6, 0.25],
                            [1.0, 1.0, 0.98, 0.9]],
               "scale": "log", "composite": "emission", "intensity": 1.0}},
    {"name": "black-holes", "label": "Black holes (markers by mass)", "gadget_type": 5,
     "layer": {"kind": "markers",
               "marker": {"min_px": 3.0, "max_px": 16.0, "style": "ring",
                          "colour": [0.35, 1.0, 1.0], "opacity": 1.0}}},
)
