"""Several particle components drawn together (issue #394): a request names
its layers (a snapshot's particle types, other datasets, or the rows of one
type in a table with a type column), and the picture is composed from each
layer's maps as app/particle_layers.py describes.

Each layer's particles are prepared as a single-layer render prepares them
(app/routers/particle_render.py, ``_prepare``: positions, smoothing lengths,
filters, any format behind app/point_table.py), then weighed by their mass.
The routes follow the single-layer ones: a JPEG preview on a reduced copy
that follows a drag, a task for stills and movies at full quality (frames,
cancel and delete through the /v1/particle_render routes), and, for a still,
its maps kept so its styles change without a new render.
"""
from __future__ import annotations

import asyncio
import io
import json
import threading
import time
from collections import OrderedDict
from pathlib import Path
from typing import Literal

import numpy as np
from fastapi import APIRouter, Depends, HTTPException
from fastapi.responses import Response
from pydantic import BaseModel, Field, field_validator, model_validator

from ..auth import verify_token
from ..dependencies import _require_dataset
from ..product_cache import dataset_identity
from ..response_headers import X_LAYER_WINDOWS, X_RENDER_MS
from ..schemas import TaskCreateResponse
from ..sessions import Session, get_session
from .particle_render import (_PREVIEW_LOCK, _PREVIEW_WORK, ParticleRenderRequest,
                              RaytraceCamera, _local_camera, _owned_render, _prepare,
                              _render_dir, run_frames, start_render_task)

router = APIRouter(tags=["particle-render"])
_auth = Depends(verify_token)

# Not ``m`` or ``M``: in a catalogue those are mostly magnitudes.
MASS_COLUMNS = ("mass", "Mass", "MASS")
BH_MASS_COLUMNS = ("bh_mass", "BH_Mass", "BH_MASS", "bhmass", *MASS_COLUMNS)
#: The fields of a layer that choose and weigh its particles; the others only
#: style it (a compose of a still may change those alone).
GEOMETRY_FIELDS = ("kind", "dataset_id", "gadget_type", "type_field", "type_value", "filters",
                   "x", "y", "z", "radius_field", "radius_scale", "neighbours", "weight_field",
                   "quantity_field")


class MarkerStyle(BaseModel):
    min_px: float = Field(3.0, ge=0.5, le=64.0)
    max_px: float = Field(16.0, ge=0.5, le=64.0)
    style: Literal["ring", "disc"] = "ring"
    colour: tuple[float, float, float] = (0.35, 1.0, 1.0)
    opacity: float = Field(1.0, ge=0.0, le=1.0)


class ParticleLayer(BaseModel):
    """One component. Its particles: ``dataset_id`` (None: the request's),
    or ``gadget_type`` (a particle type of the request's snapshot), narrowed
    to ``type_field == type_value`` (a table mixing types) and ``filters``.
    Its weight: ``weight_field`` (None: the table's mass column, else one per
    particle). See docs/technical/backend-api/particle-layers.md."""
    name: str = ""
    kind: Literal["particles", "markers"] = "particles"
    dataset_id: str | None = None
    gadget_type: int | None = Field(None, ge=0, le=5)
    type_field: str | None = None
    type_value: float | None = None
    filters: list[dict] = []
    x: str | None = None
    y: str | None = None
    z: str | None = None
    radius_field: str | None = None
    radius_scale: float = Field(1.0, gt=0.0, le=1.0e3)
    neighbours: int = Field(32, ge=1, le=512)
    weight_field: str | None = None
    quantity_field: str | None = None
    quantity_log: bool = False
    quantity_range: tuple[float, float] | None = None
    # The hue runs over the colour map from this fraction up (0: a map that
    # does not start dark is used whole).
    hue_foot: float = Field(0.3, ge=0.0, lt=1.0)
    colormap: str | list[list[float]] = "Inferno"
    scale: Literal["log", "linear", "asinh"] = "log"
    # The column density (a marker layer: the mass) at the bottom and the top
    # of the layer's window; None: measured once, on the first frame.
    range: tuple[float, float] | None = None
    intensity: float = Field(1.0, ge=0.0, le=100.0)
    visible: bool = True
    composite: Literal["emission", "absorption"] = "emission"
    # An absorption layer's optical depth at the top of its window.
    opacity: float = Field(1.0, ge=0.0, le=50.0)
    marker: MarkerStyle = MarkerStyle()

    @field_validator("colormap")
    @classmethod
    def _points(cls, value):
        if isinstance(value, list):
            from ..particle_layers import colour_table  # noqa: PLC0415
            colour_table(value)
        return value

    @model_validator(mode="after")
    def _consistent(self):
        if (self.type_field is None) != (self.type_value is None):
            raise ValueError("type_field and type_value go together.")
        if self.gadget_type is not None and self.dataset_id is not None:
            raise ValueError("A layer names a dataset_id or a gadget_type, not both.")
        for label, window in (("range", self.range), ("quantity_range", self.quantity_range)):
            if window is not None and not window[1] > window[0]:
                raise ValueError(f"{label}: the top must be above the bottom.")
        if self.range is not None and (self.kind == "markers" or self.scale != "linear") \
                and not self.range[0] > 0:
            raise ValueError("range: a log or asinh window (or a mass window) starts above 0.")
        if self.quantity_range is not None and self.quantity_log and not self.quantity_range[0] > 0:
            raise ValueError("quantity_range: a log window starts above 0.")
        if self.marker.max_px < self.marker.min_px:
            raise ValueError("marker: max_px must be at least min_px.")
        return self


