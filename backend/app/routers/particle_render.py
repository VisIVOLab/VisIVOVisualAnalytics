"""Ray-traced particle rendering as a task (issue #101): stills and movie
frames of a table of points, rendered by app/particle_raytrace.py on any
backend (it needs no GPU and no server-side rendering).

A task renders one frame per camera it is given. The frames are 8-bit PNGs
in the workspace; a still keeps its light (the floating-point image before
the tone curve) too, so it can be graded again (``/tone``) or saved as FITS
without rendering it again. A movie's exposure is fixed on its first frame,
so its brightness does not flicker from frame to frame.
"""
from __future__ import annotations

import asyncio
import contextlib
import io
import logging
import math
import shutil
import json
import threading
import time
from collections import OrderedDict
from pathlib import Path
from typing import Literal

import numpy as np
from fastapi import APIRouter, Depends, HTTPException
from fastapi.responses import FileResponse, Response
from pydantic import BaseModel, Field

from ..auth import verify_token
from ..dependencies import _require_dataset, get_exports_dir
from ..product_cache import dataset_identity
from ..response_headers import (X_EXPOSURE, X_PARTICLE_COUNT, X_PARTICLE_FIELDS,
                                X_PARTICLE_ORIGIN, X_PARTICLE_SCALE, X_PARTICLE_STEP,
                                X_RENDER_MS)
from ..schemas import TaskCreateResponse
from ..sessions import Session, get_session
from ..tasks import TASKS

router = APIRouter(tags=["particle-render"])
logger = logging.getLogger("visivo.backend")
_auth = Depends(verify_token)

_SOLID = (0.8, 0.85, 1.0)   # the point session's colour without a field


class RaytraceCamera(BaseModel):
    position: tuple[float, float, float]
    focal_point: tuple[float, float, float]
    view_up: tuple[float, float, float]
    view_angle: float = Field(30.0, gt=0.0, lt=180.0)
    parallel: bool = False
    parallel_scale: float = Field(1.0, gt=0.0)


class RaytraceColour(BaseModel):
    """The point viewer's colours: a column through the client's own colour
    function (``points``: ``[[x, r, g, b], …]``, as the point session takes
    it) or a named map; no field, one colour."""
    field: str | None = None
    points: list[list[float]] | None = None
    lut: str = "Inferno"
    min: float | None = None
    max: float | None = None
    log: bool = False


class RaytraceTone(BaseModel):
    exposure: float | None = Field(None, gt=0.0)   # None: automatic
    ev: float = Field(0.0, ge=-20.0, le=20.0)      # stops over that exposure
    # A projected map: the decades of density its brightness spans (None: from
    # the 1st to the 99.9th percentile of the lit pixels).
    decades: float | None = Field(None, gt=0.0, le=20.0)
    curve: Literal["asinh", "log", "exponential", "linear"] = "asinh"
    stretch: float = Field(10.0, gt=0.0, le=1.0e6)
    gamma: float = Field(2.2, gt=0.0, le=10.0)
    saturation: float = Field(1.0, ge=0.0, le=4.0)


class ParticleRenderRequest(BaseModel):
    dataset_id: str
    # The position columns; empty: the table's own (a sky catalogue's are the
    # computed @x/@y/@z, placed by ``sky``).
    x: str | None = None
    y: str | None = None
    z: str | None = None
    sky: dict | None = None       # {"frame", "distance_mode", "cosmology"}
    filters: list[dict] = []
    colour: RaytraceColour = RaytraceColour()
    # Each particle's size: a column (a smoothing length), or estimated from
    # its ``neighbours`` nearest particles; times ``radius_scale``.
    radius_field: str | None = None
    radius_scale: float = Field(1.0, gt=0.0, le=1.0e3)
    neighbours: int = Field(32, ge=1, le=512)
    # Its light: a column (``intensity_log10``: the column holds log10 of
    # it), or the same for every particle; a ``density`` or an amount
    # ``per_particle`` (app/particle_raytrace.py).
    intensity_field: str | None = None
    intensity_log10: bool = False
    # Contrast: the light raised to this power. Above 1 the faint, diffuse
    # particles fade and the dense structure stands out (2: a gas's density
    # squared, as an emissivity ∝ ρ² would show it).
    intensity_power: float = Field(1.0, gt=0.0, le=8.0)
    quantity: Literal["density", "per_particle"] = "per_particle"
    absorption: float = Field(0.0, ge=0.0, le=50.0)
    # ``light``: a 3-D render, the particles emitting and absorbing light.
    # ``projected``: a map of the projected density (the light, as a mass),
    # as cosmological simulations are shown: no perspective, within ``slab``
    # of the focal plane (data units; None: all), coloured by ``colour_by``.
    mode: Literal["light", "projected"] = "light"
    slab: float | None = Field(None, gt=0.0)
    # The hue of a projected map: the density itself (through ``colour.lut``),
    # the mean of the colour column (through the view's colours), or the
    # local velocity dispersion (through ``colour.lut``, from ``velocity``,
    # or the columns named VX/VY/VZ).
    colour_by: Literal["density", "colour", "velocity_dispersion"] = "density"
    velocity: tuple[str, str, str] | None = None
    width: int = Field(1920, ge=16, le=8192)
    height: int = Field(1080, ge=16, le=8192)
    cameras: list[RaytraceCamera] = Field(..., min_length=1, max_length=7200)
    tone: RaytraceTone = RaytraceTone()


