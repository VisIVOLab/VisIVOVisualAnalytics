"""Data for the VR page's other layers (issue #190, VR epic #166): point
tables sampled for a headset, catalogue sources placed in a cube's pixels,
and a velocity field's streamlines and arrows. Every computation is on the
backend; the page only draws (docs/backend-api/vr-layers.md).
"""
from __future__ import annotations

import asyncio
import logging
import threading
from pathlib import Path
from typing import Literal

from fastapi import APIRouter, Depends, HTTPException, Response
from pydantic import BaseModel, Field

from ..auth import verify_token
from ..dependencies import _require_dataset, _run, _run_io
from ..sessions import Session, get_session
from .velocity import _effective_box

router = APIRouter(tags=["vr"])
logger = logging.getLogger("visivo.vr_layers")
_auth = Depends(verify_token)


# ── Point tables ─────────────────────────────────────────────────────────────


def _point_spec(entry: dict) -> str:
    from .render import point_table_spec   # noqa: PLC0415
    spec = point_table_spec(entry)
    if spec is None:
        raise HTTPException(422, "Not a table of points (a VBT point table or a catalogue).")
    return spec


#: Only hands out each dataset's own build lock (held for microseconds).
_LOCKS_LOCK = threading.Lock()
#: Sample requests run at most this many at a time (each holds up to about
#: 0.4 GB at 10 M rows: tests/test_vr_layers.py measures it). Waited for in
#: the coroutine, before a worker thread is taken: queued requests never hold
#: threads of the shared pool.
SAMPLE_CONCURRENCY = 2
_SAMPLES: dict = {}


def _sample_slots() -> asyncio.Semaphore:
    """The sample semaphore of the running event loop (one per loop: an
    asyncio semaphore belongs to the loop it first waits on)."""
    loop = asyncio.get_running_loop()
    sem = _SAMPLES.get(loop)
    if sem is None:
        _SAMPLES.clear()            # a former loop's (tests) is of no use now
        sem = _SAMPLES[loop] = asyncio.Semaphore(SAMPLE_CONCURRENCY)
    return sem


def _build_lock(entry: dict) -> threading.Lock:
    """The dataset's own lock for building its table: two datasets build at
    once, two requests for one build it once."""
    with _LOCKS_LOCK:
        lock = entry.get("_point_table_lock")
        if lock is None:
            lock = entry["_point_table_lock"] = threading.Lock()
        return lock


def _point_table(entry: dict, spec: str, sky: dict | None = None):
    """The table of points of a dataset. A catalogue's is built once from the
    dataframe the session already holds (not read again) and kept in its
    dataset entry, so it goes when the dataset is closed (or its session
    expires); a VBT is a memory map, opened each time at no cost.

    ``sky`` places a sky catalogue as the desktop window does (issue #260):
    its frame, distance mode and cosmology. The table kept is the last one
    asked for: another placement builds it again."""
    from ..point_table import open_point_table, sky_arguments, table_from_frame   # noqa: PLC0415
    frame = entry.get("dataframe")
    if not spec.startswith("{") or frame is None:
        table = open_point_table(spec)
        if sky and table.sky is not None:
            table.set_sky_positions(*sky_arguments(sky))
        return table
    key = (spec, sky_arguments(sky))
    with _build_lock(entry):
        kept = entry.get("_point_table")
        if kept is not None and kept[0] == key:
            return kept[1]
        import json   # noqa: PLC0415
        table = table_from_frame(frame, json.loads(spec).get("coordinates") or {}, sky)
        # Its derived columns are built: the frame is the entry's own, not
        # held a second time.
        table.frame = None
        entry["_point_table"] = (key, table)
        return table


def _sky_headers(table) -> dict[str, str]:
    """A sky catalogue's placement in a sample's headers: what the page shows
    in its status line (docs/backend-api/vr-layers.md)."""
    if table.sky is None or not table.sky_settings:
        return {}
    st = table.sky_settings
    return {"X-Visivo-Sky": f"{st['frame']},{st['distance_mode']},{st['cosmology']}",
            "X-Visivo-Unplaced": str(st.get("unplaced", 0)),
            "X-Visivo-Uncertain": str(st.get("uncertain", 0))}


#: What a table that cannot be read, or a malformed request, raises: a 422.
_BAD_TABLE = (ValueError, TypeError, KeyError, OSError)


class PointInfoRequest(BaseModel):
    dataset_id: str
    # A sky catalogue's placement, the desktop window's (issue #260):
    # {"frame": fk5 | galactic | supergalactic, "distance_mode": comoving |
    # redshift_space, "cosmology": a named model}; null: fk5, comoving, Planck18.
    sky: dict | None = None


