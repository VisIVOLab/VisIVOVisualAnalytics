"""IVOA SODA on the node: subsets and products computed where the data are.

In the SRCNet architecture SODA is a node-local service: the file never leaves
the node, only the subset (or the product) does. This router is that service
for a node running the VisIVO backend.

* ``/soda/sync`` (GET or POST, SODA 1.0 sync): ``ID``, ``POS`` (CIRCLE, RANGE,
  POLYGON), ``BAND`` (metres), ``TIME``; answers with the FITS cutout.
* ``/soda/async`` (UWS 1.1 job list): the same cutout as an asynchronous job,
  plus two operations SODA leaves to the service, selected with the custom
  parameter ``OPERATION``:

  - ``OPERATION=MOMENT``: ``MOMENT=<order>`` (0, 1, 2, ...), optional
    ``CHANNELS=<first> <last>`` (0-based, inclusive) and ``THRESHOLD=<value>``
    (mask below it); the result is the 2-D moment map as FITS.
  - ``OPERATION=PV``: ``PVPATH=<ra1> <dec1> <ra2> <dec2> [...]`` (degrees, the
    path's vertices) and ``PVWIDTH=<pixels>``; the result is the
    position-velocity diagram as FITS.

``ID`` is the replica's own identifier on this node, the ``file://`` URL that
SKAVA's DataLink hands out for it; it must lie inside the node's data roots.
Errors follow DALI: ``text/plain`` bodies starting with ``UsageError:`` (bad
request) or ``FatalError:`` (the service failed).

Results are written in the backend's workspace. The UWS result link streams
the file; a client of this backend (the desktop) can instead open it in place:
the JSON form of a job (``Accept: application/json``) carries ``output_path``.
"""
from __future__ import annotations

import asyncio
import io
import json
import logging
import shutil
import uuid
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any
from urllib.parse import parse_qsl
from xml.sax.saxutils import escape, quoteattr

from fastapi import APIRouter, Depends, Request
from fastapi.responses import FileResponse, PlainTextResponse, RedirectResponse, Response, StreamingResponse

from ..auth import verify_token
from ..dependencies import _FITS_SUFFIXES, _is_supported_dataset, _normalize_path, _run_heavy, get_exports_dir
from ..skava_client import SkavaResolveError, access_url_to_local_path
from ..soda_cutout import CutoutError, CutoutNoOverlap, make_cutout
from ..tasks import TASKS
from .tasks import _owned_task

router = APIRouter(tags=["soda"])
logger = logging.getLogger("visivo.backend")
_auth = Depends(verify_token)

_UWS = "http://www.ivoa.net/xml/UWS/v1.0"
_XLINK = "http://www.w3.org/1999/xlink"
_XSI = "http://www.w3.org/2001/XMLSchema-instance"
_FITS_FORMATS = {"application/fits", "image/fits", "application/x-fits"}
_OPERATIONS = ("CUTOUT", "MOMENT", "PV")
# The parameters each operation understands; anything else is refused, so a
# mistyped name is a UsageError rather than a silently ignored request.
_ALLOWED = {
    "CUTOUT": {"ID", "POS", "CIRCLE", "POLYGON", "BAND", "TIME", "RESPONSEFORMAT"},
    "MOMENT": {"ID", "MOMENT", "CHANNELS", "THRESHOLD", "RESPONSEFORMAT"},
    "PV": {"ID", "PVPATH", "PVWIDTH", "RESPONSEFORMAT"},
}
_CONTROL = {"OPERATION", "PHASE", "RUNID", "REQUEST"}

# UWS phase for each task registry status. A job created without PHASE=RUN is
# "held": PENDING for UWS, and not counted as queued work in the node status
# (the registry's "pending" means started but not yet running).
_PHASE = {"held": "PENDING", "pending": "QUEUED", "running": "EXECUTING", "completed": "COMPLETED",
          "failed": "ERROR", "cancelled": "ABORTED"}


class SodaError(Exception):
    def __init__(self, message: str, *, kind: str = "UsageError", status: int = 400):
        super().__init__(message)
        self.kind, self.status = kind, status

    def response(self) -> PlainTextResponse:
        return PlainTextResponse(f"{self.kind}: {self}", status_code=self.status)