class ParticleLayersRequest(BaseModel):
    dataset_id: str
    # Back to front: an absorption layer dims the layers listed before it.
    layers: list[ParticleLayer] = Field(..., min_length=1, max_length=16)
    width: int = Field(1920, ge=16, le=8192)
    height: int = Field(1080, ge=16, le=8192)
    cameras: list[RaytraceCamera] = Field(..., min_length=1, max_length=7200)
    gamma: float = Field(1.0, gt=0.0, le=10.0)


class ParticleLayersPreviewRequest(ParticleLayersRequest):
    width: int = Field(640, ge=16, le=2048)
    height: int = Field(360, ge=16, le=2048)
    cameras: list[RaytraceCamera] = Field(..., min_length=1, max_length=1)
    # A draft while the view turns: at most this many particles a layer, an
    # even sample, each carrying the mass of those it stands for.
    max_particles: int | None = Field(None, ge=1000)


class ParticleLayersCompose(BaseModel):
    """A still's layers again (the same count, kind and particles), with
    other styles."""
    layers: list[ParticleLayer] = Field(..., min_length=1, max_length=16)
    gamma: float = Field(1.0, gt=0.0, le=10.0)


# ── The particles of a layer ─────────────────────────────────────────────────

def layer_entry(entry: dict, layer: ParticleLayer, session: Session) -> dict:
    """The dataset a layer draws: its own, a particle type of the request's
    snapshot, or the request's."""
    if layer.dataset_id is not None:
        return _require_dataset(session, layer.dataset_id)
    if layer.gadget_type is None:
        return entry
    from ..gadget_snapshot import gadget_ref, split_ref  # noqa: PLC0415
    from ..point_table import component_format  # noqa: PLC0415
    path = str(entry.get("path") or "")
    if component_format(path) != "gadget":
        raise HTTPException(422, "gadget_type names a particle type of a Gadget snapshot; "
                                 "this dataset is not one.")
    return {**entry, "path": gadget_ref(split_ref(path)[0], layer.gadget_type)}


def _first_column(names, candidates) -> str | None:
    return next((c for c in candidates if c in names), None)


def _layer_request(layer: ParticleLayer) -> ParticleRenderRequest:
    filters = list(layer.filters)
    if layer.type_field is not None:
        filters.append({"field": layer.type_field, "op": "=", "value": layer.type_value})
    return ParticleRenderRequest(
        dataset_id="", x=layer.x, y=layer.y, z=layer.z, filters=filters,
        radius_field=layer.radius_field, radius_scale=layer.radius_scale,
        neighbours=layer.neighbours, mode="projected", colour_by="density",
        cameras=[RaytraceCamera(position=(0, 0, 1), focal_point=(0, 0, 0), view_up=(0, 1, 0))])