@router.post("/v1/points/info", tags=["vr"])
async def points_info(request: PointInfoRequest, _: None = _auth,
                      session: Session = Depends(get_session)) -> dict:
    """The table's numeric columns, its default X/Y/Z, its row count and
    its format: ``vbt`` (a simulation's particles, which the page opens
    ray-traced, issue #264) or ``catalogue``; for a sky catalogue, how it is
    placed and how many sources have no distance."""
    entry = _require_dataset(session, request.dataset_id)
    spec = _point_spec(entry)
    try:
        table = await asyncio.to_thread(_point_table, entry, spec, request.sky)
    except _BAD_TABLE as exc:
        raise HTTPException(422, f"The table could not be read: {exc}") from exc
    # A catalogue's column units, as its file declares them (issue #259: the
    # colour bar's unit); a VBT's header carries none.
    frame = entry.get("dataframe")
    declared = ((getattr(frame, "attrs", None) or {}).get("units") or {}) if frame is not None else {}
    units = {k: str(v) for k, v in dict(declared).items() if k in table.columns and v not in (None, "")}
    out = {"valid": True, "num_rows": int(table.num_rows), "fields": list(table.columns),
           "axes": list(table.axes), "sky": table.sky is not None, "units": units,
           # point_table_spec: a VBT's header path, a catalogue's JSON spec.
           "format": "catalogue" if spec.lstrip().startswith("{") else "vbt"}
    if table.sky is not None:
        # Its frame, distance mode and cosmology, and the sources placed, left
        # out for want of a distance, and placed from cz < 1000 km/s (#265).
        out["sky_settings"] = dict(table.sky_settings)
    return out


@router.post("/v1/points/series", tags=["vr"])
async def points_series(request: PointInfoRequest, _: None = _auth,
                        session: Session = Depends(get_session)) -> dict:
    """The snapshots beside the table's file (issue #259, app/snapshot_series.py):
    the files in its folder whose names differ from it only in their number,
    in order, and where this one is. Only names are read."""
    from ..snapshot_series import snapshot_series   # noqa: PLC0415
    entry = _require_dataset(session, request.dataset_id)
    _point_spec(entry)                                  # a table of points, else 422
    path = entry.get("head_path") if entry.get("kind") == "vbt" else entry.get("path")
    if not path:
        raise HTTPException(422, "The table has no file to step from.")
    from ..dependencies import _normalize_path   # noqa: PLC0415

    def allowed(member: str) -> bool:
        # A member is opened by path later: one the data roots refuse (a
        # symbolic link resolving outside them, a hidden name) is not listed.
        try:
            _normalize_path(member)
        except (PermissionError, OSError, ValueError):
            return False
        return True
    return {"valid": True, **await asyncio.to_thread(snapshot_series, str(path), allowed)}


class PointSampleRequest(BaseModel):
    dataset_id: str
    x: str | None = None
    y: str | None = None
    z: str | None = None
    color_field: str | None = None
    color_log: bool | None = False     # null: the backend chooses (AUTO_LOG_DECADES)
    color_min: float | None = None
    color_max: float | None = None
    max_points: int = Field(2_000_000, ge=1, le=10_000_000)
    filters: list[dict] = []
    # Each row's size from this column (a smoothing length, issue #259):
    # 16-bit codes of its log10 after the colours, X-Visivo-Size-Range.
    size_field: str | None = None
    sky: dict | None = None            # as PointInfoRequest.sky


@router.post("/v1/points/sample_bin", tags=["vr"])
async def points_sample_bin(request: PointSampleRequest, _: None = _auth,
                            session: Session = Depends(get_session)) -> Response:
    """An even sample of the table's rows within ``max_points``, quantised to
    16 bits (app/point_sample.py)."""
    from ..point_sample import headers, sample_points   # noqa: PLC0415
    entry = _require_dataset(session, request.dataset_id)
    spec = _point_spec(entry)
    axes = None
    if request.x or request.y or request.z:
        axes = (request.x, request.y, request.z)

    def work():
        table = _point_table(entry, spec, request.sky)
        chosen = tuple(a or d for a, d in zip(axes, table.axes)) if axes else None
        s = sample_points(table, request.max_points, chosen, request.color_field,
                          request.color_log, request.color_min, request.color_max,
                          request.filters, request.size_field)
        return s.encode(), {**headers(s), **_sky_headers(table)}

    try:
        async with _sample_slots():     # at most SAMPLE_CONCURRENCY at a time
            body, h = await asyncio.to_thread(work)
    except _BAD_TABLE as exc:
        raise HTTPException(422, str(exc)) from exc
    h = {k: v.encode("latin-1", "replace").decode("latin-1") for k, v in h.items()}
    return Response(body, media_type="application/octet-stream", headers=h)