async def _params(request: Request) -> dict[str, list[str]]:
    """Query and form parameters; names are case-insensitive (DALI), values are not."""
    items = list(request.query_params.multi_items())
    if request.method == "POST":
        content_type = request.headers.get("content-type", "")
        if content_type.startswith("application/x-www-form-urlencoded"):
            body = (await request.body()).decode("utf-8", errors="replace")
            items += parse_qsl(body, keep_blank_values=True)
        elif content_type.startswith("multipart/"):
            raise SodaError("send parameters URL-encoded (application/x-www-form-urlencoded)")
    out: dict[str, list[str]] = {}
    for key, value in items:
        out.setdefault(key.upper(), []).append(value)
    return out


def _one(params: dict[str, list[str]], name: str) -> str | None:
    values = [v for v in params.get(name, []) if v.strip()]
    if len(values) > 1:
        raise SodaError(f"one {name} per request is supported (got {len(values)})",
                        kind="MultiValuedParamNotSupported")
    return values[0].strip() if values else None


def _resolve(dataset_id: str | None, *, any_file: bool = False) -> Path:
    """The local file an ID names, inside the node's data roots. ``any_file``
    accepts any regular file there (a compressed FITS, a PSRFITS search file),
    for a request that returns the file as stored and never reads it."""
    if not dataset_id:
        raise SodaError("ID is required")
    try:
        path = _normalize_path(access_url_to_local_path(dataset_id))
    except (SkavaResolveError, PermissionError) as exc:
        raise SodaError(f"ID cannot be served by this node: {exc}") from exc
    if not path.is_file():
        raise SodaError(f"ID not found on this node: {dataset_id}", status=404)
    if not any_file and not _is_supported_dataset(path):
        raise SodaError(f"ID is not a supported dataset: {dataset_id}")
    return path


def _media_type(path: Path) -> str:
    """The media type of a file served as stored."""
    name = path.name.lower()
    if path.suffix.lower() in _FITS_SUFFIXES:
        return "application/fits"
    if name.endswith((".h5", ".hdf5", ".he5")):
        return "application/x-hdf5"
    if name.endswith(".gz"):
        return "application/gzip"
    return "application/octet-stream"


def _whole_file(params: dict[str, list[str]]) -> Path | None:
    """The file an unconstrained cutout asks for, or None when the request
    constrains something (or is not a cutout) and takes the usual route.

    No POS, CIRCLE, POLYGON, BAND or TIME selects the whole dataset, which
    is the file as stored: every HDU and the original header, not a
    rewritten copy. This is what a DataLink "#this" link through this
    service fetches. RESPONSEFORMAT, when given, must be the file's own
    format: the node converts nothing.
    """
    if (_one(params, "OPERATION") or "CUTOUT").upper() != "CUTOUT":
        return None
    if any(_one(params, name) for name in ("POS", "CIRCLE", "POLYGON", "BAND", "TIME")):
        return None
    unknown = set(params) - _ALLOWED["CUTOUT"] - _CONTROL
    if unknown:
        raise SodaError(f"parameters not understood by OPERATION=CUTOUT: {', '.join(sorted(unknown))}")
    path = _resolve(_one(params, "ID"), any_file=True)
    fmt = _one(params, "RESPONSEFORMAT")
    if fmt and fmt.split(";")[0].strip().lower() not in (
            _FITS_FORMATS if _media_type(path) == "application/fits" else {_media_type(path)}):
        raise SodaError(f"RESPONSEFORMAT {fmt!r} is not this file's format ({_media_type(path)}); "
                        "the whole dataset is returned as stored, without conversion")
    return path


def _check_format(params: dict[str, list[str]]) -> None:
    fmt = _one(params, "RESPONSEFORMAT")
    if fmt and fmt.split(";")[0].strip().lower() not in _FITS_FORMATS:
        raise SodaError(f"RESPONSEFORMAT {fmt!r} is not supported; use application/fits")