def _weight_candidates(spec: str, layer: ParticleLayer) -> tuple[str, ...]:
    """The columns a layer's weight may be, in order: its own, else the
    table's mass columns (a black hole's own mass first, for markers). A
    snapshot or a VBT is asked for its names (it opens without reading
    them); a catalogue, already read whole, is given every candidate."""
    if layer.weight_field:
        return (layer.weight_field,)
    candidates = BH_MASS_COLUMNS if layer.kind == "markers" else MASS_COLUMNS
    if spec.startswith("{"):
        return candidates
    from ..point_table import open_point_table  # noqa: PLC0415
    first = _first_column(open_point_table(spec).columns, candidates)
    return (first,) if first else ()


def prepare_layer(entry: dict, layer: ParticleLayer) -> dict:
    """A layer's particles in their local frame (as _prepare gives them) and
    their ``mass`` (the weight, in the table's units) and ``value`` (the
    quantity whose mean colours it, or None). ``weight``: the weight column
    used, or ``"count"``; a marker layer's ``mass_window`` too, over all its
    particles, so its markers keep their sizes in every frame."""
    from ..particle_layers import mass_window  # noqa: PLC0415
    from .render import point_table_spec  # noqa: PLC0415
    spec = point_table_spec(entry)
    if spec is None:
        raise ValueError("Ray-traced rendering needs a table of points.")
    candidates = _weight_candidates(spec, layer)
    quantity = layer.quantity_field if layer.kind == "particles" else None
    wanted = tuple(dict.fromkeys((*candidates, *((quantity,) if quantity else ()))))
    particles = _prepare(entry, _layer_request(layer), columns=wanted)
    columns = particles.pop("columns", {})
    if quantity and quantity not in columns:
        raise ValueError(f"Unknown quantity field: {quantity!r}")
    if layer.weight_field and layer.weight_field not in columns:
        raise ValueError(f"Unknown weight field: {layer.weight_field!r}")
    weight = _first_column(columns, candidates)
    mass = columns[weight] if weight else np.ones(len(particles["positions"]))
    good = np.isfinite(mass) & (mass > 0)
    out = {"positions": particles["positions"][good], "radius": particles["radius"][good],
           "frame": particles["frame"], "mass": mass[good],
           "value": columns[quantity][good] if quantity else None,
           "weight": weight or "count"}
    if not len(out["positions"]):
        raise ValueError("No particle with a positive weight.")
    if layer.kind == "markers":
        out["mass_window"] = mass_window(out["mass"])
    return out


