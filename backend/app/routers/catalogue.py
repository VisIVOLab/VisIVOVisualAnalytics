from fastapi import APIRouter, Depends, HTTPException, Query
from pydantic import BaseModel
from typing import Any
import uuid
import logging
import os
from pathlib import Path
from datetime import datetime, timezone
import base64
import numpy as np
import pandas as pd
from astropy.io import fits
from ..schemas import *
from ..dependencies import *
from ..auth import verify_token
from ..sessions import Session, get_session, REGISTRY
from ..tasks import TASKS
from ..product_cache import PRODUCT_CACHE
from ..fits_dataset import ScientificFitsDataset
import asyncio

router = APIRouter(tags=['catalogue'])

logger = logging.getLogger('visivo.backend')
_auth = Depends(verify_token)

# An out-of-core sort retains only the prefix needed to answer a page. Bound
# that prefix so a huge offset cannot turn it into a full-table materialisation.
MAX_PARTICLE_SORT_WINDOW = 1_000_000
PARTICLE_CHUNK_ROWS = 1_000_000

@router.post("/v1/catalogue/open", response_model=CatalogueOpenResponse, tags=["catalogue"])
async def open_catalogue(
    request: CatalogueOpenRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> CatalogueOpenResponse:
    from ..point_table import (is_particle_format, particle_catalogue_metadata,  # noqa: PLC0415
                               particle_components, particle_dataset_path, split_component)
    raw_paths = request.paths if request.paths else [request.path]
    fmt = request.format.strip().lower()
    paths: list[Path] = []
    read_as: list[str] = []
    for raw in raw_paths:
        # A particle snapshot (issue #31) opens by a reference to one of its
        # particle types, or by a file (or folder) of it, whatever format the
        # client guessed from the name: the file says what it is. The data
        # roots check the file; the reference names what is read in it.
        base, suffix = split_component(raw)
        p = _normalize_path(base)
        try:
            snapshot = (particle_dataset_path(str(p) + suffix, _within_data_roots)
                        if fmt != "vbt" and p.exists() else None)
        except (OSError, ValueError) as exc:
            return CatalogueOpenResponse(valid=False, error=str(exc))
        if snapshot is not None:
            read_as.append(snapshot[0])
            fmt = snapshot[1]
        elif suffix:
            return CatalogueOpenResponse(valid=False, error=f"Snapshot file not found: {p}")
        elif not p.exists() or not p.is_file():
            return CatalogueOpenResponse(valid=False, error=f"Catalogue file not found: {p}")
        else:
            read_as.append(str(p))
        paths.append(p)
    if fmt in ("gadget", "rawpoints") and len(read_as) > 1:
        return CatalogueOpenResponse(
            valid=False, error="A particle snapshot opens one particle type at a time.")
    is_vbt = fmt == "vbt"
    if request.byte_order is not None and not is_vbt:
        return CatalogueOpenResponse(
            valid=False, error="byte_order applies to a VisIVO Binary Table only.")
    orders: dict[str, Any] = {}

    def load() -> tuple[pd.DataFrame | None, dict[str, Any]]:
        if is_vbt:
            # Before any value is read: the order asked for, else a check of
            # the order the header declares (issue #165).
            from ..vbt_byte_order import open_with_byte_order  # noqa: PLC0415
            from ..volume_source import with_order  # noqa: PLC0415
            for i, p in enumerate(paths):
                found = open_with_byte_order(p, request.byte_order,
                                             by=f"session {session.session_id[:8]}")
                # Read in that order, not the store's of a moment later.
                read_as[i] = with_order(p, found["byte_order"])
                changed = orders.get("changed", False) or found["changed"]
                if not orders or (found["byte_order_warning"]
                                  and not orders["byte_order_warning"]):
                    orders.update(found)
                orders["changed"] = changed
        if fmt == "gadget":
            return None, particle_catalogue_metadata(read_as[0])
        if len(paths) == 1:
            return _catalogue_metadata(Path(read_as[0]), fmt)
        # Multi-file: load each (same format) and concatenate into one
        # catalogue. Require identical columns so a silent NaN-fill can't
        # corrupt "same catalogue split into files".
        frames = [_catalogue_load_dataframe(Path(p), fmt) for p in read_as]
        base_cols = list(frames[0].columns)
        for src, frame in zip(paths[1:], frames[1:]):
            if list(frame.columns) != base_cols:
                raise ValueError(
                    f"Column mismatch in {src.name}: expected {base_cols}, "
                    f"got {list(frame.columns)}"
                )
        frame = pd.concat(frames, ignore_index=True)
        if frame.empty and len(frame.columns) == 0:
            raise ValueError("Catalogue is empty.")
        if is_particle_format(fmt):
            return frame, _vbt_points_metadata(frame, fmt)
        return frame, _catalogue_metadata_from_dataframe(frame, fmt)

    try:
        # Off the event loop: a multi-million-row table takes seconds to parse,
        # and parsing it inline froze every other route, /v1/health included.
        dataframe, metadata = await asyncio.to_thread(load)
    except Exception as exc:
        return CatalogueOpenResponse(valid=False, error=str(exc))
    if is_vbt and orders.get("changed"):
        from ..vbt_byte_order import close_render_sessions_read_otherwise  # noqa: PLC0415
        for p in paths:
            await close_render_sessions_read_otherwise(p, orders["byte_order"])

    path = paths[0]
    dataset_id = f"cat_{uuid.uuid4().hex[:12]}"
    # A snapshot's particle type is known by its reference: every reader of
    # the dataset (render sessions, the VR page, its series) opens that.
    entry_path = read_as[0] if fmt in ("gadget", "rawpoints") else str(path)
    components = particle_components(entry_path)
    session.datasets[dataset_id] = {"path": entry_path, "paths": [str(p) for p in paths],
                                    "dataframe": dataframe, **metadata,
                                    **({"components": components} if components else {})}
    if is_vbt:
        # Provenance: the order the values were read in, and the header's.
        session.datasets[dataset_id]["byte_order"] = orders.get("byte_order", "")
        session.datasets[dataset_id]["byte_order_declared"] = orders.get("byte_order_declared", "")
    logger.info(
        "[catalogue-open] session=%s dataset_id=%s rows=%d cols=%d path=%s",
        session.session_id[:8] + "…",
        dataset_id,
        int(metadata["num_rows"]),
        len(metadata["fields"]),
        path,
    )
    return CatalogueOpenResponse(
        valid=True,
        error="",
        dataset_id=dataset_id,
        session_id=session.session_id,
        num_rows=int(metadata["num_rows"]),
        fields=[CatalogueFieldInfo(**field) for field in metadata["fields"]],
        coordinate_fields=CatalogueCoordinateFields(**metadata["coordinate_fields"]),
        # Drawn on the backend: a VBT, a sky (RA/Dec) or a cartesian catalogue.
        server_side_rendering_available=(
            metadata.get("coordinate_fields", {}).get("system") in ("sky", "cartesian")
            and _server_side_rendering()),
        byte_order=orders.get("byte_order", ""),
        byte_order_declared=orders.get("byte_order_declared", ""),
        byte_order_warning=orders.get("byte_order_warning"),
        format=fmt,
        particles=is_particle_format(fmt),
        path=entry_path,
        components=[ParticleComponentInfo(**c) for c in components],
        snapshot=metadata.get("snapshot") or {},
        component=metadata.get("component") or {},
    )


def _server_side_rendering() -> bool:
    """As for a cube (routers/datasets.py): the probe imports VTK, so lazily."""
    try:
        from ..render_session import server_side_rendering_available
        return bool(server_side_rendering_available())
    except Exception:   # noqa: BLE001 - no VTK: no server-side rendering
        return False


def _particle_filtered_page(path: str, filters, limit: int, offset: int,
                            random_sample: bool, even_sample: bool, phase: int,
                            fields=None):
    """Filter a component out of core, then read only the requested page."""
    from .. import gadget_snapshot as gs  # noqa: PLC0415
    from ..point_table import particle_rows  # noqa: PLC0415
    component = gs.resolve(path)
    matching = _particle_matcher(component, filters)
    chunk = PARTICLE_CHUNK_ROWS
    counts = [len(matching(start, min(component.num_rows, start + chunk)))
              for start in range(0, component.num_rows, chunk)]
    total = int(sum(counts))
    if random_sample and sample_step(total, limit) > 1:
        ranks = _random_sample_rows(total, limit, phase)
    else:
        stride = sample_step(total, limit) if even_sample else 1
        if stride > 1:
            ranks = np.arange(phase, total, stride, dtype=np.int64)[:limit]
        else:
            stop = total if limit <= 0 else min(total, offset + limit)
            ranks = np.arange(offset, stop, dtype=np.int64)
    if not ranks.size:
        return particle_rows(path, ranks, fields), total, ranks
    selected, consumed = [], 0
    for chunk_index, count in enumerate(counts):
        local_ranks = ranks[(ranks >= consumed) & (ranks < consumed + count)] - consumed
        if local_ranks.size:
            start = chunk_index * chunk
            matches = matching(start, min(component.num_rows, start + chunk))
            selected.append(matches[local_ranks])
        consumed += count
        if consumed > ranks[-1]:
            break
    rows = np.concatenate(selected) if selected else np.empty(0, dtype=np.int64)
    return particle_rows(path, rows, fields), total, rows


def _particle_matcher(component, filters):
    """Return a bounded-memory scalar/spatial evaluator for one component.
    Boxes and spheres wrap round a periodic box (app/periodic.py): the
    snapshot's own on its coordinates, or the period a selection states."""
    from ..periodic import box_inside, selection_period, separation  # noqa: PLC0415
    numeric_ops = {"=", "!=", ">", ">=", "<", "<="}
    periods = component.periods() if hasattr(component, "periods") else {}
    prepared = []
    for item in filters:
        if item.box is not None:
            box = item.box
            if len(box.fields) != 3 or len(box.min) != 3 or len(box.max) != 3:
                raise ValueError("A box selection needs three fields, minima and maxima.")
            if item.op not in ("inside", "outside"):
                raise ValueError("A spatial selection operator is 'inside' or 'outside'.")
            if any(field not in component.names for field in box.fields):
                missing = next(field for field in box.fields if field not in component.names)
                raise ValueError(f"Unknown field: {missing!r}")
            if any(not np.isfinite(a) or not np.isfinite(b) or a >= b
                   for a, b in zip(box.min, box.max)):
                raise ValueError("Every box minimum must be finite and below its maximum.")
            prepared.append(("box", tuple(box.fields), tuple(box.min), tuple(box.max), item.op,
                             tuple(selection_period(box.period, f, periods) for f in box.fields)))
            continue
        if item.sphere is not None:
            sphere = item.sphere
            if len(sphere.fields) != 3 or len(sphere.center) != 3:
                raise ValueError("A sphere selection needs three fields and centre coordinates.")
            if item.op not in ("inside", "outside"):
                raise ValueError("A spatial selection operator is 'inside' or 'outside'.")
            if any(field not in component.names for field in sphere.fields):
                missing = next(field for field in sphere.fields if field not in component.names)
                raise ValueError(f"Unknown field: {missing!r}")
            if (any(not np.isfinite(v) for v in sphere.center)
                    or not np.isfinite(sphere.radius) or sphere.radius <= 0):
                raise ValueError("A sphere needs a finite centre and positive radius.")
            prepared.append(("sphere", tuple(sphere.fields), tuple(sphere.center),
                             float(sphere.radius), item.op,
                             tuple(selection_period(sphere.period, f, periods)
                                   for f in sphere.fields)))
            continue
        if item.field not in component.names:
            raise ValueError(f"Unknown field: {item.field!r}")
        if item.op == "in":
            # As the in-memory path (_apply_tabular_query): numbers when every
            # value is one, else the values' text against the column's.
            raw = [v.strip() for v in str(item.value).split(",")]
            try:
                prepared.append(("in", item.field, np.asarray([float(v) for v in raw])))
            except ValueError:
                prepared.append(("text-in", item.field, raw))
            continue
        if item.op == "contains":
            prepared.append(("contains", item.field, str(item.value)))
            continue
        if item.op not in numeric_ops:
            raise ValueError(f"Unknown filter operator: {item.op!r}")
        try:
            value = float(item.value)
        except ValueError:
            raise ValueError(
                f"Cannot convert {item.value!r} to number for field {item.field!r}") from None
        prepared.append(("scalar", item.field, item.op, value))

    def matching(start, stop):
        source_rows = np.arange(start, stop, dtype=np.int64)
        keep = np.ones(stop - start, dtype=bool)
        for criterion in prepared:
            if criterion[0] == "scalar":
                _, field, op, value = criterion
                column = component.take(field, source_rows)
                keep &= {"=": column == value, "!=": column != value,
                         ">": column > value, ">=": column >= value,
                         "<": column < value, "<=": column <= value}[op]
            elif criterion[0] == "in":
                _, field, values = criterion
                keep &= np.isin(component.take(field, source_rows), values)
            elif criterion[0] in ("text-in", "contains"):
                # Text matching, a chunk at a time, exactly as pandas does it
                # on a loaded catalogue (contains: case-insensitive regex).
                _, field, value = criterion
                text = pd.Series(component.take(field, source_rows)).astype(str)
                keep &= (text.isin(value) if criterion[0] == "text-in" else
                         text.str.contains(value, case=False, na=False)).to_numpy(dtype=bool)
            elif criterion[0] == "box":
                _, fields, lo, hi, op, period = criterion
                inside = np.ones(stop - start, dtype=bool)
                for field, a, b, length in zip(fields, lo, hi, period):
                    inside &= box_inside(component.take(field, source_rows), a, b, length)
                keep &= inside if op == "inside" else ~inside
            else:
                _, fields, center, radius, op, period = criterion
                distance2 = np.zeros(stop - start, dtype=np.float64)
                for field, origin, length in zip(fields, center, period):
                    delta = separation(component.take(field, source_rows), origin, length)
                    distance2 += delta * delta
                inside = distance2 <= radius * radius
                keep &= inside if op == "inside" else ~inside
        return np.flatnonzero(keep).astype(np.int64) + start
    return matching


def _diagnostic_selection(filters) -> list[str]:
    """Compact, reproducible descriptions echoed with a diagnostic result."""
    descriptions = []
    for item in filters:
        if item.box is not None:
            descriptions.append(
                f"{item.name or 'box'}: {item.op} "
                f"{dict(zip(item.box.fields, zip(item.box.min, item.box.max)))}")
        elif item.sphere is not None:
            descriptions.append(
                f"{item.name or 'sphere'}: {item.op} centre={item.sphere.center}, "
                f"radius={item.sphere.radius}, fields={item.sphere.fields}")
        else:
            descriptions.append(f"{item.field} {item.op} {item.value}")
    return descriptions or ["complete dataset"]


class DiagnosticsCancelled(Exception):
    """A diagnostics run stopped between two chunks (its task was cancelled)."""


#: Rows a DataFrame diagnostics pass filters at a time (bounded memory).
DIAGNOSTIC_CHUNK_ROWS = 1_000_000


def _diagnostic_accumulate(chunks, field_range, bins: int, weighted: bool,
                           progress=None, stop=None):
    """Exact bounded-memory moments and histogram over the chunks ``chunks()``
    yields: ``(values, weights)``, or ``(values, weights, done)`` with
    ``done`` the fraction of the pass read so far, reported to ``progress``
    (over both passes when the range is automatic). ``stop`` (an Event) set
    between two chunks raises DiagnosticsCancelled.

    The variance combines each chunk's count, mean and squared deviations
    (Chan et al.'s pairwise update), not sum(x^2)/n - mean^2, which loses
    every digit when the spread is small beside the mean (positions far from
    the origin, masses around 1e10).

    The unweighted statistics (finite_rows, minimum, maximum, mean, stddev)
    cover every finite value and do not depend on the weight field; a row
    whose weight is not finite or is negative counts with weight zero in the
    weighted mean, weight sum and histogram."""
    passes = 1 if field_range is not None else 2

    def chunks_of(index):
        for item in chunks():
            if stop is not None and stop.is_set():
                raise DiagnosticsCancelled("Cancelled by client.")
            yield item[0], item[1]
            # After the chunk has been counted: the progress is work done.
            if progress is not None and len(item) > 2:
                progress((index + float(item[2])) / passes)

    def valid(values, weights):
        values = np.asarray(values, dtype=np.float64)
        keep = np.isfinite(values)
        if weighted:
            # A row with an unusable weight stays in the unweighted numbers
            # (count, range, mean, stddev) and weighs nothing in the weighted
            # ones, so choosing a weight field never changes the former.
            weights = np.asarray(weights, dtype=np.float64)
            weights = np.where(np.isfinite(weights) & (weights >= 0.0), weights, 0.0)
            return values[keep], weights[keep], values.size
        return values[keep], None, values.size

    selected = finite = 0
    mean = m2 = weight_sum = weighted_sum = 0.0
    minimum, maximum = np.inf, -np.inf
    histogram = np.zeros(bins, dtype=np.float64)
    edges = None
    if field_range is not None:
        lo, hi = field_range
        edges = np.linspace(lo, hi, bins + 1, dtype=np.float64)
    for raw_values, raw_weights in chunks_of(0):
        values, weights, size = valid(raw_values, raw_weights)
        selected += size
        if not values.size:
            continue
        n_b = values.size
        mean_b = float(values.mean())
        m2_b = float(np.dot(values - mean_b, values - mean_b))
        total = finite + n_b
        delta = mean_b - mean
        mean += delta * n_b / total
        m2 += m2_b + delta * delta * finite * n_b / total
        finite = total
        minimum = min(minimum, float(values.min()))
        maximum = max(maximum, float(values.max()))
        if weighted:
            weight_sum += float(weights.sum(dtype=np.float64))
            weighted_sum += float(np.dot(values, weights))
        if edges is not None:
            # A range given: one pass, the histogram with the moments.
            histogram += np.histogram(values, bins=edges,
                                      weights=weights if weighted else None)[0]
    if not finite:
        raise ValueError("The selected population has no finite values for this field.")
    if edges is None:
        lo, hi = minimum, maximum
        if not np.isfinite(lo) or not np.isfinite(hi) or lo >= hi:
            pad = max(abs(lo) * 0.5, 0.5)
            lo, hi = lo - pad, hi + pad
        edges = np.linspace(lo, hi, bins + 1, dtype=np.float64)
        # `chunks` is a factory, so an automatic range still stays bounded:
        # the second pass rereads each slice instead of retaining all values.
        for raw_values, raw_weights in chunks_of(1):
            values, weights, _ = valid(raw_values, raw_weights)
            histogram += np.histogram(values, bins=edges,
                                      weights=weights if weighted else None)[0]
    if progress is not None:
        progress(1.0)
    return {"selected_rows": selected, "finite_rows": finite,
            "minimum": minimum, "maximum": maximum, "mean": mean,
            "stddev": float(np.sqrt(max(0.0, m2 / finite))),
            "weighted_mean": weighted_sum / weight_sum if weighted and weight_sum > 0 else None,
            "weight_sum": weight_sum if weighted else None,
            "bin_edges": edges.tolist(), "counts": histogram.tolist()}


def _filter_fields(filters) -> set[str]:
    """Every column a filter list reads."""
    names = set()
    for item in filters:
        spatial = item.box if item.box is not None else item.sphere
        names.update(spatial.fields if spatial is not None else [item.field])
    return names


def _entry_periods(entry) -> dict[str, float]:
    """The periodic coordinates of a catalogue entry: a Gadget snapshot's
    box on its positions (app/periodic.py), {} for any other table. Kept in
    the entry after the first query (the snapshot does not change while it
    is open)."""
    cached = entry.get("_periods")
    if cached is not None:
        return cached
    from ..point_table import component_format  # noqa: PLC0415
    path = entry.get("path")
    periods = {}
    if path and component_format(path) == "gadget":
        from .. import gadget_snapshot as gs  # noqa: PLC0415
        periods = gs.resolve(path).periods()
    entry["_periods"] = periods
    return periods


def _diagnostic_chunks(entry, request):
    """The chunk factory of a diagnostics run (see _diagnostic_accumulate):
    an out-of-core particle type through its matcher, a loaded table a
    million rows at a time (only the columns read, each chunk filtered as a
    whole table would be: the filters are row by row)."""
    weighted = bool(request.weight_field)
    from ..point_table import is_particle_format  # noqa: PLC0415
    if is_particle_format(entry.get("format")) and entry.get("dataframe") is None:
        from .. import gadget_snapshot as gs  # noqa: PLC0415
        component = gs.resolve(entry["path"])
        matching = _particle_matcher(component, request.filters)
        n = max(1, component.num_rows)

        def chunks():
            for start in range(0, component.num_rows, PARTICLE_CHUNK_ROWS):
                stop = min(component.num_rows, start + PARTICLE_CHUNK_ROWS)
                rows = matching(start, stop)
                yield (component.take(request.field, rows) if rows.size else np.empty(0),
                       (component.take(request.weight_field, rows) if rows.size
                        else np.empty(0)) if weighted else None,
                       stop / n)
        return chunks
    frame = entry["dataframe"]
    needed = [name for name in frame.columns
              if name in _filter_fields(request.filters)
              or name in (request.field, request.weight_field)]
    periods = _entry_periods(entry)
    n = max(1, len(frame))

    def chunks():
        for start in range(0, len(frame), DIAGNOSTIC_CHUNK_ROWS):
            stop = min(len(frame), start + DIAGNOSTIC_CHUNK_ROWS)
            part, _ = _apply_tabular_query(frame.iloc[start:stop][needed], request.filters,
                                           None, 0, 0, periods=periods)
            yield (part[request.field].to_numpy(),
                   part[request.weight_field].to_numpy() if weighted else None,
                   stop / n)
    return chunks


def _run_diagnostics(entry, request, progress=None, stop=None) -> CatalogueDiagnosticsResponse:
    """Validate a diagnostics request against its catalogue and compute it
    (on a worker thread: two passes over every selected row)."""
    if entry["kind"] != "catalogue":
        raise HTTPException(422, "This endpoint requires a catalogue dataset.")
    fields = {str(field.get("name", "")) for field in entry.get("fields", ())}
    if request.field not in fields:
        raise ValueError(f"Unknown field: {request.field!r}")
    if request.weight_field and request.weight_field not in fields:
        raise ValueError(f"Unknown weight field: {request.weight_field!r}")
    if ((request.range_min is None) != (request.range_max is None)
            or (request.range_min is not None
                and (not np.isfinite(request.range_min)
                     or not np.isfinite(request.range_max)
                     or request.range_min >= request.range_max))):
        raise ValueError("Histogram range needs two finite increasing values.")
    weighted = bool(request.weight_field)
    result = _diagnostic_accumulate(
        _diagnostic_chunks(entry, request),
        ((request.range_min, request.range_max) if request.range_min is not None else None),
        request.bins, weighted, progress=progress, stop=stop)
    units = (entry.get("component") or {}).get("units") or {}
    return CatalogueDiagnosticsResponse(
        valid=True, error="", field=request.field,
        field_unit=str(units.get(request.field, "unit not declared")),
        weight_field=request.weight_field,
        weight_unit=str(units.get(request.weight_field, "unit not declared")) if weighted else "",
        weighting=(f"weighted by {request.weight_field}" if weighted else "unweighted"),
        selection=_diagnostic_selection(request.filters), **result)


@router.post("/v1/catalogue/diagnostics", response_model=CatalogueDiagnosticsResponse,
             tags=["catalogue"])
async def catalogue_diagnostics(
    request: CatalogueDiagnosticsRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> CatalogueDiagnosticsResponse:
    """Exact histogram and moments over the complete selected population,
    answered when done. For a large table, /v1/tasks/catalogue_diagnostics
    runs the same computation as a task with progress and cancellation."""
    try:
        entry = _require_dataset(session, request.dataset_id)
        return await asyncio.to_thread(_run_diagnostics, entry, request)
    except HTTPException:
        raise
    except Exception as exc:
        return CatalogueDiagnosticsResponse(valid=False, error=str(exc))


@router.post("/v1/tasks/catalogue_diagnostics", response_model=TaskCreateResponse,
             tags=["tasks"])
async def create_catalogue_diagnostics_task(
    request: CatalogueDiagnosticsRequest,
    token: str = _auth,
    session: Session = Depends(get_session),
) -> TaskCreateResponse:
    """/v1/catalogue/diagnostics as a task (issue #448): poll
    ``GET /v1/tasks/{task_id}`` for its progress (0..1 over the passes) and
    its result (the diagnostics response), ``DELETE`` it to stop it between
    two chunks. A request the catalogue refuses (unknown field, bad range)
    still creates the task; its result then says why (``valid: false``)."""
    import threading  # noqa: PLC0415
    entry = _require_dataset(session, request.dataset_id)
    if entry["kind"] != "catalogue":
        raise HTTPException(422, "This endpoint requires a catalogue dataset.")
    # A heavy task like the others: one of the session's compute slots, held
    # until the computation itself ends (not when a cancelled runner does).
    if not await acquire_task_slot(session):
        raise task_slot_busy_error()
    task = TASKS.create("catalogue_diagnostics", owner_token=token)
    TASKS.update(task.task_id, status="running", progress=0.0)
    stop = threading.Event()

    def progress(fraction: float) -> None:
        TASKS.update(task.task_id, progress=min(0.99, max(0.0, float(fraction))))

    async def _runner() -> None:
        work = asyncio.ensure_future(
            asyncio.to_thread(_run_diagnostics, entry, request, progress, stop))

        def _ended(future) -> None:
            session.release_task_slot()
            # A cancelled run ends in DiagnosticsCancelled after its runner
            # has gone: retrieved here, so asyncio does not log it as lost.
            if not future.cancelled():
                future.exception()
        work.add_done_callback(_ended)
        TASKS.update(task.task_id, work_handle=work)
        try:
            result = await asyncio.shield(work)
            TASKS.update(task.task_id, status="completed", progress=1.0,
                         result=result.model_dump())
        except asyncio.CancelledError:
            stop.set()
            TASKS.update(task.task_id, status="cancelled", error="Cancelled by client.")
        except DiagnosticsCancelled:
            TASKS.update(task.task_id, status="cancelled", error="Cancelled by client.")
        except Exception as exc:   # noqa: BLE001 - reported in the result
            TASKS.update(task.task_id, status="completed", progress=1.0,
                         result=CatalogueDiagnosticsResponse(
                             valid=False, error=str(exc)).model_dump())

    handle = asyncio.create_task(_runner())
    TASKS.update(task.task_id, asyncio_handle=handle)
    return TaskCreateResponse(valid=True, error="", task_id=task.task_id, status="running")


def _sorted_order(values: np.ndarray, rows: np.ndarray, descending: bool) -> np.ndarray:
    """The order of ``values`` by value (NaN last either way), ties by row
    ascending: a total order, so every page of a sort is a slice of one
    sequence, whatever the chunking (reversing a stable ascending sort put
    ties in descending row order and let pages repeat or skip rows)."""
    missing = np.isnan(values) if values.dtype.kind == "f" else np.zeros(values.shape, bool)
    key = np.where(missing, 0, values) if missing.any() else values
    if not descending:
        return np.lexsort((rows, key, missing))
    # Ascending by (present first, value, -row), reversed: NaN last, value
    # descending, row ascending. Row numbers are non-negative int64.
    return np.lexsort((-rows, key, ~missing))[::-1]


def _particle_sorted_page(path: str, filters, sort, limit: int, offset: int,
                          fields=None):
    """Sort a page while retaining at most ``offset + limit`` candidate rows."""
    from .. import gadget_snapshot as gs  # noqa: PLC0415
    from ..point_table import particle_rows  # noqa: PLC0415
    component = gs.resolve(path)
    if sort.field not in component.names:
        raise ValueError(f"Unknown sort field: {sort.field!r}")
    if limit <= 0:
        raise ValueError("A positive limit is required for out-of-core particle sorting.")
    if offset < 0:
        raise ValueError("offset must not be negative.")
    window = offset + limit
    if window > MAX_PARTICLE_SORT_WINDOW:
        raise ValueError(
            f"The requested sorted window ({window} rows) exceeds the "
            f"out-of-core limit of {MAX_PARTICLE_SORT_WINDOW} rows.")
    descending = sort.direction.lower() == "desc"
    matching = _particle_matcher(component, filters)
    kept_rows = np.empty(0, dtype=np.int64)
    kept_values = None
    total = 0
    for start in range(0, component.num_rows, PARTICLE_CHUNK_ROWS):
        selected = matching(start, min(component.num_rows, start + PARTICLE_CHUNK_ROWS))
        total += len(selected)
        if not len(selected):
            continue
        values = np.asarray(component.take(sort.field, selected))
        rows = np.concatenate((kept_rows, selected))
        values = values if kept_values is None else np.concatenate((kept_values, values))
        order = _sorted_order(values, rows, descending)[:window]
        kept_rows, kept_values = rows[order], values[order]
    page_rows = kept_rows[offset:window]
    return particle_rows(path, page_rows, fields), int(total), page_rows

@router.post("/v1/catalogue/subset", response_model=CatalogueSubsetResponse, tags=["catalogue"])
async def catalogue_subset(
    request: CatalogueSubsetRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> CatalogueSubsetResponse:
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "catalogue":
            raise HTTPException(422, "This endpoint requires a catalogue dataset.")
        max_rows = max(1, int(request.max_rows))
        from ..point_table import is_particle_format, particle_rows  # noqa: PLC0415
        if is_particle_format(entry.get("format")) and entry.get("dataframe") is None:
            count = min(max_rows, int(entry.get("num_rows", 0)))
            frame = await asyncio.to_thread(
                particle_rows, entry["path"], np.arange(count, dtype=np.int64))
        else:
            df: pd.DataFrame = entry["dataframe"]
            frame = df.iloc[:max_rows]
        rows = frame.replace({np.nan: None}).to_dict(orient="records")
    except HTTPException:
        raise
    except Exception as exc:
        return CatalogueSubsetResponse(valid=False, error=str(exc))
    return CatalogueSubsetResponse(valid=True, error="", rows=rows)

@router.post("/v1/catalogue/query", response_model=TabularQueryResponse, tags=["catalogue"])
async def catalogue_query(
    request: TabularQueryRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> TabularQueryResponse:
    """Query a catalogue with optional filters, sort, and pagination."""
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "catalogue":
            raise HTTPException(422, "This endpoint requires a catalogue dataset.")
        # Use the DataFrame cached at open time – no disk I/O.
        # _apply_tabular_query operates on a copy so the original is never mutated.
        if request.phase < 0:
            raise HTTPException(422, "phase must not be negative.")
        from ..point_table import is_particle_format, particle_rows  # noqa: PLC0415
        particle = is_particle_format(entry.get("format"))
        requested_fields = list(dict.fromkeys(request.fields))
        known_fields = {str(field.get("name", "")) for field in entry.get("fields", ())}
        unknown_fields = [field for field in requested_fields if field not in known_fields]
        if unknown_fields:
            raise ValueError(f"Unknown field: {unknown_fields[0]!r}")
        projection = requested_fields or None
        gadget_out_of_core = particle and entry.get("dataframe") is None
        if gadget_out_of_core and request.sort is not None:
            if request.random_sample or request.even_sample:
                raise ValueError(
                    "Sampling and sorting cannot be combined for an out-of-core particle table.")
            df, total, indices = await asyncio.to_thread(
                _particle_sorted_page, entry["path"], request.filters, request.sort,
                request.limit, request.offset, projection)
            row_index = [int(index) for index in indices]
        elif gadget_out_of_core and request.sort is None:
            if request.filters:
                df, total, indices = await asyncio.to_thread(
                    _particle_filtered_page, entry["path"], request.filters,
                    request.limit, request.offset, request.random_sample,
                    request.even_sample, request.phase, projection)
            else:
                total = int(entry.get("num_rows", 0))
                if request.random_sample and sample_step(total, request.limit) > 1:
                    indices = _random_sample_rows(total, request.limit, request.phase)
                else:
                    stride = sample_step(total, request.limit) if request.even_sample else 1
                    if stride > 1:
                        indices = np.arange(request.phase, total, stride, dtype=np.int64)[:request.limit]
                    else:
                        stop = total if request.limit <= 0 else min(total, request.offset + request.limit)
                        indices = np.arange(request.offset, stop, dtype=np.int64)
                df = await asyncio.to_thread(
                    particle_rows, entry["path"], indices, projection)
            row_index = [int(index) for index in indices]
        else:
            table = entry.get("dataframe")
            df, total = _apply_tabular_query(
                table, request.filters, request.sort, request.limit, request.offset,
                even_sample=request.even_sample, phase=request.phase,
                random_sample=request.random_sample, periods=_entry_periods(entry))
            row_index = _original_row_positions(table, df)
            if projection is not None:
                df = df.loc[:, requested_fields]
        rows = df.replace({np.nan: None}).to_dict(orient="records")
        step = sample_step(total, request.limit) if (request.even_sample or request.random_sample) else 1
    except HTTPException:
        raise
    except Exception as exc:
        return TabularQueryResponse(valid=False, error=str(exc))
    return TabularQueryResponse(
        valid=True,
        error="",
        total_rows=total,
        returned_rows=len(rows),
        step=step,
        rows=rows,
        row_index=row_index,
    )


def _original_row_positions(table: pd.DataFrame, subset: pd.DataFrame) -> list[int]:
    """Where each row of ``subset`` sits in ``table``, 0-based.

    The query keeps the opened frame's index on the rows it returns, so the
    positions come from the index, not from the order of the page (which a
    filter, a sort or an even sample has changed). An index that cannot be
    looked up unambiguously yields no positions rather than wrong ones."""
    if not table.index.is_unique:
        return []
    positions = table.index.get_indexer(subset.index)
    if (positions < 0).any():
        return []
    return [int(p) for p in positions]
