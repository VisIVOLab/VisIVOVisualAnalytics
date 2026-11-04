"""Ray-traced volume rendering (issue #106): stills and movies of a 3-D grid
(a FITS cube, a field of a VBT volume) as a medium that emits and absorbs
light, and the grid prepared for the desktop's GPU view.

The model is app/volume_raytrace.py. A render is a task of the particles'
kind: its frames, its tone, its light and its deletion are served by the
routes of app/routers/particle_render.py, by task id.

Brightness comes from the dataset; colour from the same quantity, from
another field of the same VBT volume (a two-field colour table) or, for a
spectral cube, from the velocity of each voxel's channel, through a named map
or the client's own colours. A projected map's colour is the
density-weighted mean of the colour index along the ray, so the desktop's GPU
view, which averages the same index, shows what a still saves.
"""
from __future__ import annotations

import asyncio
import io
import json
import math
import threading
import zlib
import time
from collections import OrderedDict
from typing import Literal

import numpy as np
from fastapi import APIRouter, Depends, HTTPException, Request
from fastapi.responses import Response
from pydantic import BaseModel, Field

from ..auth import verify_token
from ..dependencies import _require_dataset
from ..response_headers import (X_COLOUR_RANGE, X_COLOUR_TABLE, X_EXPOSURE, X_RENDER_MS,
                                X_VOLUME_BOX, X_VOLUME_SHAPE)
from ..schemas import TaskCreateResponse
from ..sessions import Session, get_session
from .particle_render import (RaytraceCamera, RaytraceColour, RaytraceTone, _auto_exposure,
                              start_render_task, run_frames)

router = APIRouter(tags=["volume-render"])
_auth = Depends(verify_token)

_LUT_SIZE = 256


class VolumeRenderRequest(BaseModel):
    dataset_id: str
    # The world box the grid's points span (x0, x1, y0, y1, z0, z1): the
    # desktop's 3-D view places the cube it shows there. None: the full
    # cube's voxel coordinates, 0 to n - 1 along each axis.
    box: tuple[float, float, float, float, float, float] | None = None
    # The light: the dataset's values (``brightness_log10``: they hold their
    # logarithm), raised to ``brightness_power`` (the contrast).
    brightness_log10: bool = False
    brightness_power: float = Field(1.0, gt=0.0, le=8.0)
    # Values at or below it emit nothing (in the dataset's own values, a
    # logarithm when they hold one): the 3-D view's threshold, so a radio
    # cube's noise, summed over every channel along a ray, does not veil it.
    # None: only values at or below 0 are dark.
    brightness_floor: float | None = None
    # The colour: of the brightness itself (``density``), of another field
    # of the same VBT volume (``colour``, ``colour_field`` its index) or of
    # the velocity of each voxel's channel, in km/s on a linear scale
    # (``velocity``: a FITS cube whose third axis is spectral), through
    # ``colour`` (a named map or the client's points; its range and scale).
    colour_by: Literal["density", "colour", "velocity"] = "density"
    colour_field: int | None = Field(None, ge=0)
    colour: RaytraceColour = RaytraceColour()
    absorption: float = Field(0.0, ge=0.0, le=50.0)
    # ``light``: emission and absorption in perspective. ``projected``: the
    # column of the brightness along each line of sight, without perspective,
    # within ``slab`` of the focal plane (world units; None: all of it).
    mode: Literal["light", "projected"] = "light"
    slab: float | None = Field(None, gt=0.0)
    # Samples per voxel along a ray (2: every half voxel).
    quality: float = Field(2.0, ge=0.25, le=16.0)
    # Larger grids are read with a stride, to at most this many voxels.
    max_voxels: int = Field(512 ** 3, ge=4096, le=1024 ** 3)
    cameras: list[RaytraceCamera] = Field(..., min_length=1, max_length=7200)
    width: int = Field(1920, ge=16, le=8192)
    height: int = Field(1080, ge=16, le=8192)
    tone: RaytraceTone = RaytraceTone()


