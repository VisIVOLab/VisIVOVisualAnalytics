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


def _point_table(entry: dict, spec: str):
    """The table of points of a dataset. A catalogue's is built once from the
    dataframe the session already holds (not read again) and kept in its
    dataset entry, so it goes when the dataset is closed (or its session
    expires); a VBT is a memory map, opened each time at no cost."""
    from ..point_table import open_point_table, table_from_frame   # noqa: PLC0415
    frame = entry.get("dataframe")
    if not spec.startswith("{") or frame is None:
        return open_point_table(spec)
    with _build_lock(entry):
        kept = entry.get("_point_table")
        if kept is not None and kept[0] == spec:
            return kept[1]
        import json   # noqa: PLC0415
        table = table_from_frame(frame, json.loads(spec).get("coordinates") or {})
        # Its derived columns are built: the frame is the entry's own, not
        # held a second time.
        table.frame = None
        entry["_point_table"] = (spec, table)
        return table


#: What a table that cannot be read, or a malformed request, raises: a 422.
_BAD_TABLE = (ValueError, TypeError, KeyError, OSError)


class PointInfoRequest(BaseModel):
    dataset_id: str


@router.post("/v1/points/info", tags=["vr"])
async def points_info(request: PointInfoRequest, _: None = _auth,
                      session: Session = Depends(get_session)) -> dict:
    """The table's numeric columns, its default X/Y/Z and its row count."""
    entry = _require_dataset(session, request.dataset_id)
    spec = _point_spec(entry)
    try:
        table = await asyncio.to_thread(_point_table, entry, spec)
    except _BAD_TABLE as exc:
        raise HTTPException(422, f"The table could not be read: {exc}") from exc
    return {"valid": True, "num_rows": int(table.num_rows), "fields": list(table.columns),
            "axes": list(table.axes), "sky": table.sky is not None}


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
        table = _point_table(entry, spec)
        chosen = tuple(a or d for a, d in zip(axes, table.axes)) if axes else None
        s = sample_points(table, request.max_points, chosen, request.color_field,
                          request.color_log, request.color_min, request.color_max,
                          request.filters)
        return s.encode(), headers(s)

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
