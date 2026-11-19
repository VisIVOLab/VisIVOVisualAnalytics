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

# A Pandas frame stores Python/NumPy column objects in addition to the numeric
# payload.  Beyond this many values, the compatibility path used for a global
# sort is too likely to exhaust a desktop backend's memory.  Sampling and
# numeric filtering remain out-of-core and have no such limit.
MAX_MATERIALISED_PARTICLE_CELLS = 50_000_000

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
    if fmt == "gadget" and len(read_as) > 1:
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
    entry_path = read_as[0] if fmt == "gadget" else str(path)
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
                            random_sample: bool, even_sample: bool, phase: int):
    """Filter a component out of core, then read only the requested page."""
    from .. import gadget_snapshot as gs  # noqa: PLC0415
    from ..point_table import particle_rows  # noqa: PLC0415
    component = gs.resolve(path)
    numeric_ops = {"=", "!=", ">", ">=", "<", "<="}
    prepared = []
    for item in filters:
        if item.field not in component.names:
            raise ValueError(f"Unknown field: {item.field!r}")
        if item.op not in numeric_ops:
            raise ValueError(f"Particle filtering supports numeric operators only, not {item.op!r}")
        prepared.append((item.field, item.op, float(item.value)))
    chunk = 1_000_000

    def matching(start, stop):
        source_rows = np.arange(start, stop, dtype=np.int64)
        keep = np.ones(stop - start, dtype=bool)
        for field, op, value in prepared:
            column = component.take(field, source_rows)
            keep &= {"=": column == value, "!=": column != value,
                     ">": column > value, ">=": column >= value,
                     "<": column < value, "<=": column <= value}[op]
        return np.flatnonzero(keep).astype(np.int64) + start

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
        return particle_rows(path, ranks), total, ranks
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
    return particle_rows(path, rows), total, rows

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
        if is_particle_format(entry.get("format")):
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
        if particle and request.sort is None:
            if request.filters:
                df, total, indices = await asyncio.to_thread(
                    _particle_filtered_page, entry["path"], request.filters,
                    request.limit, request.offset, request.random_sample,
                    request.even_sample, request.phase)
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
                df = await asyncio.to_thread(particle_rows, entry["path"], indices)
            row_index = [int(index) for index in indices]
        else:
            table = entry.get("dataframe")
            if table is None and particle:
                cells = int(entry.get("num_rows", 0)) * max(
                    1, len(entry.get("fields") or ()))
                if cells > MAX_MATERIALISED_PARTICLE_CELLS:
                    raise ValueError(
                        "Global sorting is disabled for this very large particle table; "
                        "use out-of-core filters or sampling instead. "
                        "Out-of-core sorting is not available yet.")
                from ..point_table import particle_frame  # noqa: PLC0415
                table = await asyncio.to_thread(particle_frame, entry["path"])
                entry["dataframe"] = table
            df, total = _apply_tabular_query(
                table, request.filters, request.sort, request.limit, request.offset,
                even_sample=request.even_sample, phase=request.phase,
                random_sample=request.random_sample)
            row_index = _original_row_positions(table, df)
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