# ── Catalogue sources in a cube ──────────────────────────────────────────────


class CubeCatalogueRequest(BaseModel):
    dataset_id: str                      # the cube
    # "catalogue": a catalogue open in this session (catalogue_id);
    # "vlkb_sources" / "vlkb_filaments": the VLKB over the cube's footprint;
    # "crossmatch": an external catalogue over it (SIMBAD, 2MASS, NVSS, FIRST).
    source: str = "catalogue"
    catalogue_id: str = ""
    label_field: str | None = None
    spectral_field: str | None = None    # None: freq, v_rad, v_opt, vlsr... when present
    spectral_unit: str | None = None
    # radio | optical | relativistic; null: the column name's (v_rad, v_opt...),
    # or the axis's own for v_app and for a column of no stated convention.
    spectral_convention: str | None = None
    bands: list[str] = ["ft"]            # VLKB compact sources: ft = band-merged
    service_url: str = ""                # VLKB: "" = the backend's configured one
    crossmatch_catalogue: str = "simbad"
    radius_arcsec: float = Field(60.0, ge=0.0)
    max_sources: int = Field(2000, ge=1, le=20000)


#: Rows a VLKB query returns at most (TAP MAXREC).
VLKB_MAX_ROWS = 20_000


def _vlkb_rows(service_url: str, adql: str, max_rows: int) -> tuple[list[dict], bool]:
    """A VLKB TAP query's rows by column name, and whether the row limit was
    hit. The CSV goes through a temporary directory of its own, removed after."""
    import csv   # noqa: PLC0415
    import tempfile   # noqa: PLC0415
    from .. import vlkb_tap   # noqa: PLC0415
    with tempfile.TemporaryDirectory(prefix="visivo-vlkb-") as tmp:
        result = vlkb_tap.run_query(service_url, adql, str(Path(tmp) / "vlkb.csv"), max_rows)
        path = result.get("output_path")
        if not path:
            return [], False
        with open(path, newline="", encoding="utf-8") as fh:
            return [dict(r) for r in csv.DictReader(fh)], bool(result.get("hit_row_limit"))


def _vlkb_query(kind: str, box: tuple[float, float, float, float]) -> str:
    """The desktop's own ADQL (runVlkbCatalogueQuery) over a Galactic box,
    in two longitude ranges across l = 0 (cube_catalogue.vlkb_where)."""
    from ..cube_catalogue import vlkb_where   # noqa: PLC0415
    if kind == "vlkb_filaments":
        return ("SELECT idfil,contour FROM filaments.filaments WHERE "
                + vlkb_where("glon", "glat", box))
    return ("SELECT * FROM compactsources.sed_view_final WHERE "
            + vlkb_where("glonft", "glatft", box))