class VolumeGridRequest(VolumeRenderRequest):
    cameras: list[RaytraceCamera] = Field(default_factory=list, max_length=1)
    max_voxels: int = Field(256 ** 3, ge=4096, le=512 ** 3)
    # A GPU's 3-D textures: at most this many voxels along any axis (2048 is
    # what OpenGL 4.1 guarantees).
    max_axis: int = Field(2048, ge=16, le=16384)


def _paths(entry: dict, req: VolumeRenderRequest) -> tuple[str, str | None]:
    """The brightness's volume and the colour's (None: the brightness)."""
    from ..volume_source import (resolve_vbt, vbt_field_ref, vbt_volume_header,  # noqa: PLC0415
                                 volume_shape)
    path = str(entry.get("path") or "")
    if entry.get("kind") != "cube" or not path:
        raise HTTPException(422, "Ray-traced volume rendering needs a cube.")
    try:
        full, _ = volume_shape(path)
    except (OSError, ValueError) as exc:
        raise HTTPException(422, f"Cannot read the cube: {exc}") from exc
    # A plane has no depth for a ray to cross: a black picture, not a render.
    if len(full) < 3 or any(int(n) < 2 for n in full[-3:]):
        raise HTTPException(422, f"Ray-traced volume rendering needs a cube at least two "
                                 f"voxels deep along each axis (this one is {tuple(full)}).")
    if req.colour_by == "velocity":
        _velocities(path, int(full[-3]))   # refused now, not in the task
    if req.colour_by != "colour":
        return path, None
    if req.colour_field is None:
        raise HTTPException(422, "colour_by 'colour' needs colour_field, the field to colour by.")
    vbt = resolve_vbt(path)
    if vbt is None:
        raise HTTPException(422, "A colour from another field needs a VBT volume; "
                                 "this cube has one field.")
    count = vbt_volume_header(vbt[0])["field_count"]
    if not req.colour_field < count:
        raise HTTPException(422, f"The volume has {count} fields; no field {req.colour_field}.")
    return path, vbt_field_ref(vbt[0], req.colour_field)


def _velocities(path: str, depth: int) -> np.ndarray:
    """The velocity of each of the cube's ``depth`` channels, in km/s: a
    velocity axis as it is, a frequency axis as optical velocity from the
    rest frequency (app/compute/kinematic_fit.cube_velocity_axis)."""
    from ..compute.kinematic_fit import cube_velocity_axis  # noqa: PLC0415
    from ..fits_dataset import ScientificFitsDataset  # noqa: PLC0415
    from ..volume_source import resolve_vbt  # noqa: PLC0415
    if resolve_vbt(path) is not None:
        raise HTTPException(422, "A VBT volume has no spectral axis to colour by velocity.")
    try:
        ds = ScientificFitsDataset(path)
        # The grid is read as (FITS axis 3, 2, 1): its depth must be the spectrum.
        if ds.cube_numpy_spectral_fits_axis() != 3:
            raise ValueError("Colouring by velocity needs the spectral axis to be the "
                             "cube's third (NAXIS3).")
        return np.asarray(cube_velocity_axis(ds, depth)["vel"], dtype=np.float64)
    except (OSError, ValueError) as exc:
        raise HTTPException(422, f"Cannot colour by velocity: {exc}") from exc


def _stride(shape: tuple[int, ...], max_voxels: int, max_axis: int | None = None) -> int:
    """The one stride along every axis that keeps the grid within
    ``max_voxels`` (and ``max_axis`` along each axis)."""
    stride = max(1, math.ceil((math.prod(shape) / max_voxels) ** (1.0 / 3.0)))
    if max_axis:
        stride = max(stride, *(math.ceil(n / max_axis) for n in shape))
    return stride


def _read(path: str, stride: int) -> np.ndarray:
    """The volume as float32, read with ``stride``."""
    from ..volume_source import volume_data  # noqa: PLC0415
    with volume_data(path) as data:
        return np.array(data[::stride, ::stride, ::stride], dtype=np.float32)


