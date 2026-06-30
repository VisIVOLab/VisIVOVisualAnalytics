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
    try:
        if len(paths) == 1:
            dataframe, metadata = _catalogue_metadata(paths[0], request.format)
        else:
            # Multi-file: load each (same format) and concatenate into one
            # catalogue. Require identical columns so a silent NaN-fill can't
            # corrupt "same catalogue split into files".
            frames = [_catalogue_load_dataframe(p, request.format) for p in paths]
            base_cols = list(frames[0].columns)
            for src, frame in zip(paths[1:], frames[1:]):
                if list(frame.columns) != base_cols:
                    raise ValueError(
                        f"Column mismatch in {src.name}: expected {base_cols}, "
                        f"got {list(frame.columns)}"
                    )
            dataframe = pd.concat(frames, ignore_index=True)
            if dataframe.empty and len(dataframe.columns) == 0:
                raise ValueError("Catalogue is empty.")
            metadata = _catalogue_metadata_from_dataframe(dataframe, request.format.strip().lower())
    except Exception as exc:
        return CatalogueOpenResponse(valid=False, error=str(exc))

    path = paths[0]
    dataset_id = f"cat_{uuid.uuid4().hex[:12]}"
    session.datasets[dataset_id] = {"path": str(path), "dataframe": dataframe, **metadata}
    logger.info(
        "[catalogue-open] session=%s dataset_id=%s rows=%d cols=%d path=%s",
        session.session_id,
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
    )

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
        df, total = _apply_tabular_query(
            entry["dataframe"], request.filters, request.sort, request.limit, request.offset
        )
        rows = df.replace({np.nan: None}).to_dict(orient="records")
    except HTTPException:
        raise
    except Exception as exc:
        return TabularQueryResponse(valid=False, error=str(exc))
    return TabularQueryResponse(
        valid=True,
        error="",
        total_rows=total,
        returned_rows=len(rows),
        rows=rows,
    )