@router.post("/v1/cube/catalogue_pixels", tags=["vr"])
async def cube_catalogue_pixels(request: CubeCatalogueRequest, _: None = _auth,
                                session: Session = Depends(get_session)) -> dict:
    """Catalogue sources placed in the cube's pixels with its full WCS
    (app/cube_catalogue.py): markers, ellipses and contours, 0-based pixels."""
    from .. import cube_catalogue as cc   # noqa: PLC0415
    entry = _require_dataset(session, request.dataset_id)
    if entry.get("kind") not in ("cube", "image"):
        raise HTTPException(422, "Catalogue sources are placed over a cube or an image.")
    path = str(entry["path"])
    try:
        geo = await asyncio.to_thread(cc.cube_geometry, path)
    except ValueError as exc:
        raise HTTPException(422, str(exc)) from exc
    frame = cc.celestial_frame(geo.cel)
    arcpix = cc.pixel_scale_arcsec(geo.cel)
    limit = request.max_sources
    try:
        if request.source == "catalogue":
            cat = session.datasets.get(request.catalogue_id)
            if not cat or cat.get("dataframe") is None:
                raise HTTPException(404, f"Unknown catalogue {request.catalogue_id!r}: "
                                         "open it with /v1/catalogue/open first.")
            df = cat["dataframe"]
            label = request.label_field or cc.label_column(df)
            placed = await asyncio.to_thread(
                cc.place_table, df, cat.get("coordinate_fields") or {}, geo.cel, geo.wcs,
                geo.shape, label, request.spectral_field, request.spectral_unit,
                request.spectral_convention, limit)
            name = Path(str(cat.get("path", ""))).name
        elif request.source in ("vlkb_sources", "vlkb_filaments"):
            box = cc.galactic_footprint(geo.cel, geo.shape[0], geo.shape[1])
            from .. import vlkb_tap   # noqa: PLC0415
            try:
                rows, hit_limit = await _run_io(_vlkb_rows, request.service_url,
                                                _vlkb_query(request.source, box), VLKB_MAX_ROWS)
            except vlkb_tap.VlkbQueryError as exc:
                return {"valid": False, "error": str(exc), "source": request.source}
            # Off the event loop: thousands of rows projected.
            if request.source == "vlkb_filaments":
                placed = await asyncio.to_thread(cc.place_contours, rows, geo.cel, geo.shape,
                                                 "filament", False, limit)
            else:
                bands = tuple(b for b in request.bands if b in cc.VLKB_BANDS) or ("ft",)
                placed = await asyncio.to_thread(cc.place_vlkb_sources, rows, geo.cel,
                                                 geo.shape, bands, limit)
            if hit_limit:
                placed.warnings.append(f"the VLKB answered its first {VLKB_MAX_ROWS} rows only")
            name = "VLKB " + ("filaments" if request.source == "vlkb_filaments"
                              else "compact sources")
        elif request.source == "crossmatch":
            from ..compute import worker_crossmatch   # noqa: PLC0415
            import pandas as pd   # noqa: PLC0415
            try:
                res = await asyncio.to_thread(worker_crossmatch, path, request.radius_arcsec,
                                              request.crossmatch_catalogue, limit)
            except ImportError as exc:
                raise HTTPException(503, str(exc)) from exc
            srcs = res.get("sources") or []
            df = pd.DataFrame({"ra": [s.get("ra_deg") for s in srcs],
                               "dec": [s.get("dec_deg") for s in srcs],
                               "name": [str(s.get("name") or s.get("id") or "") for s in srcs]})
            placed = await asyncio.to_thread(
                cc.place_table, df, {"system": "sky", "ra": "ra", "dec": "dec"}, geo.cel,
                geo.wcs, geo.shape, "name", "", None, None, limit)
            name = f"cross-match {request.crossmatch_catalogue}"
        else:
            raise HTTPException(422, f"Unknown source {request.source!r}.")
    except HTTPException:
        raise
    except ValueError as exc:
        raise HTTPException(422, str(exc)) from exc
    out = cc.as_response(placed, request.source, frame, arcpix)
    out["name"] = name
    out["shape"] = list(geo.shape)
    return out


# ── A catalogue classified in the headset (issue #279) ───────────────────────


class ClassifiedSource(BaseModel):
    # catalogue: a row of the session's catalogue; overlay: a source of an
    # overlay with no table behind it (VLKB, a cross-match); user: found by eye.
    origin: Literal["catalogue", "overlay", "user"] = "catalogue"
    row: int | None = Field(default=None, ge=0)
    label: str = Field(default="", max_length=200)
    lon: float | None = Field(default=None, allow_inf_nan=False)
    lat: float | None = Field(default=None, allow_inf_nan=False)
    frame: Literal["icrs", "galactic"] | None = None     # of lon/lat; None: source_frame
    x: float | None = Field(default=None, allow_inf_nan=False)
    y: float | None = Field(default=None, allow_inf_nan=False)
    z: float | None = Field(default=None, allow_inf_nan=False)
    velocity_kms: float | None = Field(default=None, allow_inf_nan=False)
    flag: str = Field(default="", max_length=32)
    note: str = Field(default="", max_length=64)


class ClassifiedVotableRequest(BaseModel):
    dataset_id: str                      # the cube the catalogue is placed over
    catalogue_id: str = ""               # a catalogue of the session: its columns kept
    source_frame: Literal["icrs", "galactic"] = "icrs"   # of the sources' lon/lat
    flag_set: list[str] = Field(default_factory=list, max_length=8)
    flag_column: str = Field(default="vr_flag", pattern=r"^[A-Za-z_][A-Za-z0-9_]{0,31}$")
    sources: list[ClassifiedSource] = Field(default_factory=list, max_length=20000)
    output_basename: str = Field(default="", max_length=200)


class ClassifiedVotableResponse(BaseModel):
    valid: bool
    error: str = ""
    output_path: str = ""
    workspace_filename: str = ""
    n_rows: int = 0
    n_flagged: int = 0
    n_user: int = 0
    columns: list[str] = []