def _lut(colour: RaytraceColour, lo: float, hi: float, log: bool) -> np.ndarray:
    """``_LUT_SIZE`` colours evenly over the colour index: a named map over
    [0, 1], or the client's points at their data values over [lo, hi] (on a
    log scale, evenly in the logarithm)."""
    from ..render_session._common import _build_color_tf, _parse_tf_points  # noqa: PLC0415
    t = (np.arange(_LUT_SIZE) + 0.5) / _LUT_SIZE
    if colour.points:
        import vtk  # noqa: PLC0415
        ctf = vtk.vtkColorTransferFunction()
        for x, r, g, b in _parse_tf_points(colour.points, 4):
            ctf.AddRGBPoint(x, r, g, b)
        log = log and lo > 0
        if log:
            ctf.SetScale(1)   # between its points, evenly in the logarithm
        xs = lo * (hi / lo) ** t if log else lo + (hi - lo) * t
    else:
        ctf = _build_color_tf(colour.lut, 0.0, 1.0)
        xs = t
    return np.array([ctf.GetColor(float(x)) for x in xs], dtype=np.float32)


def _prepare(entry: dict, req: VolumeRenderRequest) -> dict:
    """The grids a render or the GPU view draws: the emissivity, the colour
    index in [0, 1], their table and the colour's range, the world box."""
    from ..volume_raytrace import colour_index, emission_from, velocity_range  # noqa: PLC0415
    from ..volume_source import volume_shape  # noqa: PLC0415
    bright_path, colour_path = _paths(entry, req)
    shape = tuple(int(n) for n in volume_shape(bright_path)[0][-3:])
    stride = _stride(shape, req.max_voxels, getattr(req, "max_axis", None))
    field = _read(bright_path, stride)
    nz, ny, nx = shape
    full = req.box or (0.0, nx - 1.0, 0.0, ny - 1.0, 0.0, nz - 1.0)
    # The box the cube's points span; a strided grid's last point is the
    # cube's voxel (m - 1)·stride, short of n - 1: its box ends there, or the
    # grid would be stretched over the whole cube's.
    box = []
    for axis, n in zip(range(3), (nx, ny, nz)):
        lo, hi = float(full[2 * axis]), float(full[2 * axis + 1])
        m = field.shape[2 - axis]
        box += [lo, lo + (hi - lo) * ((m - 1) * stride) / max(n - 1, 1)]
    emission = emission_from(field, log10=req.brightness_log10, power=req.brightness_power,
                             floor=req.brightness_floor)
    log = _colour_log(req)
    if req.colour_by == "velocity":
        # Each voxel's channel's velocity: one value per plane of the grid,
        # over the range that holds the light.
        vel = _velocities(bright_path, nz)[::stride]
        lo, hi = velocity_range(vel, emission, lo=req.colour.min, hi=req.colour.max)
        plane = np.clip((vel - lo) / (hi - lo), 0.0, 1.0).astype(np.float32)
        index = np.ascontiguousarray(np.broadcast_to(plane[:, None, None], emission.shape))
        del field
        return _grids(req, emission, index, lo, hi, False, box, shape)
    if colour_path is not None:
        cshape = tuple(int(n) for n in volume_shape(colour_path)[0][-3:])
        if cshape != shape:
            raise HTTPException(422, f"The colour field's grid {cshape} is not the "
                                     f"brightness's {shape}.")
        source = _read(colour_path, stride)
        source_log10 = False
    else:
        # The brightness's own colour: of the quantity, not of its contrast;
        # a logarithm kept as one (10 to it may not fit a float).
        source = field
        source_log10 = req.brightness_log10
    # The colour's range where there is light: an emissivity a ten-thousandth
    # of the brightest is about where the tone curve's darkest grey ends.
    lit = emission > np.float32(1e-4)
    if source_log10:
        # The logarithm is the log scale: indexed linearly in it; the range
        # reported in the quantity's own values.
        to_log = (lambda v: None if v is None else math.log10(v) if v > 0 else None)
        index, llo, lhi = colour_index(source, lo=to_log(req.colour.min),
                                       hi=to_log(req.colour.max), lit=lit)
        lo, hi, log = 10.0 ** llo, 10.0 ** lhi, True
    else:
        index, lo, hi = colour_index(source, lo=req.colour.min, hi=req.colour.max, log=log,
                                     lit=lit)
    del lit, source, field
    return _grids(req, emission, index, lo, hi, log, box, shape)