def _validate(params: dict[str, list[str]]) -> tuple[str, Path, dict[str, str]]:
    """The operation, the dataset path and the operation's parameters, or a UsageError."""
    operation = (_one(params, "OPERATION") or "CUTOUT").upper()
    if operation not in _OPERATIONS:
        raise SodaError(f"OPERATION must be one of {', '.join(_OPERATIONS)}")
    unknown = set(params) - _ALLOWED[operation] - _CONTROL
    if unknown:
        raise SodaError(f"parameters not understood by OPERATION={operation}: {', '.join(sorted(unknown))}")
    _check_format(params)
    path = _resolve(_one(params, "ID"))
    values = {name: v for name in _ALLOWED[operation] if (v := _one(params, name)) is not None}
    # SODA's three spatial parameters: POS, or CIRCLE / POLYGON on their own.
    spatial = [name for name in ("POS", "CIRCLE", "POLYGON") if name in values]
    if len(spatial) > 1:
        raise SodaError(f"give one spatial constraint, not {' and '.join(spatial)}")
    if spatial and spatial[0] != "POS":
        values["POS"] = f"{spatial[0]} {values.pop(spatial[0])}"
    if operation == "MOMENT":
        try:
            order = int(values.get("MOMENT", ""))
        except ValueError as exc:
            raise SodaError("MOMENT=<order> is required (an integer)") from exc
        if not 0 <= order <= 10:
            raise SodaError("MOMENT must be between 0 and 10")
        if "CHANNELS" in values:
            try:
                first, last = (int(p) for p in values["CHANNELS"].replace(",", " ").split())
            except ValueError as exc:
                raise SodaError("CHANNELS must be '<first> <last>' (0-based integers)") from exc
            if first < 0 or first > last:
                raise SodaError("CHANNELS must be '<first> <last>' with 0 <= first <= last")
        if "THRESHOLD" in values:
            try:
                float(values["THRESHOLD"])
            except ValueError as exc:
                raise SodaError("THRESHOLD must be a number") from exc
    if operation == "PV":
        nums = (values.get("PVPATH") or "").replace(",", " ").split()
        try:
            coords = [float(n) for n in nums]
        except ValueError as exc:
            raise SodaError("PVPATH must be numbers: '<ra1> <dec1> <ra2> <dec2> ...'") from exc
        if len(coords) < 4 or len(coords) % 2:
            raise SodaError("PVPATH needs at least two '<ra> <dec>' vertices")
        if "PVWIDTH" in values and not (values["PVWIDTH"].isdigit() and int(values["PVWIDTH"]) > 0):
            raise SodaError("PVWIDTH must be a positive integer (pixels)")
    return operation, path, values


_ALL_CHANNELS = 2 ** 31 - 1


def _job_dir(job_id: str) -> Path:
    """Where a job writes its product. Removing it (abort, delete, expiry)
    reclaims the storage, and makes any late write of an abandoned
    computation fail instead of leaving an orphan file."""
    return get_exports_dir() / "soda" / "".join(c for c in job_id if c.isalnum() or c in "-_")


def _output_path(out_dir: Path, stem: str, suffix: str) -> Path:
    safe = "".join(c if c.isalnum() or c in "-_." else "_" for c in stem)
    return out_dir / f"{safe}_{suffix}_{uuid.uuid4().hex[:8]}.fits"


# Job directories whose removal failed (on Windows, a file still open in a
# worker or a download); retried at every later reclaim.
_UNRECLAIMED: set[str] = set()


_RETRY_SECONDS = 30.0
_retry_handle: asyncio.TimerHandle | None = None
_retry_loop: asyncio.AbstractEventLoop | None = None


def _reclaim(job_id: str | None = None) -> None:
    for pending in ([job_id] if job_id else []) + list(_UNRECLAIMED):
        directory = _job_dir(pending)
        shutil.rmtree(directory, ignore_errors=True)
        if directory.exists():
            _UNRECLAIMED.add(pending)
        else:
            _UNRECLAIMED.discard(pending)
    _schedule_retry()


def _schedule_retry() -> None:
    """While something could not be removed, try again every _RETRY_SECONDS."""
    global _retry_handle, _retry_loop
    if not _UNRECLAIMED:
        return
    try:
        loop = asyncio.get_running_loop()
    except RuntimeError:              # e.g. called from the registry's eviction, off the loop
        return
    if _retry_handle is not None and not _retry_handle.cancelled() and _retry_loop is loop:
        return                        # already scheduled on this loop

    def retry() -> None:
        global _retry_handle
        _retry_handle = None
        _reclaim()

    _retry_handle, _retry_loop = loop.call_later(_RETRY_SECONDS, retry), loop


def _on_evict(record) -> None:
    if (record.result or {}).get("soda"):
        _reclaim(record.task_id)


TASKS.add_eviction_listener(_on_evict)