# The stop flags of the renders running: a frame loop on a worker thread
# checks its own between frames (cancelling the asyncio task does not reach it).
_STOPS: dict[str, threading.Event] = {}


def _render_dir(task_id: str) -> Path:
    # Hidden: the workspace listings skip it; the client saves what it keeps.
    return get_exports_dir() / ".renders" / task_id


def _colours(columns, rows: np.ndarray, colour: RaytraceColour) -> np.ndarray:
    """(N, 3) RGB in [0, 1], through the lookup table the point session uses."""
    from ..render_session._common import _build_color_tf, _parse_tf_points  # noqa: PLC0415
    from ..render_session.points import _sampled_lut  # noqa: PLC0415
    if not colour.field:
        return np.tile(np.array(_SOLID, dtype=np.float32), (len(rows), 1))
    if colour.field not in columns:
        raise ValueError(f"Unknown colour field: {colour.field!r}")
    values = np.asarray(columns[colour.field][rows], dtype=np.float64)
    finite = values[np.isfinite(values)]
    lo, hi = (float(finite.min()), float(finite.max())) if finite.size else (0.0, 1.0)
    if colour.log:
        # A log scale starts at the smallest positive value when the column
        # reaches zero (a temperature, a density with empty cells).
        positive = finite[finite > 0]
        lo = float(positive.min()) if positive.size else lo
    cmin = colour.min if colour.min is not None else lo
    cmax = colour.max if colour.max is not None else hi
    if not cmax > cmin:
        cmin, cmax = lo, (hi if hi > lo else lo + 1.0)
    log = bool(colour.log) and cmin > 0
    if colour.points:
        # The client's own colours, placed at data values (on its log scale
        # when it has one).
        import vtk  # noqa: PLC0415
        ctf = vtk.vtkColorTransferFunction()
        for x, r, g, b in _parse_tf_points(colour.points, 4):
            ctf.AddRGBPoint(x, r, g, b)
        if log:
            ctf.SetScale(1)
        lut = _sampled_lut(ctf, cmin, cmax, log)
    elif log:
        # A named map runs evenly over its range: on a log scale, over the
        # logarithms. Values at or below zero take its lowest colour.
        with np.errstate(divide="ignore", invalid="ignore"):
            values = np.log10(np.maximum(values, cmin))
        cmin, cmax = math.log10(cmin), math.log10(cmax)
        lut = _sampled_lut(_build_color_tf(colour.lut, cmin, cmax), cmin, cmax, False)
    else:
        lut = _sampled_lut(_build_color_tf(colour.lut, cmin, cmax), cmin, cmax, False)
    from vtk.util.numpy_support import numpy_to_vtk, vtk_to_numpy  # noqa: PLC0415
    rgba = vtk_to_numpy(lut.MapScalars(numpy_to_vtk(values.astype(np.float32), deep=True), 0, -1))
    return (rgba[:, :3].astype(np.float32) / 255.0)


