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

@router.post("/v1/catalogue/open", response_model=CatalogueOpenResponse, tags=["catalogue"])
async def open_catalogue(
    request: CatalogueOpenRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> CatalogueOpenResponse:
    raw_paths = request.paths if request.paths else [request.path]
    paths = [_normalize_path(p) for p in raw_paths]
    for p in paths:
        if not p.exists() or not p.is_file():
            return CatalogueOpenResponse(valid=False, error=f"Catalogue file not found: {p}")
    is_vbt = request.format.strip().lower() == "vbt"
    if request.byte_order is not None and not is_vbt:
        return CatalogueOpenResponse(
            valid=False, error="byte_order applies to a VisIVO Binary Table only.")
    orders: dict[str, Any] = {}
    read_as = [str(p) for p in paths]

    def load() -> tuple[pd.DataFrame, dict[str, Any]]:
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
        if len(paths) == 1:
            return _catalogue_metadata(Path(read_as[0]), request.format)
        # Multi-file: load each (same format) and concatenate into one
        # catalogue. Require identical columns so a silent NaN-fill can't
        # corrupt "same catalogue split into files".
        frames = [_catalogue_load_dataframe(Path(p), request.format) for p in read_as]
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
        return frame, _catalogue_metadata_from_dataframe(frame, request.format.strip().lower())

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
    session.datasets[dataset_id] = {"path": str(path), "paths": [str(p) for p in paths],
                                    "dataframe": dataframe, **metadata}
    if is_vbt:
        # Provenance: the order the values were read in, and the header's.
        session.datasets[dataset_id]["byte_order"] = orders.get("byte_order", "")
        session.datasets[dataset_id]["byte_order_declared"] = orders.get("byte_order_declared", "")
    logger.info(
        "[catalogue-open] session=%s dataset_id=%s rows=%d cols=%d path=%s",
        session.session_id[:8] + "…",
        dataset_id,
        len(dataframe),
        len(dataframe.columns),
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
    )


def _server_side_rendering() -> bool:
    """As for a cube (routers/datasets.py): the probe imports VTK, so lazily."""
    try:
        from ..render_session import server_side_rendering_available
        return bool(server_side_rendering_available())
    except Exception:   # noqa: BLE001 - no VTK: no server-side rendering
        return False

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
        df: pd.DataFrame = entry["dataframe"]
        max_rows = max(1, int(request.max_rows))
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
        df, total = _apply_tabular_query(
            entry["dataframe"], request.filters, request.sort, request.limit, request.offset,
            even_sample=request.even_sample, phase=request.phase,
            random_sample=request.random_sample,
        )
        rows = df.replace({np.nan: None}).to_dict(orient="records")
        step = sample_step(total, request.limit) if (request.even_sample or request.random_sample) else 1
        row_index = _original_row_positions(entry["dataframe"], df)
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