@router.post("/v1/catalogue/classified_votable", response_model=ClassifiedVotableResponse, tags=["vr"])
async def catalogue_classified_votable(request: ClassifiedVotableRequest, _: None = _auth,
                                       session: Session = Depends(get_session)) -> ClassifiedVotableResponse:
    """The headset's classification of a catalogue (app/catalogue_classified.py)
    written as a VOTable in the Workspace Exports: the catalogue's own columns
    and rows (when it is a catalogue of this session) plus the flag column,
    vr_note, vr_origin, vr_x/y/z and vr_velocity_kms; the sources found by eye
    appended as rows. The desktop opens it as a catalogue (issue #279)."""
    from .. import catalogue_classified as ccl   # noqa: PLC0415
    from ..dependencies import make_workspace_path   # noqa: PLC0415
    entry = _require_dataset(session, request.dataset_id)
    frame = coords = label = None
    if request.catalogue_id:
        cat = session.datasets.get(request.catalogue_id)
        if not cat or cat.get("dataframe") is None:
            raise HTTPException(404, f"Unknown catalogue {request.catalogue_id!r}: "
                                     "open it with /v1/catalogue/open first.")
        frame = cat["dataframe"]
        coords = cat.get("coordinate_fields") or {}
        from .. import cube_catalogue as cc   # noqa: PLC0415
        label = cc.label_column(frame)
        stem_from = Path(str(cat.get("path", "") or "catalogue")).stem
    else:
        stem_from = f"{Path(str(entry.get('path', '') or 'cube')).stem}_sources"
    stem = Path(request.output_basename.strip()).stem if request.output_basename.strip() else f"{stem_from}_vr_classified"
    sources = [s.model_dump() for s in request.sources]
    try:
        table = await asyncio.to_thread(ccl.classified_table, sources, request.flag_column, frame,
                                        coords, label, request.source_frame)
    except ValueError as exc:
        raise HTTPException(422, str(exc)) from exc
    try:
        path = make_workspace_path(stem + ".xml", reserve=True)
    except Exception as exc:   # noqa: BLE001 - said to the page
        return ClassifiedVotableResponse(valid=False, error=str(exc))
    meta = {"cube": Path(str(entry.get("path", ""))).name,
            "flag_set": ", ".join(f.strip() for f in request.flag_set if f.strip()),
            "made_in": "the VR headset (VisIVO Visual Analytics)"}
    try:
        await asyncio.to_thread(ccl.write_votable, table, str(path), meta)
    except Exception as exc:   # noqa: BLE001 - the placeholder goes, the page is told
        path.unlink(missing_ok=True)
        logger.warning("[classified_votable] failed: %s", exc)
        return ClassifiedVotableResponse(valid=False, error=str(exc))
    flags = [s for s in sources if s.get("flag")]
    return ClassifiedVotableResponse(
        valid=True, output_path=str(path), workspace_filename=path.name, n_rows=len(table),
        n_flagged=len(flags), n_user=sum(1 for s in sources if s.get("origin") == "user"),
        columns=list(table.colnames))


# ── Velocity fields ──────────────────────────────────────────────────────────


class VelocityLinesRequest(BaseModel):
    dataset_id: str
    # The box the desktop shows (Mpc): omitted, the one the field was opened
    # with; 0, unit cells. See _effective_box.
    box_mpc: float | None = Field(default=None, ge=0, allow_inf_nan=False)
    max_dim: int = Field(128, ge=0, le=1024)     # the desktop's display grid
    n_seeds: int = Field(200, ge=1, le=5000)
    max_arrows: int = Field(50_000, ge=1, le=500_000)
    seed: int = Field(1, ge=0, le=2 ** 31 - 1)
    lines: bool = True
    arrows: bool = True


@router.post("/v1/velocity/lines_bin", tags=["vr"])
async def velocity_lines_bin(request: VelocityLinesRequest, _: None = _auth,
                             session: Session = Depends(get_session)) -> Response:
    """Streamlines and arrows of the display grid, traced as the desktop
    traces them (app/velocity_lines.py)."""
    from ..velocity_lines import flow_lines_worker, headers   # noqa: PLC0415
    entry = session.datasets.get(request.dataset_id)
    if not entry or entry.get("kind") != "velocity":
        return Response(b"unknown velocity dataset", media_type="text/plain", status_code=404)
    try:
        result = await _run(flow_lines_worker, entry["path"],
                            _effective_box(request.box_mpc, entry),
                            request.max_dim, request.n_seeds, request.max_arrows,
                            request.seed, request.lines, request.arrows)
    except Exception as exc:   # noqa: BLE001 - as the other velocity routes
        logger.warning("[velocity-lines] failed: %s", exc)
        return Response(str(exc).encode("utf-8"), media_type="text/plain", status_code=500)
    return Response(result["bytes"], media_type="application/octet-stream",
                    headers=headers(result))