def _pv_pixels(path: Path, pvpath: str) -> list[list[float]]:
    """PVPATH vertices (degrees) → 0-based pixel (x, y) on the cube's celestial grid."""
    from astropy.io import fits
    from astropy.wcs import WCS

    from ..soda_cutout import _first_image_hdu

    coords = [float(n) for n in pvpath.replace(",", " ").split()]
    with fits.open(path, memmap=True) as hdul:
        header = _first_image_hdu(hdul).header
    from astropy.coordinates import SkyCoord

    cel = WCS(header).celestial
    # PVPATH is ICRS like every SODA position; astropy converts to the cube's
    # own frame (Galactic cubes included).
    xs, ys = cel.world_to_pixel(SkyCoord(coords[0::2], coords[1::2], unit="deg", frame="icrs"))
    pixels = [[float(x), float(y)] for x, y in zip(xs, ys)]
    if not all(v == v for xy in pixels for v in xy):
        raise CutoutNoOverlap("PVPATH lies outside the dataset's projection.")
    return pixels


async def _execute(operation: str, path: Path, values: dict[str, str], out_dir: Path,
                   task_id: str | None = None) -> dict[str, Any]:
    """Run one operation, writing into ``out_dir``; returns {output_path, ...} or raises.
    ``task_id`` names the job: the moment and PV workers poll its stop file
    to end an aborted job (app/compute/abort.py); the cutout is one read and
    one write."""
    if task_id is not None:
        record = TASKS.peek(task_id)
        if record is None or record.status == "cancelled":
            raise asyncio.CancelledError("aborted before the computation started")
    stop_file = TASKS.stop_file(task_id) if task_id else None
    if operation == "CUTOUT":
        result = await asyncio.to_thread(make_cutout, str(path), pos=values.get("POS"),
                                         band=values.get("BAND"), time=values.get("TIME"),
                                         filename_hint=path.stem)
        out = _output_path(out_dir, path.stem, "cutout")
        await asyncio.to_thread(out.write_bytes, result.fits_bytes)
        applied = ",".join(k for k, v in result.applied.items() if v is True)
        return {"output_path": str(out), "applied": applied}
    if operation == "MOMENT":
        from ..compute import worker_moment_save_fits

        order = int(values["MOMENT"])
        # Without CHANNELS: the whole spectral axis (the worker clamps the end).
        first, last = (int(p) for p in values.get("CHANNELS", f"0 {_ALL_CHANNELS}").replace(",", " ").split())
        threshold = values.get("THRESHOLD")
        out = _output_path(out_dir, path.stem, f"m{order}")
        result = await _run_heavy(worker_moment_save_fits, str(path), str(out), order, first, last,
                                  threshold is not None, float(threshold or 0.0), False, False, stop_file,
                                  task_id=task_id)
        return {"output_path": result["output_path"], "applied": "moment"}
    from ..compute import worker_pv_save_fits

    vertices = await asyncio.to_thread(_pv_pixels, path, values["PVPATH"])
    out = _output_path(out_dir, path.stem, "pv")
    result = await _run_heavy(worker_pv_save_fits, str(path), str(out), vertices,
                              max(1, int(values.get("PVWIDTH", "1"))), stop_file, task_id=task_id)
    return {"output_path": result["output_path"], "applied": "pv"}


# ── sync ────────────────────────────────────────────────────────────────────


@router.api_route("/soda/sync", methods=["GET", "POST"])
async def soda_sync(request: Request, token: str = _auth) -> Response:
    """SODA sync: the cutout of one dataset, streamed as FITS."""
    try:
        params = await _params(request)
        if (_one(params, "OPERATION") or "CUTOUT").upper() != "CUTOUT":
            raise SodaError("only cutouts run synchronously; use /soda/async for OPERATION=MOMENT or PV")
        whole = _whole_file(params)
        if whole is not None:
            logger.info("[soda] sync %s unconstrained: the file as stored", whole.name)
            return FileResponse(whole, media_type=_media_type(whole), filename=whole.name,
                                headers={"X-Soda-Applied": ""})
        _, path, values = _validate(params)
        result = await asyncio.to_thread(make_cutout, str(path), pos=values.get("POS"),
                                         band=values.get("BAND"), time=values.get("TIME"),
                                         filename_hint=path.stem)
    except SodaError as exc:
        return exc.response()
    except CutoutNoOverlap:
        # SODA: a valid request that selects nothing is an empty answer, not an error.
        return Response(status_code=204)
    except CutoutError as exc:
        return SodaError(str(exc)).response()
    applied = ",".join(k for k, v in result.applied.items() if v is True)
    logger.info("[soda] sync %s applied=%s bytes=%d", path.name, applied, len(result.fits_bytes))
    return StreamingResponse(io.BytesIO(result.fits_bytes), media_type="application/fits", headers={
        "Content-Disposition": f'attachment; filename="{result.filename}"',
        "X-Soda-Applied": applied,
    })


# ── async (UWS) ─────────────────────────────────────────────────────────────