def _prepare(entry: dict, req: ParticleRenderRequest) -> dict:
    """Positions, sizes, colours and light of the particles drawn."""
    from ..particle_raytrace import smoothing_lengths  # noqa: PLC0415
    from ..point_sample import place_rows  # noqa: PLC0415
    from ..point_table import filter_mask, open_point_table  # noqa: PLC0415
    from .render import point_table_spec  # noqa: PLC0415
    spec = point_table_spec(entry)
    if spec is None:
        raise ValueError("Ray-traced rendering needs a table of points.")
    table = open_point_table(spec)
    if req.sky and table.sky is not None:
        table.set_sky_positions(str(req.sky.get("frame", "fk5")),
                                str(req.sky.get("distance_mode", "comoving")),
                                str(req.sky.get("cosmology", "Planck18")))
    axes = (req.x or table.axes[0], req.y or table.axes[1], req.z or table.axes[2])
    for name in axes:
        if name not in table.columns:
            raise ValueError(f"Unknown position field: {name!r}")
    rows = np.arange(table.num_rows, dtype=np.int64)
    if req.filters:
        rows = rows[filter_mask(table.columns, req.filters, rows)]
    # Float32 coordinates local to the rows and their map back to the
    # table's, in float64 (point_sample.place_rows, as the remote point
    # session and the VR page read them): a float32 cast of the positions
    # merged 1e9, 1e9 + 1 and 1e9 + 2 into one particle. Still 12 bytes a
    # particle; a row without a finite position within float32's range is
    # left out, as before. One scale on every axis (uniform_scale), so the
    # kernels stay round: 1 unless an axis's offsets could not keep their
    # precision (a span beyond float32's range or below 1e-30).
    from ..render_session.points import uniform_scale  # noqa: PLC0415
    placed = uniform_scale(place_rows(table.columns, axes, rows))
    rows, pos = placed.rows, placed.xyz
    origin, scale = placed.origin.copy(), float(placed.scale[0])
    del placed
    if req.intensity_field:
        if req.intensity_field not in table.columns:
            raise ValueError(f"Unknown intensity field: {req.intensity_field!r}")
        light = np.asarray(table.columns[req.intensity_field][rows], dtype=np.float64)
        if req.intensity_log10:
            with np.errstate(over="ignore"):
                light = 10.0 ** (light - np.nanmax(light[np.isfinite(light)], initial=0.0))
    else:
        light = np.ones(len(rows))
    keep = np.isfinite(light) & (light > 0)
    rows, pos, light = rows[keep], pos[keep], light[keep]
    if not len(rows):
        raise ValueError("No particle to draw: no row has a position and a positive intensity.")
    light = light / np.percentile(light, 99.0)
    if req.intensity_power != 1.0:
        light = light ** req.intensity_power
    light = light.astype(np.float32)
    if req.radius_field:
        if req.radius_field not in table.columns:
            raise ValueError(f"Unknown radius field: {req.radius_field!r}")
        radius = np.asarray(table.columns[req.radius_field][rows], dtype=np.float32)
        positive = np.isfinite(radius) & (radius > 0)
        if not positive.any():
            raise ValueError(f"The radius field {req.radius_field!r} has no positive value.")
        radius = np.where(positive, radius, radius[positive].min())
        if scale != 1.0:   # the table's units, into the local frame's
            radius = radius / np.float32(scale)
    else:
        radius = smoothing_lengths(pos, req.neighbours)
    radius = (radius * req.radius_scale).astype(np.float32)
    # Positions and sizes in the local frame; ``frame`` (origin, scale) maps
    # it to the table's coordinates, table = origin + scale * local, and
    # _local_camera moves the camera into it.
    particles = {"positions": pos, "radius": radius, "intensity": light,
                 "frame": (origin, scale)}
    if req.mode == "light":
        particles["colour"] = _colours(table.columns, rows, req.colour)
    elif req.colour_by == "colour" and req.colour.field:
        if req.colour.field not in table.columns:
            raise ValueError(f"Unknown colour field: {req.colour.field!r}")
        particles["value"] = np.asarray(table.columns[req.colour.field][rows], dtype=np.float32)
    elif req.colour_by == "velocity_dispersion":
        from ..particle_raytrace import velocity_dispersion  # noqa: PLC0415
        names = req.velocity or _velocity_columns(table.columns)
        if names is None or any(n not in table.columns for n in names):
            raise ValueError("No velocity columns (VX, VY, VZ) for the velocity dispersion.")
        vel = np.stack([np.asarray(table.columns[n][rows], dtype=np.float32) for n in names],
                       axis=1)
        particles["value"] = velocity_dispersion(pos, np.nan_to_num(vel), req.neighbours)
    return particles


def _velocity_columns(columns) -> tuple[str, str, str] | None:
    """The velocity columns by their usual names (VX, VEL_X, V_X, VELX…)."""
    upper = {str(n).upper().replace("_", ""): n for n in columns}
    for stem in ("V", "VEL", "VELOCITY"):
        names = [upper.get(f"{stem}{a}") for a in "XYZ"]
        if all(names):
            return tuple(names)
    return None


def _save_png(rgb: np.ndarray, path: Path) -> None:
    from PIL import Image  # noqa: PLC0415
    Image.fromarray(rgb).save(path, format="PNG", compress_level=3)