def _grids(req: VolumeRenderRequest, emission: np.ndarray, index: np.ndarray, lo: float,
           hi: float, log: bool, box: list, shape: tuple) -> dict:
    return {"emission": emission, "index": index, "lut": _lut(req.colour, lo, hi, log),
            # A map's hue by its own column: the named map, over its brightness.
            "density_lut": _lut(RaytraceColour(lut=req.colour.lut), 0.0, 1.0, False),
            "range": (lo, hi), "log": log, "box": tuple(float(v) for v in box),
            "shape": shape}


def _projected_image(maps: np.ndarray, lut: np.ndarray, tone: RaytraceTone,
                     by_density: bool) -> np.ndarray:
    """A projected map's picture: brightness from the logarithm of the column,
    the hue from the brightness itself or from the mean colour index."""
    from ..particle_raytrace import density_brightness  # noqa: PLC0415
    column, mean = maps[..., 0], maps[..., 1]
    b = density_brightness(column, ev=tone.ev, decades=tone.decades, gamma=1.0)
    hue_at = b if by_density else np.nan_to_num(mean, nan=0.0)
    i = np.clip((hue_at * (len(lut) - 1) + 0.5).astype(np.int64), 0, len(lut) - 1)
    rgb = lut[i]
    # By density the map's own ramp is the brightness; by another quantity
    # its colour is weighted by the column's.
    rgb = rgb * (b > 0)[..., None] if by_density else rgb * b[..., None]
    return (np.clip(rgb, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)


def _frame(grids: dict, req: VolumeRenderRequest, camera, tone: RaytraceTone,
           exposure: float | None):
    """One frame: (8-bit picture, the data it was made from, the exposure
    before ``ev``)."""
    from ..particle_raytrace import Camera, tone_map  # noqa: PLC0415
    from ..volume_raytrace import default_step, projected_maps, render  # noqa: PLC0415
    cam = Camera(**camera.model_dump())
    step = 2.0 * default_step(grids["emission"].shape, grids["box"]) / req.quality
    if req.mode == "projected":
        maps = projected_maps(grids["emission"], grids["index"], cam, req.width, req.height,
                              box=grids["box"], slab=req.slab, step=step)
        by_density = req.colour_by == "density"
        lut = grids["density_lut"] if by_density else grids["lut"]
        return _projected_image(maps, lut, tone, by_density), maps, 1.0
    light = render(grids["emission"], grids["index"], grids["lut"], cam, req.width, req.height,
                   box=grids["box"], absorption=req.absorption, step=step)
    if exposure is None:
        exposure = _auto_exposure(light)
    params = tone.model_dump(exclude={"exposure", "ev", "decades"})
    return (tone_map(light, exposure=exposure * 2.0 ** tone.ev, **params), light,
            float(exposure))


def regrade(light: np.ndarray, req: VolumeRenderRequest, tone: RaytraceTone) -> np.ndarray:
    """A still graded again (the tone route): a projected map's from its
    maps, the table rebuilt from the request."""
    if req.mode == "projected":
        by_density = req.colour_by == "density"
        colour = RaytraceColour(lut=req.colour.lut) if by_density else req.colour
        lut = _lut(colour, *(_saved_range(req)), colour.log and _colour_log(req))
        return _projected_image(light, lut, tone, by_density)
    from ..particle_raytrace import tone_map  # noqa: PLC0415
    params = tone.model_dump(exclude={"exposure", "ev", "decades"})
    exposure = tone.exposure if tone.exposure is not None else _auto_exposure(light)
    return tone_map(light, exposure=exposure * 2.0 ** tone.ev, **params)


def _colour_log(req: VolumeRenderRequest) -> bool:
    """Whether the colour's scale is logarithmic: never a velocity's, which
    is signed."""
    return req.colour.log and req.colour_by != "velocity"


def _saved_range(req: VolumeRenderRequest) -> tuple[float, float]:
    lo = req.colour.min if req.colour.min is not None else 0.0
    hi = req.colour.max if req.colour.max is not None else 1.0
    return lo, hi


@router.post("/v1/tasks/volume_render", response_model=TaskCreateResponse)
async def create_volume_render_task(request: VolumeRenderRequest, token: str = _auth,
                                    session: Session = Depends(get_session)) -> TaskCreateResponse:
    """Stills (one camera) and movie frames (several) of a cube, rendered as
    a task; its results at /v1/particle_render/{task_id}/… ."""
    entry = _require_dataset(session, request.dataset_id)
    _paths(entry, request)   # refused now, not in the task

    def prepare() -> dict:
        grids = _prepare(entry, request)
        # The range the still's colours were made over, so the tone route
        # rebuilds the same table.
        request.colour = request.colour.model_copy(
            update={"min": grids["range"][0], "max": grids["range"][1]})
        return grids

    return start_render_task(
        "volume_render", token,
        lambda task_id, stop: run_frames(
            task_id, request, stop, prepare,
            lambda grids, cam, exposure: _frame(grids, request, cam, request.tone, exposure),
            lambda grids: {"voxels": int(grids["emission"].size),
                           "colour_range": list(grids["range"])}))


# The grids the GPU view was last sent, or the preview last drew from, per
# client and what shapes them: the tone and the camera change nothing here.
# One preparation at a time: the asyncio lock queues the requests, the thread
# lock serialises the work, which runs on after its request is abandoned.
_PREPARED: OrderedDict[str, dict] = OrderedDict()
_PREPARED_KEEP = 2
_PREPARE_LOCK = asyncio.Lock()
_PREPARE_WORK = threading.Lock()
_GRID_FIELDS = ("dataset_id", "box", "brightness_log10", "brightness_power",
                "brightness_floor", "colour_by",
                "colour_field", "colour", "max_voxels", "max_axis")


def _accepts_gzip(accept_encoding: str) -> bool:
    """Whether an Accept-Encoding takes gzip: named (or ``*``) with a
    quality above 0."""
    q = {}
    for part in accept_encoding.lower().split(","):
        name, *params = (p.strip() for p in part.split(";"))
        weight = 1.0
        for param in params:
            if param.startswith("q="):
                try:
                    weight = float(param[2:])
                except ValueError:
                    weight = 0.0
        if name:
            q[name] = weight
    return q.get("gzip", q.get("*", 0.0)) > 0.0


def _loopback(host: str) -> bool:
    import ipaddress  # noqa: PLC0415
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return host == "localhost"


def _cached(entry: dict, req: VolumeGridRequest, token: str) -> dict:
    """The grids ``req`` shapes, prepared once per client (under
    ``_PREPARE_WORK``)."""
    key = token + json.dumps(req.model_dump(include=set(_GRID_FIELDS)), sort_keys=True,
                             default=str)
    grids = _PREPARED.get(key)
    if grids is None:
        grids = _prepare(entry, req)
        _PREPARED[key] = grids
        while len(_PREPARED) > _PREPARED_KEEP:
            _PREPARED.popitem(last=False)
    else:
        _PREPARED.move_to_end(key)
    return grids


@router.post("/v1/volume_render/grid")
async def volume_render_grid(request: VolumeGridRequest, http_request: Request,
                             token: str = _auth,
                             session: Session = Depends(get_session)) -> Response:
    """The grids the desktop's GPU view marches, prepared as for a render:
    little-endian, x fastest, the emissivity as float32 (nz·ny·nx), then the
    colour index as float16 (the same count), then the colour table as
    float32 RGB (256 entries). The grid's shape in ``X-Visivo-Volume-Shape``
    (nz,ny,nx), the world box it spans in ``X-Visivo-Volume-Box``, the
    colour's data range in ``X-Visivo-Colour-Range`` (lo,hi,log).

    Gzipped for a client on another machine that accepts it: what lies below
    the brightness floor is zero, a radio cube's noise most of the grid, and
    a remote desktop fetches it through a tunnel of a few MB/s. Not on this
    machine, where packing would only cost time."""
    entry = _require_dataset(session, request.dataset_id)
    _paths(entry, request)
    host = http_request.client.host if http_request.client else ""
    gzip = _accepts_gzip(http_request.headers.get("accept-encoding", "")) and not _loopback(host)

    def pack() -> tuple[bytes, dict]:
        with _PREPARE_WORK:
            grids = _cached(entry, request, token)
        e = np.ascontiguousarray(grids["emission"], dtype="<f4")
        c = np.ascontiguousarray(grids["index"], dtype="<f2")
        lut = np.ascontiguousarray(grids["lut"], dtype="<f4")
        nz, ny, nx = e.shape
        lo, hi = grids["range"]
        headers = {X_VOLUME_SHAPE: f"{nz},{ny},{nx}",
                   X_VOLUME_BOX: ",".join(repr(float(v)) for v in grids["box"]),
                   X_COLOUR_RANGE: f"{lo!r},{hi!r},{int(bool(grids['log']))}"}
        # One copy into the reply, not one per part and another to join them.
        body = b"".join((memoryview(e).cast("B"), memoryview(c).cast("B"),
                         memoryview(lut).cast("B")))
        if gzip:
            # Level 1: most of what it saves, at a few hundred MB/s.
            packer = zlib.compressobj(1, zlib.DEFLATED, 16 + zlib.MAX_WBITS)
            body = packer.compress(body) + packer.flush()
            headers["Content-Encoding"] = "gzip"
        return body, headers

    async with _PREPARE_LOCK:
        t0 = time.time()
        try:
            body, headers = await asyncio.to_thread(pack)
        except ValueError as exc:
            raise HTTPException(422, str(exc)) from exc
    headers[X_RENDER_MS] = f"{(time.time() - t0) * 1000.0:.0f}"
    return Response(body, media_type="application/octet-stream", headers=headers)


class VolumePreviewRequest(VolumeGridRequest):
    cameras: list[RaytraceCamera] = Field(..., min_length=1, max_length=1)
    width: int = Field(640, ge=16, le=2048)
    height: int = Field(360, ge=16, le=2048)


@router.post("/v1/volume_render/preview")
async def volume_render_preview(request: VolumePreviewRequest, token: str = _auth,
                                session: Session = Depends(get_session)) -> Response:
    """One frame of the view, for a desktop without a GPU view: a JPEG,
    marched through the grid the GPU view would have been sent (prepared once,
    then only the camera and the tone change), with its exposure before ``ev``
    (``X-Visivo-Exposure``). What the desktop draws over it, as over its GPU
    view: the grid's box (``X-Visivo-Volume-Box``), the colour's range
    (``X-Visivo-Colour-Range``, lo,hi,log) and table (``X-Visivo-Colour-Table``:
    256 RGB colours, 8-bit, in hex)."""
    entry = _require_dataset(session, request.dataset_id)
    _paths(entry, request)

    def frame() -> tuple[bytes, float, dict]:
        with _PREPARE_WORK:
            grids = _cached(entry, request, token)
            rgb, _, exposure = _frame(grids, request, request.cameras[0], request.tone,
                                      request.tone.exposure)
        from PIL import Image  # noqa: PLC0415
        buf = io.BytesIO()
        Image.fromarray(rgb).save(buf, format="JPEG", quality=88)
        lo, hi = grids["range"]
        table = (np.clip(grids["lut"], 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)
        headers = {X_VOLUME_BOX: ",".join(repr(float(v)) for v in grids["box"]),
                   X_COLOUR_RANGE: f"{lo!r},{hi!r},{int(bool(grids['log']))}",
                   X_COLOUR_TABLE: table.tobytes().hex()}
        return buf.getvalue(), exposure, headers

    async with _PREPARE_LOCK:
        t0 = time.time()
        try:
            jpeg, exposure, headers = await asyncio.to_thread(frame)
        except ValueError as exc:
            raise HTTPException(422, str(exc)) from exc
    headers[X_EXPOSURE] = repr(float(exposure))
    headers[X_RENDER_MS] = f"{(time.time() - t0) * 1000.0:.0f}"
    return Response(jpeg, media_type="image/jpeg", headers=headers)