def _job_url(request: Request, job_id: str) -> str:
    return str(request.url_for("soda_async_job", job_id=job_id))


def _start(task_id: str) -> None:
    task = TASKS.peek(task_id)
    spec = (task.result or {}).get("soda") if task else None
    if task is None or spec is None or task.status != "held" or task.asyncio_handle is not None:
        return
    operation, path, values = spec["operation"], Path(spec["path"]), spec["values"]
    spec["start_time"] = _utc_now()
    TASKS.update(task_id, status="running", progress=0.05)
    # The stop file exists on the record from now on: an ABORT that lands
    # before the worker has started is touched into it, not lost.
    TASKS.stop_file(task_id)

    async def runner() -> None:
        # The computation runs in a worker process the event loop cannot
        # interrupt. ABORT ends the job at once for the client (phase
        # ABORTED, no result) and touches the job's stop file: a moment or PV
        # worker polls it between its slabs and ends within one; a cutout
        # runs to its end. Either way the abandoned computation's output is
        # deleted rather than published, and until the worker really ends the
        # node keeps counting the job as running (the worker is registered on
        # the record), so a placement decision is not told the node is idle
        # while it is not.
        work = None
        try:
            out_dir = _job_dir(task_id)
            out_dir.mkdir(parents=True, exist_ok=True)
            work = asyncio.ensure_future(_execute(operation, path, values, out_dir, task_id))
            TASKS.update(task_id, work_handle=work)
            output = await asyncio.shield(work)
            spec["end_time"] = _utc_now()
            TASKS.update(task_id, status="completed", progress=1.0, result={"soda": spec, **output})
            logger.info("[soda] async %s %s → %s", operation, path.name, output["output_path"])
        except asyncio.CancelledError:
            spec["end_time"] = _utc_now()
            TASKS.update(task_id, status="cancelled", error="Aborted by the client.")
            _reclaim(task_id)
            if work is not None:
                # Once the abandoned computation really ends, try again: the
                # first removal can fail while it still holds a file open.
                work.add_done_callback(lambda fut: _abandoned(fut, task_id))
        except CutoutNoOverlap as exc:
            # SODA: an empty selection completes, with a result that says why.
            spec["end_time"] = _utc_now()
            _reclaim(task_id)
            TASKS.update(task_id, status="completed", progress=1.0,
                         result={"soda": spec, "no_data": f"No data: {exc}"})
        except (SodaError, CutoutError) as exc:
            spec["end_time"] = _utc_now()
            _reclaim(task_id)
            TASKS.update(task_id, status="failed", progress=1.0, error=f"UsageError: {exc}")
        except Exception as exc:  # noqa: BLE001 — reported through the job's error
            spec["end_time"] = _utc_now()
            _reclaim(task_id)
            logger.exception("[soda] async job %s failed", task_id)
            TASKS.update(task_id, status="failed", progress=1.0, error=f"FatalError: {exc}")

    TASKS.update(task_id, asyncio_handle=asyncio.create_task(runner()))


def _abandoned(work: "asyncio.Future[Any]", job_id: str) -> None:
    """An aborted job's computation ended: take its outcome (an aborted
    worker's ``ComputationAborted`` is logged once, not reported as an
    exception nobody retrieved) and reclaim the job's directory again."""
    if not work.cancelled():
        exc = work.exception()
        if exc is not None:
            logger.info("[soda] abandoned job %s ended: %s", job_id, exc)
    _reclaim(job_id)