def _projected_image(maps: np.ndarray, req: ParticleRenderRequest,
                     tone: RaytraceTone) -> np.ndarray:
    """A projected map's picture: brightness from the logarithm of the density,
    the hue from the density or from the mean quantity (a 2-D colour table)."""
    from ..particle_raytrace import density_brightness  # noqa: PLC0415
    density, mean = maps[..., 0], maps[..., 1]
    b = density_brightness(density, ev=tone.ev, decades=tone.decades)
    flat = np.arange(b.size)
    # The view's colours with no colour column (one colour): by the density.
    if (req.colour_by == "density" or not np.isfinite(mean).any()
            or (req.colour_by == "colour" and not req.colour.field)):
        rgb = _colours({"b": b.ravel()}, flat, RaytraceColour(field="b", lut=req.colour.lut,
                                                              min=0.0, max=1.0))
        rgb = rgb.reshape(*b.shape, 3) * (b > 0)[..., None]
    else:
        colour = req.colour
        values = np.nan_to_num(mean.ravel(), nan=0.0)
        if req.colour_by == "velocity_dispersion" or not colour.points:
            # A named map, over the spread of the mean on the lit pixels. Only
            # its coloured part (from 30 % up): the brightness is the density's,
            # and the map's dark foot would darken the low values twice.
            lit = mean[np.isfinite(mean)]
            log = colour.log and (lit > 0).any()
            scale = np.log10 if log else (lambda x: x)
            ref = scale(lit[lit > 0]) if log else lit
            lo, hi = float(np.percentile(ref, 1.0)), float(np.percentile(ref, 99.0))
            with np.errstate(divide="ignore", invalid="ignore"):
                frac = (scale(np.maximum(values, 1e-300) if log else values) - lo) / ((hi - lo) or 1.0)
            values = 0.3 + 0.7 * np.clip(np.nan_to_num(frac), 0.0, 1.0)
            colour = RaytraceColour(field="m", lut=colour.lut, min=0.0, max=1.0)
        else:
            colour = colour.model_copy(update={"field": "m"})
        hue = _colours({"m": values}, flat, colour)
        rgb = hue.reshape(*b.shape, 3) * b[..., None]
    return (np.clip(rgb, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)


def _local_camera(camera, particles: dict):
    """The camera (in the table's coordinates) in the particles' local frame,
    and that frame's scale: placed in float64, so the frame is drawn from
    float32 offsets however far from 0 the table lies (``frame``, _prepare).
    A shift and one scale on every axis: the view is the same picture."""
    from ..particle_raytrace import Camera  # noqa: PLC0415
    origin, scale = particles.get("frame", ((0.0, 0.0, 0.0), 1.0))
    origin = np.asarray(origin, dtype=np.float64)
    c = camera.model_dump()
    for key in ("position", "focal_point"):
        c[key] = tuple(float(v) for v in (np.asarray(c[key], dtype=np.float64) - origin) / scale)
    c["parallel_scale"] = c["parallel_scale"] / scale
    return Camera(**c), float(scale)


def _frame(particles: dict, req: ParticleRenderRequest, camera, width: int, height: int,
           tone: RaytraceTone, exposure: float | None):
    """One frame: (8-bit picture, the data it was made from, the exposure
    before ``ev``). The data: the light (a 3-D render) or the density and the
    mean quantity (a projected map)."""
    from ..particle_raytrace import projected_maps, render, tone_map  # noqa: PLC0415
    cam, scale = _local_camera(camera, particles)
    if req.mode == "projected":
        maps = projected_maps(particles["positions"], particles["radius"],
                              particles["intensity"], particles.get("value"), cam, width, height,
                              slab=req.slab / scale if req.slab else req.slab)
        return _projected_image(maps, req, tone), maps, 1.0
    light = render(particles["positions"], particles["radius"], particles["colour"],
                   particles["intensity"], cam, width, height,
                   absorption=req.absorption, quantity=req.quantity)
    if exposure is None:
        exposure = _auto_exposure(light)
    params = tone.model_dump(exclude={"exposure", "ev", "decades"})
    return (tone_map(light, exposure=exposure * 2.0 ** tone.ev, **params), light,
            float(exposure))


def _run(task_id: str, entry: dict, req: ParticleRenderRequest, stop: threading.Event) -> dict:
    return run_frames(
        task_id, req, stop, lambda: _prepare(entry, req),
        lambda particles, cam, exposure: _frame(particles, req, cam, req.width, req.height,
                                                req.tone, exposure),
        lambda particles: {"particles": int(len(particles["positions"]))})


def run_frames(task_id: str, req, stop: threading.Event, prepare, frame, summary) -> dict:
    """A render task's work, on a worker thread: ``prepare()`` the data once,
    then each camera's ``frame(data, camera, exposure)`` (a picture, the data
    it was made from, the exposure before ``ev``) saved as a PNG; a still
    keeps its data and request, to be graded again. Shared by the particle
    and the volume renders (app/routers/volume_render.py)."""
    t0 = time.time()
    data = prepare()
    if stop.is_set():   # cancelled or deleted while the data was prepared
        raise asyncio.CancelledError
    out = _render_dir(task_id)
    out.mkdir(parents=True, exist_ok=True)
    exposure = req.tone.exposure
    still = len(req.cameras) == 1
    try:
        for i, cam in enumerate(req.cameras):
            if stop.is_set():
                raise asyncio.CancelledError
            # The first frame's automatic exposure, for every frame.
            rgb, light, exposure = frame(data, cam, exposure)
            _save_png(rgb, out / f"frame_{i:05d}.png")
            if still:
                np.save(out / "light.npy", light)
                (out / "request.json").write_text(req.model_dump_json())
            TASKS.update(task_id,
                         progress=min(0.99, 0.05 + 0.95 * (i + 1) / len(req.cameras)))
        # Stopped during the last frame: not published as done.
        if stop.is_set():
            raise asyncio.CancelledError
    except BaseException:
        # Cancelled, deleted (which may have removed the folder under the
        # frame being written) or failed: nobody will fetch them.
        shutil.rmtree(out, ignore_errors=True)
        raise
    return {"frames": len(req.cameras), "width": req.width, "height": req.height,
            **summary(data), "exposure": float(exposure),
            "seconds": round(time.time() - t0, 2), "hdr": still}


def _auto_exposure(light: np.ndarray) -> float:
    lum = light @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    lit = lum[lum > 0]
    ref = float(np.percentile(lit, 99.5)) if lit.size else 0.0
    return 1.0 / ref if ref > 0 else 1.0


# ── The interactive preview ─────────────────────────────────────────────

# The particles of the last few views previewed (positions, sizes, colours,
# light), per client: a drag changes only the camera, so a frame is then only
# projected and composited. Keyed by everything that shapes them.
_PREPARED: OrderedDict[str, dict] = OrderedDict()
_PREPARED_KEEP = 2
# One preview at a time: the asyncio lock queues the requests, the thread
# lock serialises the work itself, which runs on after its request is
# abandoned (a client gone): the next waits for it, and _PREPARED is never
# changed by two threads.
_PREVIEW_LOCK = asyncio.Lock()
_PREVIEW_WORK = threading.Lock()
_PARTICLE_FIELDS = ("dataset_id", "x", "y", "z", "sky", "filters", "colour", "radius_field",
                    "radius_scale", "neighbours", "intensity_field", "intensity_log10",
                    "intensity_power", "mode", "colour_by", "velocity")


def _prepared(entry: dict, req: ParticleRenderRequest, token: str) -> dict:
    fields = req.model_dump(include=set(_PARTICLE_FIELDS))
    # The files' version too: a table rewritten under the same dataset id is
    # prepared again rather than drawn from the particles of the old one.
    key = token + dataset_identity(entry) + json.dumps(fields, sort_keys=True, default=str)
    if key in _PREPARED:
        _PREPARED.move_to_end(key)
        return _PREPARED[key]
    particles = _prepare(entry, req)
    _PREPARED[key] = particles
    while len(_PREPARED) > _PREPARED_KEEP:
        _PREPARED.popitem(last=False)
    return particles


class ParticlePreviewRequest(ParticleRenderRequest):
    width: int = Field(640, ge=16, le=2048)
    height: int = Field(360, ge=16, le=2048)
    cameras: list[RaytraceCamera] = Field(..., min_length=1, max_length=1)
    # A draft while the view turns: at most this many particles, an even
    # sample, each carrying the light of those it stands for.
    max_particles: int | None = Field(None, ge=1000)


def _sampled(particles: dict, limit: int | None) -> dict:
    n = len(particles["positions"])
    if not limit or n <= limit:
        return particles
    step = -(-n // limit)
    # ``frame`` is the particles' coordinate frame, not one value a particle.
    out = {k: v if k == "frame" else v[::step] for k, v in particles.items()}
    out["intensity"] = out["intensity"] * np.float32(step)
    return out


@router.post("/v1/particle_render/preview")
async def particle_render_preview(request: ParticlePreviewRequest, token: str = _auth,
                                  session: Session = Depends(get_session)) -> Response:
    """One frame of the view, small enough to follow a drag: a JPEG, with its
    exposure before ``ev`` (``X-Visivo-Exposure``: the one it found when it was
    given none), so the next frames keep it and do not flicker."""
    entry = _require_dataset(session, request.dataset_id)
    from .render import point_table_spec  # noqa: PLC0415
    if point_table_spec(entry) is None:
        raise HTTPException(422, "Ray-traced rendering needs a table of points.")

    def frame() -> tuple[bytes, float, float]:
        with _PREVIEW_WORK:
            return draw()

    def draw() -> tuple[bytes, float, float]:
        t0 = time.time()
        particles = _sampled(_prepared(entry, request, token), request.max_particles)
        rgb, _, exposure = _frame(particles, request, request.cameras[0], request.width,
                                  request.height, request.tone, request.tone.exposure)
        from PIL import Image  # noqa: PLC0415
        buf = io.BytesIO()
        Image.fromarray(rgb).save(buf, format="JPEG", quality=88)
        return buf.getvalue(), exposure, (time.time() - t0) * 1000.0

    # One at a time: a client sends its next frame when this one is back, and
    # two clients would only slow each other down.
    async with _PREVIEW_LOCK:
        try:
            jpeg, exposure, ms = await asyncio.to_thread(frame)
        except ValueError as exc:
            raise HTTPException(422, str(exc)) from exc
    return Response(jpeg, media_type="image/jpeg",
                    headers={X_EXPOSURE: repr(float(exposure)), X_RENDER_MS: f"{ms:.0f}"})


class ParticleDataRequest(ParticleRenderRequest):
    cameras: list[RaytraceCamera] = Field(default_factory=list, max_length=1)
    # More than this many: an even sample, each carrying the light of those
    # it stands for (the step is in X-Visivo-Particle-Step).
    max_particles: int = Field(8_000_000, ge=1000, le=50_000_000)
    # True: positions and sizes in the particles' local frame, float32
    # offsets that keep points far from 0 apart, table = origin + scale *
    # local (X-Visivo-Particle-Origin, X-Visivo-Particle-Scale); the client
    # moves its camera into that frame. False (a client that predates it):
    # the table's own coordinates, a float32 cast of them, as before.
    local_frame: bool = False


@router.post("/v1/particle_render/particles")
async def particle_render_particles(request: ParticleDataRequest, token: str = _auth,
                                    session: Session = Depends(get_session)) -> Response:
    """The particles a render draws, prepared as for it, for the desktop's GPU
    preview to sum the same kernels: little-endian float32 rows, their fields
    in ``X-Visivo-Particle-Fields``. A 3-D render's rows: position, size,
    emission at the kernel's peak (app/particle_raytrace.py,
    emission_weights) and colour; a projected map's: position, size, mass at
    the peak and the quantity its hue averages (0 without one)."""
    from ..particle_raytrace import emission_weights  # noqa: PLC0415
    entry = _require_dataset(session, request.dataset_id)
    from .render import point_table_spec  # noqa: PLC0415
    if point_table_spec(entry) is None:
        raise HTTPException(422, "Ray-traced rendering needs a table of points.")

    def pack() -> tuple[bytes, int, str, int, np.ndarray, float]:
        with _PREVIEW_WORK:
            return packed()

    def packed() -> tuple[bytes, int, str, int, np.ndarray, float]:
        n_all = len(_prepared(entry, request, token)["positions"])
        particles = _sampled(_prepared(entry, request, token), request.max_particles)
        step = -(-n_all // request.max_particles) if n_all > request.max_particles else 1
        pos, radius = particles["positions"], particles["radius"]
        origin, scale = particles.get("frame", ((0.0, 0.0, 0.0), 1.0))
        origin, scale = np.asarray(origin, dtype=np.float64), float(scale)
        if not request.local_frame:
            pos, radius = _table_coordinates(pos, radius, origin, scale)
            origin, scale = np.zeros(3), 1.0
        if request.mode == "projected":
            weight = emission_weights(particles["intensity"], radius, "per_particle")
            value = particles.get("value")
            # As projected_maps reads it: a NaN would blank, on the GPU, every
            # pixel the particle covers.
            value = (np.zeros(len(pos)) if value is None
                     else np.nan_to_num(np.asarray(value, dtype=np.float32)))
            columns = [pos, radius[:, None], weight[:, None], value[:, None]]
            names = "x,y,z,r,w,v"
        else:
            weight = emission_weights(particles["intensity"], radius, request.quantity)
            columns = [pos, radius[:, None], weight[:, None], particles["colour"]]
            names = "x,y,z,r,e,red,green,blue"
        rows = np.hstack([np.asarray(c, dtype="<f4") for c in columns])
        return np.ascontiguousarray(rows).tobytes(), len(rows), names, step, origin, scale

    async with _PREVIEW_LOCK:
        try:
            body, count, names, step, origin, scale = await asyncio.to_thread(pack)
        except ValueError as exc:
            raise HTTPException(422, str(exc)) from exc
    return Response(body, media_type="application/octet-stream",
                    headers={X_PARTICLE_COUNT: str(count), X_PARTICLE_FIELDS: names,
                             X_PARTICLE_STEP: str(step),
                             X_PARTICLE_ORIGIN: ",".join(repr(float(v)) for v in origin),
                             X_PARTICLE_SCALE: repr(float(scale))})


def _table_coordinates(pos: np.ndarray, radius: np.ndarray, origin: np.ndarray,
                       scale: float) -> tuple[np.ndarray, np.ndarray]:
    """Local positions and sizes back in the table's coordinates, as float32
    (the rows of a client that does not ask for the local frame): an axis at
    a time, in float64, so the temporaries stay at 8 bytes a particle."""
    out = np.empty(pos.shape, dtype=np.float32)
    for i in range(3):
        out[:, i] = pos[:, i].astype(np.float64) * scale + origin[i]
    return out, (radius if scale == 1.0 else (radius * np.float64(scale)).astype(np.float32))


@router.post("/v1/tasks/particle_render", response_model=TaskCreateResponse)
async def create_particle_render_task(request: ParticleRenderRequest, token: str = _auth,
                                      session: Session = Depends(get_session)) -> TaskCreateResponse:
    entry = _require_dataset(session, request.dataset_id)
    from .render import point_table_spec  # noqa: PLC0415
    if point_table_spec(entry) is None:
        raise HTTPException(422, "Ray-traced rendering needs a table of points "
                                 "(a VBT point table or a catalogue).")
    return start_render_task(
        "particle_render", token,
        lambda task_id, stop: _run(task_id, entry, request, stop))


# The operations whose results the routes below serve, by task id.
RENDER_OPERATIONS = ("particle_render", "volume_render")


def start_render_task(operation: str, token: str, work) -> TaskCreateResponse:
    """A render task running ``work(task_id, stop)`` on a worker thread
    (run_frames), its frames served by the routes below."""
    task = TASKS.create(operation, owner_token=token)
    TASKS.update(task.task_id, status="running", progress=0.02)
    stop = threading.Event()
    _STOPS[task.task_id] = stop

    async def _runner() -> None:
        work_future = asyncio.ensure_future(asyncio.to_thread(work, task.task_id, stop))
        # The record lives as long as the thread does, which a cancellation
        # does not stop before the frame it is on is done.
        TASKS.update(task.task_id, work_handle=work_future)
        try:
            result = await asyncio.shield(work_future)
            TASKS.update(task.task_id, status="completed", progress=1.0,
                         result={"valid": True, "error": "", **result})
        except asyncio.CancelledError:
            stop.set()
            TASKS.update(task.task_id, status="cancelled", progress=0.0,
                         error="Cancelled by client.")
            # The stop flag stays findable (a delete sets it again) until the
            # thread has ended and removed what it wrote.
            with contextlib.suppress(BaseException):
                await work_future
        except Exception as exc:   # noqa: BLE001 - reported to the client
            logger.warning("[%s] %s failed: %s", operation, task.task_id, exc)
            TASKS.update(task.task_id, status="failed", progress=1.0, error=str(exc))
        finally:
            _STOPS.pop(task.task_id, None)

    handle = asyncio.create_task(_runner())
    TASKS.update(task.task_id, asyncio_handle=handle)
    return TaskCreateResponse(valid=True, error="", task_id=task.task_id, status="running")


def _owned_render(task_id: str, token: str) -> Path:
    from .tasks import _owned_task  # noqa: PLC0415
    task = _owned_task(task_id, token)
    if task is None or task.operation not in RENDER_OPERATIONS:
        raise HTTPException(404, f"Unknown render task '{task_id}'.")
    if task.status != "completed":
        raise HTTPException(409, f"Render task '{task_id}' is {task.status}.")
    return _render_dir(task_id)


@router.get("/v1/particle_render/{task_id}/frames/{index}")
async def particle_render_frame(task_id: str, index: int, token: str = _auth) -> FileResponse:
    path = _owned_render(task_id, token) / f"frame_{index:05d}.png"
    if index < 0 or not path.is_file():
        raise HTTPException(404, f"No frame {index} in render task '{task_id}'.")
    return FileResponse(path, media_type="image/png")


def _light(task_id: str, token: str) -> np.ndarray:
    path = _owned_render(task_id, token) / "light.npy"
    if not path.is_file():
        raise HTTPException(404, "Only a still keeps its light.")
    return np.load(path)


@router.post("/v1/particle_render/{task_id}/tone")
async def particle_render_tone(task_id: str, tone: RaytraceTone, token: str = _auth) -> Response:
    """The still graded again with another tone curve, as PNG (nothing is
    rendered again)."""
    from ..particle_raytrace import tone_map  # noqa: PLC0415
    light = _light(task_id, token)
    saved = _owned_render(task_id, token) / "request.json"
    from .tasks import _owned_task  # noqa: PLC0415
    if _owned_task(task_id, token).operation == "volume_render":
        from .volume_render import VolumeRenderRequest, regrade  # noqa: PLC0415
        req = VolumeRenderRequest.model_validate_json(saved.read_text())
        rgb = await asyncio.to_thread(regrade, light, req, tone)
        return Response(_png(rgb), media_type="image/png")
    req = ParticleRenderRequest.model_validate_json(saved.read_text()) if saved.is_file() else None
    if req is not None and req.mode == "projected":
        rgb = await asyncio.to_thread(_projected_image, light, req, tone)
    else:
        params = tone.model_dump(exclude={"exposure", "ev", "decades"})
        exposure = tone.exposure if tone.exposure is not None else _auto_exposure(light)
        rgb = await asyncio.to_thread(tone_map, light, exposure=exposure * 2.0 ** tone.ev,
                                      **params)
    return Response(_png(rgb), media_type="image/png")


def _png(rgb: np.ndarray) -> bytes:
    buf = io.BytesIO()
    from PIL import Image  # noqa: PLC0415
    Image.fromarray(rgb).save(buf, format="PNG", compress_level=3)
    return buf.getvalue()


@router.get("/v1/particle_render/{task_id}/light.fits")
async def particle_render_light_fits(task_id: str, token: str = _auth) -> Response:
    """The still's light before the tone curve: a float32 FITS cube, its
    three planes red, green and blue, rows bottom up as FITS shows them."""
    from astropy.io import fits  # noqa: PLC0415
    light = _light(task_id, token)
    cube = np.ascontiguousarray(np.moveaxis(light, -1, 0)[:, ::-1, :])
    hdu = fits.PrimaryHDU(cube.astype(np.float32))
    if cube.shape[0] == 2:   # a projected map
        hdu.header["CTYPE3"] = ("MAP", "plane 1 projected density, 2 its weighted mean")
        hdu.header["BUNIT"] = ("arbitrary", "relative projected density")
    else:
        hdu.header["CTYPE3"] = ("RGB", "plane 1 red, 2 green, 3 blue")
        hdu.header["BUNIT"] = ("arbitrary", "relative light")
    hdu.header["ORIGIN"] = "VisIVO Visual Analytics"
    buf = io.BytesIO()
    hdu.writeto(buf)
    return Response(buf.getvalue(), media_type="application/fits")


def _on_evict(record) -> None:
    # A render nobody deleted: its frames go with its record. Off the
    # registry's lock, which the listener is called under.
    if record.operation in RENDER_OPERATIONS:
        threading.Thread(target=shutil.rmtree, args=(_render_dir(record.task_id),),
                         kwargs={"ignore_errors": True}, daemon=True).start()


TASKS.add_eviction_listener(_on_evict)


@router.delete("/v1/particle_render/{task_id}")
async def delete_particle_render(task_id: str, token: str = _auth) -> dict:
    from .tasks import _owned_task  # noqa: PLC0415
    task = _owned_task(task_id, token)
    if task is None or task.operation not in RENDER_OPERATIONS:
        raise HTTPException(404, f"Unknown render task '{task_id}'.")
    # Still rendering: stopped first, or it would write its frames after this
    # (its own cleanup removes what it wrote).
    stop = _STOPS.get(task_id)
    if stop is not None:
        stop.set()
        TASKS.cancel(task_id)
    shutil.rmtree(_render_dir(task_id), ignore_errors=True)
    return {"deleted": True}