def _sampled(particles: dict, limit: int | None) -> dict:
    n = len(particles["positions"])
    if not limit or n <= limit:
        return particles
    step = -(-n // limit)
    out = {k: (v[::step] if isinstance(v, np.ndarray) else v) for k, v in particles.items()}
    out["mass"] = out["mass"] * float(step)
    return out


# ── Frames ───────────────────────────────────────────────────────────────────

def layer_frame(particles: dict, layer: ParticleLayer, camera, width: int,
                height: int) -> np.ndarray:
    """A particle layer's maps (column density in the table's units, mean
    quantity) or a marker layer's markers, seen by ``camera``."""
    from ..particle_layers import layer_maps, place_markers  # noqa: PLC0415
    cam, scale = _local_camera(camera, particles)
    if layer.kind == "markers":
        return place_markers(particles["positions"], particles["mass"], cam, width, height,
                             layer.range or particles["mass_window"], layer.marker.min_px,
                             layer.marker.max_px)
    maps = layer_maps(particles["positions"], particles["radius"], particles["mass"],
                      particles["value"], cam, width, height)
    maps[..., 0] /= scale * scale
    return maps


def _maps(prepared: list, layers: list[ParticleLayer], camera, width: int,
          height: int) -> list[np.ndarray | None]:
    """Each visible layer's :func:`layer_frame`; None for a hidden one."""
    return [None if particles is None or not layer.visible
            else layer_frame(particles, layer, camera, width, height)
            for particles, layer in zip(prepared, layers)]


def _windows(maps: list, prepared: list, layers: list[ParticleLayer]) -> list[dict]:
    """Each layer's windows: its own, else measured on ``maps`` (a marker
    layer's: the masses of all its particles)."""
    from ..particle_layers import auto_window, quantity_window  # noqa: PLC0415
    out = []
    for m, particles, layer in zip(maps, prepared, layers):
        given = {"range": list(layer.range) if layer.range else None,
                 "quantity_range": list(layer.quantity_range) if layer.quantity_range else None}
        if layer.kind == "markers":
            if particles is not None and not layer.range:
                given["range"] = [float(v) for v in particles["mass_window"]]
            out.append({**given, "quantity_range": None})
            continue
        if m is None:
            out.append(given)
            continue
        window = layer.range or auto_window(m[..., 0], layer.scale)
        qwindow = None
        if layer.quantity_field:
            qwindow = layer.quantity_range or quantity_window(m[..., 1], layer.quantity_log)
        out.append({"range": [float(v) for v in window],
                    "quantity_range": [float(v) for v in qwindow] if qwindow else None})
    return out


def picture(maps: list, layers: list[ParticleLayer], windows: list[dict], shape,
            gamma: float) -> tuple[np.ndarray, np.ndarray]:
    """``(8-bit picture, linear composite)`` of a frame's maps."""
    from ..particle_layers import (colour_table, compose, draw_markers,  # noqa: PLC0415
                                   finish, layer_light, optical_depth)
    parts = []
    for m, layer, window in zip(maps, layers, windows):
        if m is None or layer.kind == "markers":
            continue
        qwindow = window["quantity_range"] if layer.quantity_field else None
        light = layer_light(m, colour_table(layer.colormap), window["range"], layer.scale,
                            layer.intensity, qwindow, layer.quantity_log, layer.hue_foot)
        part = {"light": light, "composite": layer.composite}
        if layer.composite == "absorption":
            part["tau"] = optical_depth(m[..., 0], window["range"], layer.opacity)
        parts.append(part)
    linear = compose(parts, shape)
    rgb = finish(linear, gamma)
    for m, layer in zip(maps, layers):
        if m is not None and layer.kind == "markers" and len(m):
            rgb = draw_markers(rgb, m, layer.marker.colour, layer.marker.style,
                               layer.marker.opacity)
    return rgb, linear.astype(np.float32)


def _fixed(layers: list[ParticleLayer], windows: list[dict]) -> list[ParticleLayer]:
    """The layers with the windows found, so later frames keep them."""
    return [layer.model_copy(update={
        "range": tuple(w["range"]) if w["range"] else None,
        "quantity_range": tuple(w["quantity_range"]) if w["quantity_range"] else None})
        for layer, w in zip(layers, windows)]


class _Frames:
    """What run_frames reads of a request: the cameras, the size, no
    exposure (each layer's window is its exposure) and the request saved
    with a still."""

    def __init__(self, req: ParticleLayersRequest):
        self.req, self.cameras = req, req.cameras
        self.width, self.height = req.width, req.height
        self.tone = type("Tone", (), {"exposure": None})()

    def model_dump_json(self) -> str:
        return self.req.model_dump_json()


def _run(task_id: str, entries: list[dict], req: ParticleLayersRequest,
         stop: threading.Event) -> dict:
    state = {"layers": list(req.layers), "windows": None}
    still = len(req.cameras) == 1

    def prepare() -> list:
        out = []
        for i, (entry, layer) in enumerate(zip(entries, req.layers)):
            if stop.is_set():
                break
            # A hidden layer is not drawn: nothing to prepare.
            out.append(_named(layer, i, prepare_layer, entry, layer) if layer.visible else None)
        return out

    def frame(prepared, camera, _exposure):
        layers = state["layers"]
        maps = _maps(prepared, layers, camera, req.width, req.height)
        if state["windows"] is None:
            state["windows"] = _windows(maps, prepared, layers)
            state["layers"] = _fixed(layers, state["windows"])
        rgb, linear = picture(maps, state["layers"], state["windows"],
                              (req.height, req.width), req.gamma)
        if still:
            _save_maps(_render_dir(task_id), maps, state["layers"], state["windows"],
                       [p["weight"] if p is not None else None for p in prepared])
        return rgb, linear, 1.0

    def summary(prepared) -> dict:
        windows = state["windows"] or [{}] * len(prepared)
        return {"layers": [{"name": layer.name or f"layer {i + 1}",
                            "particles": int(len(p["positions"])) if p is not None else 0,
                            "weight": p["weight"] if p is not None else None, **windows[i]}
                           for i, (p, layer) in enumerate(zip(prepared, req.layers))]}

    return run_frames(task_id, _Frames(req), stop, prepare, frame, summary)


def _named(layer: ParticleLayer, index: int, fn, *args):
    """``fn(*args)``, a ValueError naming the layer it is about."""
    try:
        return fn(*args)
    except ValueError as exc:
        raise ValueError(f"Layer {layer.name or index + 1}: {exc}") from exc


def _save_maps(folder: Path, maps: list, layers: list[ParticleLayer],
               windows: list[dict], weights: list[str | None]) -> None:
    arrays = {f"{'markers' if layers[i].kind == 'markers' else 'maps'}_{i}": np.asarray(m)
              for i, m in enumerate(maps) if m is not None}
    np.savez(folder / "layers.npz", **arrays)
    (folder / "windows.json").write_text(json.dumps({"windows": windows, "weights": weights}))


def _check_entries(session: Session, req: ParticleLayersRequest) -> list[dict]:
    from .render import point_table_spec  # noqa: PLC0415
    entry = _require_dataset(session, req.dataset_id)
    entries = [layer_entry(entry, layer, session) for layer in req.layers]
    for e in entries:
        if point_table_spec(e) is None:
            raise HTTPException(422, "Ray-traced rendering needs a table of points "
                                     "(a VBT point table, a snapshot or a catalogue).")
    return entries


@router.post("/v1/tasks/particle_layers", response_model=TaskCreateResponse)
async def create_particle_layers_task(request: ParticleLayersRequest, token: str = _auth,
                                      session: Session = Depends(get_session)
                                      ) -> TaskCreateResponse:
    """Stills and movie frames of several components; frames, cancel and
    delete as a single-layer render's (/v1/particle_render/{task_id}/…)."""
    entries = _check_entries(session, request)
    return start_render_task("particle_layers", token,
                             lambda task_id, stop: _run(task_id, entries, request, stop))


# ── The interactive preview ─────────────────────────────────────────────────

# Each layer's prepared particles, by everything that chooses and weighs them:
# a style change, a camera move or another layer's change prepares nothing.
_PREPARED: OrderedDict[str, dict] = OrderedDict()
# A full table each: a handful, a snapshot's components being millions of rows.
_PREPARED_KEEP = 4
# The maps of the last view previewed, by layer: a change of style or of
# visibility composes them again without drawing a particle.
_MAPS: dict = {"view": None, "layers": {}}


def _layer_key(token: str, entry: dict, layer: ParticleLayer) -> str:
    """What chooses and weighs a layer's particles."""
    return json.dumps([token, dataset_identity(entry), str(entry.get("path")),
                       layer.model_dump(include=set(GEOMETRY_FIELDS))],
                      sort_keys=True, default=str)


def _maps_key(token: str, entry: dict, layer: ParticleLayer) -> str:
    """What a layer's maps depend on beyond the view: a marker layer's sizes too."""
    key = _layer_key(token, entry, layer)
    if layer.kind == "markers":
        key += json.dumps([layer.range, layer.marker.min_px, layer.marker.max_px])
    return key


def _prepared(token: str, entry: dict, layer: ParticleLayer, index: int) -> dict:
    key = _layer_key(token, entry, layer)
    if key in _PREPARED:
        _PREPARED.move_to_end(key)
        return _PREPARED[key]
    particles = _named(layer, index, prepare_layer, entry, layer)
    _PREPARED[key] = particles
    while len(_PREPARED) > _PREPARED_KEEP:
        _PREPARED.popitem(last=False)
    return particles


def _preview_maps(token: str, entries: list[dict], request) -> tuple[list, list]:
    """``(prepared, maps)`` of a preview, each layer's maps drawn again only
    when the view or what the layer draws changed (a marker layer is never
    sampled: a few particles, each with its own mass)."""
    view = json.dumps([request.cameras[0].model_dump(), request.width, request.height,
                       request.max_particles], default=str)
    if _MAPS["view"] != view:
        _MAPS.update(view=view, layers={})
    cached = _MAPS["layers"]
    prepared, maps, keep = [], [], {}
    for i, (entry, layer) in enumerate(zip(entries, request.layers)):
        key = _maps_key(token, entry, layer)
        if not layer.visible:
            # Kept: showing it again composes without drawing.
            if key in cached:
                keep[key] = cached[key]
            prepared.append(None)
            maps.append(None)
            continue
        particles = _prepared(token, entry, layer, i)
        if layer.kind == "particles":
            particles = _sampled(particles, request.max_particles)
        if key not in cached:
            cached[key] = layer_frame(particles, layer, request.cameras[0], request.width,
                                      request.height)
        keep[key] = cached[key]
        prepared.append(particles)
        maps.append(cached[key])
    _MAPS["layers"] = keep
    return prepared, maps


@router.post("/v1/particle_layers/preview")
async def particle_layers_preview(request: ParticleLayersPreviewRequest, token: str = _auth,
                                  session: Session = Depends(get_session)) -> Response:
    """One frame of the layers, small enough to follow a drag: a JPEG, with
    each layer's windows (``X-Visivo-Layer-Windows``, JSON: ``[{"range",
    "quantity_range"}, …]``) for the next requests to send back, so the
    brightness does not change as the view turns."""
    entries = _check_entries(session, request)

    def draw() -> tuple[bytes, list, float]:
        t0 = time.time()
        prepared, maps = _preview_maps(token, entries, request)
        windows = _windows(maps, prepared, request.layers)
        rgb, _ = picture(maps, request.layers, windows, (request.height, request.width),
                         request.gamma)
        from PIL import Image  # noqa: PLC0415
        buf = io.BytesIO()
        Image.fromarray(rgb).save(buf, format="JPEG", quality=88)
        return buf.getvalue(), windows, (time.time() - t0) * 1000.0

    def frame():
        with _PREVIEW_WORK:
            return draw()

    async with _PREVIEW_LOCK:
        try:
            jpeg, windows, ms = await asyncio.to_thread(frame)
        except ValueError as exc:
            raise HTTPException(422, str(exc)) from exc
    return Response(jpeg, media_type="image/jpeg",
                    headers={X_LAYER_WINDOWS: json.dumps(windows, separators=(",", ":")),
                             X_RENDER_MS: f"{ms:.0f}"})


# ── A still's maps ───────────────────────────────────────────────────────────

def _still(task_id: str, token: str) -> tuple[Path, ParticleLayersRequest]:
    from .tasks import _owned_task  # noqa: PLC0415
    folder = _owned_render(task_id, token)
    if _owned_task(task_id, token).operation != "particle_layers" \
            or not (folder / "layers.npz").is_file():
        raise HTTPException(404, "Only a still of several layers keeps its maps.")
    saved = ParticleLayersRequest.model_validate_json((folder / "request.json").read_text())
    return folder, saved


def _load_maps(folder: Path, n: int) -> list:
    with np.load(folder / "layers.npz") as data:
        return [data[f"maps_{i}"] if f"maps_{i}" in data
                else data[f"markers_{i}"] if f"markers_{i}" in data else None
                for i in range(n)]


def _recompose(folder: Path, saved: ParticleLayersRequest, body: ParticleLayersCompose) -> bytes:
    from ..particle_layers import auto_window, quantity_window  # noqa: PLC0415
    maps = _load_maps(folder, len(saved.layers))
    windows = json.loads((folder / "windows.json").read_text())["windows"]
    fixed = _fixed(saved.layers, windows)
    layers = []
    for i, (layer, old, m) in enumerate(zip(body.layers, fixed, maps)):
        if layer.kind == "particles" and m is not None:
            # A window measured for another scale is measured again for this one.
            if layer.range is None and layer.scale != old.scale:
                windows[i]["range"] = list(auto_window(m[..., 0], layer.scale))
            if layer.quantity_field and layer.quantity_range is None \
                    and layer.quantity_log != old.quantity_log:
                windows[i]["quantity_range"] = list(quantity_window(m[..., 1],
                                                                    layer.quantity_log))
        if layer.kind == "markers" and m is not None and layer.visible and (
                (layer.marker.min_px, layer.marker.max_px)
                != (old.marker.min_px, old.marker.max_px)
                or (layer.range is not None and list(layer.range) != windows[i]["range"])):
            raise ValueError(f"Layer {i + 1}: a marker layer's sizes are set by its render; "
                             "render it again.")
        if layer.kind == "particles":
            if layer.range is not None:
                windows[i]["range"] = list(layer.range)
            if layer.quantity_range is not None:
                windows[i]["quantity_range"] = list(layer.quantity_range)
        layers.append(layer if m is not None else layer.model_copy(update={"visible": False}))
    maps = [m if layer.visible else None for m, layer in zip(maps, layers)]
    rgb, _ = picture(maps, layers, windows, (saved.height, saved.width), body.gamma)
    from PIL import Image  # noqa: PLC0415
    buf = io.BytesIO()
    Image.fromarray(rgb).save(buf, format="PNG", compress_level=3)
    return buf.getvalue()


@router.post("/v1/particle_layers/{task_id}/compose")
async def particle_layers_compose(task_id: str, body: ParticleLayersCompose,
                                  token: str = _auth) -> Response:
    """A still composed again with other styles (colour maps, windows,
    intensities, composites, visibility of the layers it drew), as PNG:
    nothing is rendered again. A layer hidden in the render has no maps and
    stays hidden; one that draws other particles, or markers of other
    sizes, is refused (409)."""
    folder, saved = _still(task_id, token)
    if len(body.layers) != len(saved.layers):
        raise HTTPException(409, f"The still has {len(saved.layers)} layers.")
    for i, (new, old) in enumerate(zip(body.layers, saved.layers)):
        if new.model_dump(include=set(GEOMETRY_FIELDS)) != old.model_dump(
                include=set(GEOMETRY_FIELDS)):
            raise HTTPException(409, f"Layer {i + 1} draws other particles: render it again.")
    try:
        png = await asyncio.to_thread(_recompose, folder, saved, body)
    except ValueError as exc:
        raise HTTPException(409, str(exc)) from exc
    return Response(png, media_type="image/png")


@router.get("/v1/particle_layers/{task_id}/maps.fits")
async def particle_layers_maps_fits(task_id: str, token: str = _auth) -> Response:
    """A still's maps before any colour: one image extension a particle layer
    drawn, its planes the column density (the weight per unit area, in the
    table's units) and the weighted mean of its quantity, rows bottom up."""
    from astropy.io import fits  # noqa: PLC0415
    folder, saved = _still(task_id, token)
    maps = _load_maps(folder, len(saved.layers))
    weights = json.loads((folder / "windows.json").read_text())["weights"]
    hdus = [fits.PrimaryHDU()]
    hdus[0].header["ORIGIN"] = "VisIVO Visual Analytics"
    for i, (m, layer) in enumerate(zip(maps, saved.layers)):
        if m is None or layer.kind == "markers":
            continue
        cube = np.ascontiguousarray(np.moveaxis(m, -1, 0)[:, ::-1, :]).astype(np.float32)
        name = (layer.name or "").encode("ascii", "ignore").decode().strip()
        hdu = fits.ImageHDU(cube, name=(name or f"LAYER{i + 1}")[:68])
        hdu.header["CTYPE3"] = ("MAP", "plane 1 column density, 2 weighted mean")
        hdu.header["WEIGHT"] = (str(weights[i] or "count")[:68], "weight column")
        hdu.header["MAPUNIT"] = ("weight/length^2", "plane 1, in the table's own units")
        if layer.quantity_field:
            hdu.header["QUANTITY"] = (layer.quantity_field[:68], "plane 2 quantity")
        hdus.append(hdu)
    buf = io.BytesIO()
    fits.HDUList(hdus).writeto(buf)
    return Response(buf.getvalue(), media_type="application/fits")


@router.get("/v1/particle_layers/presets")
async def particle_layers_presets(token: str = _auth) -> dict:
    """Common looks of a simulation's components, each the fields of a layer
    it sets (app/particle_layers.py, PRESETS)."""
    from ..particle_layers import PRESETS  # noqa: PLC0415
    return {"presets": list(PRESETS)}