def _utc(stamp: datetime) -> str:
    """UWS timestamps: UTC, ISO 8601 with milliseconds, and a trailing Z.
    Whole seconds would make a job's own duration (endTime minus
    startTime) useless below a few seconds."""
    return stamp.astimezone(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def _utc_now() -> str:
    return _utc(datetime.now(tz=timezone.utc))


def _as_utc(iso: str | None) -> str | None:
    if not iso:
        return None
    try:
        stamp = datetime.fromisoformat(iso.replace("Z", "+00:00"))
    except ValueError:
        return None
    return _utc(stamp if stamp.tzinfo else stamp.replace(tzinfo=timezone.utc))


def _destruction(task) -> str | None:
    """When the registry forgets the job (its retention TTL), as ISO time."""
    stats = TASKS.stats()
    if not stats.get("ttl_enabled"):
        return None
    try:
        updated = datetime.fromisoformat(task.updated_at)
    except ValueError:
        return None
    if updated.tzinfo is None:
        updated = updated.replace(tzinfo=timezone.utc)
    return _utc(updated + timedelta(seconds=float(stats["ttl_seconds"])))


def _job_view(request: Request, task) -> dict[str, Any]:
    spec = (task.result or {}).get("soda") or {}
    view = {
        "job_id": task.task_id,
        "phase": _PHASE.get(task.status, "UNKNOWN"),
        "operation": spec.get("operation"),
        "parameters": {"ID": spec.get("id"), **spec.get("values", {})},
        "run_id": spec.get("runid"),
        "creation_time": _as_utc(task.created_at),
        "start_time": spec.get("start_time"),
        "end_time": spec.get("end_time"),
        "destruction": _destruction(task),
        "updated_time": _as_utc(task.updated_at),
        "progress": task.progress,
        "results": [],
        "error": task.error or None,
    }
    if task.status == "completed" and task.result and task.result.get("output_path"):
        view["results"] = [{"id": "result", "href": _job_url(request, task.task_id) + "/results/result"}]
        view["output_path"] = task.result["output_path"]
        view["applied"] = task.result.get("applied", "")
    elif task.status == "completed" and task.result and task.result.get("no_data"):
        # The empty selection's result: a short text saying so (SODA async).
        view["results"] = [{"id": "result", "href": _job_url(request, task.task_id) + "/results/result"}]
        view["no_data"] = task.result["no_data"]
    return view


def _nil(tag: str, value: str | None) -> str:
    return (f"<uws:{tag}>{escape(value)}</uws:{tag}>" if value
            else f'<uws:{tag} xsi:nil="true"/>')


def _job_xml(view: dict[str, Any]) -> str:
    params = "".join(f'<uws:parameter id={quoteattr(k)}>{escape(str(v))}</uws:parameter>'
                     for k, v in view["parameters"].items() if v is not None)
    results = "".join(f'<uws:result id="{r["id"]}" xlink:href={quoteattr(r["href"])}/>' for r in view["results"])
    error = (f'<uws:errorSummary type="fatal" hasDetail="true"><uws:message>{escape(view["error"])}</uws:message></uws:errorSummary>'
             if view["error"] and view["phase"] == "ERROR" else "")
    run_id = f'<uws:runId>{escape(view["run_id"])}</uws:runId>' if view["run_id"] else ""
    # Element order and the nillable mandatory ones as the UWS 1.1 schema has them.
    return (f'<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<uws:job xmlns:uws="{_UWS}" xmlns:xlink="{_XLINK}" xmlns:xsi="{_XSI}" version="1.1">'
            f'<uws:jobId>{escape(view["job_id"])}</uws:jobId>{run_id}'
            f'<uws:ownerId xsi:nil="true"/>'
            f'<uws:phase>{view["phase"]}</uws:phase>'
            f'<uws:creationTime>{escape(view["creation_time"])}</uws:creationTime>'
            f'{_nil("startTime", view["start_time"])}{_nil("endTime", view["end_time"])}'
            f'<uws:executionDuration>0</uws:executionDuration>'
            f'{_nil("destruction", view["destruction"])}'
            f'<uws:parameters>{params}</uws:parameters>'
            f'<uws:results>{results}</uws:results>{error}</uws:job>')


def _wants_json(request: Request) -> bool:
    return "application/json" in request.headers.get("accept", "")


def _job_response(request: Request, task) -> Response:
    view = _job_view(request, task)
    if _wants_json(request):
        return Response(content=json.dumps(view), media_type="application/json")
    return Response(content=_job_xml(view), media_type="text/xml")


def _job_or_404(job_id: str, token: str):
    task = _owned_task(job_id, token)
    if task is None or not (task.result or {}).get("soda"):
        raise SodaError(f"no such job: {job_id}", status=404)
    return task


@router.get("/soda/async")
async def soda_async_list(request: Request, token: str = _auth) -> Response:
    """The caller's SODA jobs (UWS job list)."""
    jobs = [t for t in TASKS.list_owned(token) if (t.result or {}).get("soda")]
    if _wants_json(request):
        return Response(content=json.dumps(
            [{"job_id": t.task_id, "phase": _PHASE.get(t.status, "UNKNOWN"),
              "href": _job_url(request, t.task_id)} for t in jobs]), media_type="application/json")
    refs = "".join(f'<uws:jobref id={quoteattr(t.task_id)} xlink:href={quoteattr(_job_url(request, t.task_id))}>'
                   f'<uws:phase>{_PHASE.get(t.status, "UNKNOWN")}</uws:phase></uws:jobref>' for t in jobs)
    return Response(content=f'<?xml version="1.0" encoding="UTF-8"?>\n<uws:jobs xmlns:uws="{_UWS}" '
                            f'xmlns:xlink="{_XLINK}" version="1.1">{refs}</uws:jobs>', media_type="text/xml")


@router.post("/soda/async")
async def soda_async_create(request: Request, token: str = _auth) -> Response:
    """Create a job (PENDING); with PHASE=RUN it starts at once. Answers 303 to the job."""
    try:
        params = await _params(request)
        operation, path, values = _validate(params)
        phase = (_one(params, "PHASE") or "").upper()
        if phase not in ("", "RUN"):
            raise SodaError("PHASE at creation can only be RUN")
    except SodaError as exc:
        return exc.response()
    task = TASKS.create("soda-" + operation.lower(), owner_token=token)
    TASKS.update(task.task_id, status="held",
                 result={"soda": {"operation": operation, "path": str(path), "id": _one(params, "ID"),
                                  "values": values, "runid": _one(params, "RUNID")}})
    if phase == "RUN":
        _start(task.task_id)
    return RedirectResponse(_job_url(request, task.task_id), status_code=303)


@router.get("/soda/async/{job_id}", name="soda_async_job")
async def soda_async_job(job_id: str, request: Request, token: str = _auth) -> Response:
    try:
        return _job_response(request, _job_or_404(job_id, token))
    except SodaError as exc:
        return exc.response()


@router.get("/soda/async/{job_id}/phase")
async def soda_async_phase(job_id: str, token: str = _auth) -> Response:
    try:
        return PlainTextResponse(_PHASE.get(_job_or_404(job_id, token).status, "UNKNOWN"))
    except SodaError as exc:
        return exc.response()


@router.post("/soda/async/{job_id}/phase")
async def soda_async_set_phase(job_id: str, request: Request, token: str = _auth) -> Response:
    """PHASE=RUN starts a pending job; PHASE=ABORT stops a pending or running one."""
    try:
        task = _job_or_404(job_id, token)
        phase = (_one(await _params(request), "PHASE") or "").upper()
        if phase == "RUN":
            if task.status != "held":
                raise SodaError(f"job is {_PHASE.get(task.status)}, not PENDING")
            _start(job_id)
        elif phase == "ABORT":
            if task.status == "held":
                TASKS.update(job_id, status="cancelled", error="Aborted by the client.")
            elif task.status in ("pending", "running"):
                TASKS.cancel(job_id)
        else:
            raise SodaError("PHASE must be RUN or ABORT")
    except SodaError as exc:
        return exc.response()
    return RedirectResponse(_job_url(request, job_id), status_code=303)


@router.get("/soda/async/{job_id}/results")
async def soda_async_results(job_id: str, request: Request, token: str = _auth) -> Response:
    try:
        view = _job_view(request, _job_or_404(job_id, token))
    except SodaError as exc:
        return exc.response()
    results = "".join(f'<uws:result id="{r["id"]}" xlink:href={quoteattr(r["href"])}/>' for r in view["results"])
    return Response(content=f'<?xml version="1.0" encoding="UTF-8"?>\n<uws:results xmlns:uws="{_UWS}" '
                            f'xmlns:xlink="{_XLINK}">{results}</uws:results>', media_type="text/xml")


@router.get("/soda/async/{job_id}/results/result")
async def soda_async_result(job_id: str, token: str = _auth) -> Response:
    """The product itself, as FITS."""
    try:
        task = _job_or_404(job_id, token)
        if task.status == "completed" and (task.result or {}).get("no_data"):
            return PlainTextResponse(task.result["no_data"])
        output = (task.result or {}).get("output_path")
        if task.status != "completed" or not output or not Path(output).is_file():
            raise SodaError(f"job {job_id} has no result (phase {_PHASE.get(task.status)})", status=404)
    except SodaError as exc:
        return exc.response()
    return FileResponse(output, media_type="application/fits", filename=Path(output).name)


@router.get("/soda/async/{job_id}/error")
async def soda_async_error(job_id: str, token: str = _auth) -> Response:
    try:
        task = _job_or_404(job_id, token)
    except SodaError as exc:
        return exc.response()
    return PlainTextResponse(task.error or "")


@router.delete("/soda/async/{job_id}")
async def soda_async_delete(job_id: str, request: Request, token: str = _auth) -> Response:
    """Abort the job if needed and forget it; its product stays in the workspace."""
    try:
        task = _job_or_404(job_id, token)
    except SodaError as exc:
        return exc.response()
    if task.status in ("pending", "running"):
        TASKS.cancel(job_id)
    # Destroying a job reclaims its result storage (UWS).
    _reclaim(job_id)
    TASKS.forget(job_id)
    return RedirectResponse(str(request.url_for("soda_async_list")), status_code=303)



# ── the rest of the UWS job resources ───────────────────────────────────────


@router.get("/soda/async/{job_id}/parameters")
async def soda_async_parameters(job_id: str, request: Request, token: str = _auth) -> Response:
    try:
        view = _job_view(request, _job_or_404(job_id, token))
    except SodaError as exc:
        return exc.response()
    params = "".join(f'<uws:parameter id={quoteattr(k)}>{escape(str(v))}</uws:parameter>'
                     for k, v in view["parameters"].items() if v is not None)
    return Response(content=f'<?xml version="1.0" encoding="UTF-8"?>\n<uws:parameters xmlns:uws="{_UWS}">'
                            f'{params}</uws:parameters>', media_type="text/xml")


@router.get("/soda/async/{job_id}/quote")
async def soda_async_quote(job_id: str, token: str = _auth) -> Response:
    """No estimate of when the job will end is made: empty, as UWS allows."""
    try:
        _job_or_404(job_id, token)
    except SodaError as exc:
        return exc.response()
    return PlainTextResponse("")


@router.get("/soda/async/{job_id}/executionduration")
async def soda_async_duration(job_id: str, token: str = _auth) -> Response:
    """0: no limit is imposed on a job beyond the backend's own task timeout."""
    try:
        _job_or_404(job_id, token)
    except SodaError as exc:
        return exc.response()
    return PlainTextResponse("0")


@router.get("/soda/async/{job_id}/destruction")
async def soda_async_destruction(job_id: str, token: str = _auth) -> Response:
    try:
        return PlainTextResponse(_destruction(_job_or_404(job_id, token)) or "")
    except SodaError as exc:
        return exc.response()


@router.get("/soda/async/{job_id}/owner")
async def soda_async_owner(job_id: str, token: str = _auth) -> Response:
    """Owners are tokens, which are never echoed: the owner is not disclosed."""
    try:
        _job_or_404(job_id, token)
    except SodaError as exc:
        return exc.response()
    return PlainTextResponse("")


@router.post("/soda/async/{job_id}/executionduration")
@router.post("/soda/async/{job_id}/destruction")
async def soda_async_fixed(job_id: str, request: Request, token: str = _auth) -> Response:
    """The service keeps its own limits (UWS lets it); the job is shown as it stands."""
    try:
        _job_or_404(job_id, token)
    except SodaError as exc:
        return exc.response()
    return RedirectResponse(_job_url(request, job_id), status_code=303)


# ── VOSI ────────────────────────────────────────────────────────────────────


@router.get("/soda/capabilities")
async def soda_capabilities(request: Request) -> Response:
    """VOSI capabilities: the sync and async SODA interfaces of this node."""
    base = str(request.base_url).rstrip("/")

    def capability(standard: str, path: str) -> str:
        return (f'<capability standardID="{standard}"><interface xsi:type="vs:ParamHTTP" role="std">'
                f'<accessURL use="full">{escape(base + path)}</accessURL></interface></capability>')

    body = (capability("ivo://ivoa.net/std/SODA#sync-1.0", "/soda/sync")
            + capability("ivo://ivoa.net/std/SODA#async-1.0", "/soda/async")
            + capability("ivo://ivoa.net/std/VOSI#capabilities", "/soda/capabilities")
            + capability("ivo://ivoa.net/std/VOSI#availability", "/soda/availability"))
    return Response(content=(f'<?xml version="1.0" encoding="UTF-8"?>\n'
                             f'<vosi:capabilities xmlns:vosi="http://www.ivoa.net/xml/VOSICapabilities/v1.0" '
                             f'xmlns:xsi="{_XSI}" xmlns:vs="http://www.ivoa.net/xml/VODataService/v1.1">'
                             f'{body}</vosi:capabilities>'), media_type="text/xml")


@router.get("/soda/availability")
async def soda_availability() -> Response:
    """VOSI availability: the service answers, so it is available."""
    return Response(content=('<?xml version="1.0" encoding="UTF-8"?>\n'
                             '<vosi:availability xmlns:vosi="http://www.ivoa.net/xml/VOSIAvailability/v1.0">'
                             '<vosi:available>true</vosi:available></vosi:availability>'),
                    media_type="text/xml")
