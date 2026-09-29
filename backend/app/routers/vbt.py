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

router = APIRouter(tags=['vbt'])

logger = logging.getLogger('visivo.backend')
_auth = Depends(verify_token)

@router.post("/v1/vbt/open", response_model=VbtOpenResponse, tags=["vbt"])
async def vbt_open(
    request: VbtOpenRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> VbtOpenResponse:
    """Register a VisIVO Binary Table (.head + binary) with the backend.

    Returns field schema and geometry metadata; the client receives a
    dataset_id to use for subsequent /v1/vbt/subset calls.
    """
    head_path = _normalize_path(request.path)
    if not head_path.exists() or not head_path.is_file():
        return VbtOpenResponse(valid=False, error="VBT header file not found.")
    if not head_path.suffix == ".head" and not str(head_path).endswith(".bin.head"):
        pass  # accept any extension — validation happens on parse
    bin_path = Path(str(head_path)[: -len(".head")] if str(head_path).endswith(".head") else str(head_path))
    if not bin_path.exists():
        return VbtOpenResponse(valid=False, error=f"VBT binary file not found: {bin_path.name}")

    try:
        header = _parse_vbt_header(head_path)
    except Exception as exc:
        return VbtOpenResponse(valid=False, error=str(exc))

    # Memory-map the binary file once at open time.  np.memmap opens a
    # read-only virtual mapping – no actual data is loaded until accessed,
    # and the OS page cache handles repeated access efficiently.
    field_count: int = header["field_count"]
    num_rows: int = header["num_rows"]
    np_dtype = np.dtype(header["np_endian_code"])
    expected_scalars = field_count * num_rows
    actual_scalars = bin_path.stat().st_size // np_dtype.itemsize
    if actual_scalars != expected_scalars:
        return VbtOpenResponse(
            valid=False,
            error=(
                f"Binary size mismatch: header says {expected_scalars} scalars "
                f"({field_count} fields × {num_rows} rows), "
                f"file contains {actual_scalars} scalars."
            ),
        )
    try:
        # Shape (field_count, num_rows): field-major layout matches VBT spec.
        memmap = np.memmap(bin_path, dtype=np_dtype, mode="r", shape=(field_count, num_rows))
    except Exception as exc:
        return VbtOpenResponse(valid=False, error=f"Failed to memory-map VBT binary: {exc}")

    dataset_id = f"vbt_{uuid.uuid4().hex[:12]}"
    session.datasets[dataset_id] = {
        "kind": "vbt",
        "head_path": str(head_path),
        "bin_path": str(bin_path),
        "header": header,
        "memmap": memmap,  # shape (field_count, num_rows), read-only
    }
    logger.info(
        "[vbt-open] session=%s dataset_id=%s kind=%s fields=%d rows=%d path=%s (memmap)",
        session.session_id[:8] + "…",
        dataset_id,
        header["kind"],
        header["field_count"],
        header["num_rows"],
        head_path,
    )

    fields = [VbtFieldInfo(name=n, type=header["scalar_type"]) for n in header["field_names"]]
    return VbtOpenResponse(
        valid=True,
        error="",
        dataset_id=dataset_id,
        session_id=session.session_id,
        kind=header["kind"],
        fields=fields,
        num_rows=header["num_rows"],
        scalar_type=header["scalar_type"],
        dimensions=header["dimensions"] or [],
        spacing=header["spacing"] or [],
    )

#: Budget for one point-table reply, encoded. 256 MiB is already far past what
#: a viewer can draw as glyphs; the point is to fail with a sentence instead of
#: with a dead process.
_POINT_SUBSET_LIMIT_BYTES = 256 * 1024 * 1024


@router.post("/v1/vbt/subset", response_model=VbtSubsetResponse, tags=["vbt"])
async def vbt_subset(
    request: VbtSubsetRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> VbtSubsetResponse:
    """Return a subset of VBT data encoded as base64 binary columns.

    For **point** datasets all fields are returned up to *max_rows* rows.
    Each column is a base64-encoded float64-LE byte array.

    For **volume** datasets a single field block is returned as a
    base64-encoded float32-LE byte array of length nx*ny*nz.
    """
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "vbt":
            raise HTTPException(422, "This endpoint requires a VBT dataset.")
    except HTTPException:
        raise

    header = entry["header"]
    field_names: list[str] = header["field_names"]
    num_rows: int = header["num_rows"]
    # Use the pre-opened memmap – no disk I/O beyond what the OS pages in.
    arr: np.memmap = entry["memmap"]  # shape (field_count, num_rows)

    if header["kind"] == "point":
        max_rows = request.max_rows if request.max_rows > 0 else num_rows
        max_rows = min(max_rows, num_rows)

        # A point table is returned as one base64 float64 column per field, so
        # the reply costs 8 bytes per value on the wire plus a third again for
        # the encoding — and the arrays, the encoded strings and the serialised
        # JSON are all alive at once.
        #
        # max_rows = 0 means "everything", and the desktop client sends exactly
        # that. On a 21-million-row, 11-field table (mergerSTAR.bin, a real
        # user file) that is 1.8 GB of arrays and 2.5 GB of base64 before
        # serialisation: the process is killed, and the client reports
        # "connection refused" — which points at the network rather than at the
        # request that caused it. Refusing with the number of rows that DO fit
        # turns an unexplained death into an answer the caller can act on.
        payload_bytes = max_rows * len(field_names) * 8 * 4 // 3
        if payload_bytes > _POINT_SUBSET_LIMIT_BYTES:
            fits = (_POINT_SUBSET_LIMIT_BYTES * 3 // 4) // (len(field_names) * 8)
            return VbtSubsetResponse(
                valid=False,
                error=(
                    f"This subset would be {payload_bytes / (1 << 20):.0f} MiB on the wire "
                    f"({max_rows} rows x {len(field_names)} fields), over the "
                    f"{_POINT_SUBSET_LIMIT_BYTES >> 20} MiB limit. Ask for at most "
                    f"{fits} rows via max_rows."
                ),
            )
        columns: dict[str, str] = {}
        for i, fname in enumerate(field_names):
            # np.array() copies the memmap slice so the wire bytes are a plain ndarray.
            col = np.array(arr[i, :max_rows], dtype="<f8")
            columns[fname] = base64.b64encode(col.tobytes()).decode("ascii")
        return VbtSubsetResponse(
            valid=True,
            error="",
            kind="point",
            num_rows=max_rows,
            fields=field_names,
            columns=columns,
        )
    else:  # volume
        field_name = request.field_name if request.field_name else field_names[0]
        if field_name not in field_names:
            return VbtSubsetResponse(
                valid=False, error=f"Field '{field_name}' not found in VBT dataset."
            )
        field_idx = field_names.index(field_name)
        block = np.array(arr[field_idx], dtype="<f4")  # float32-LE on the wire
        return VbtSubsetResponse(
            valid=True,
            error="",
            kind="volume",
            num_rows=num_rows,
            fields=field_names,
            field_name=field_name,
            block=base64.b64encode(block.tobytes()).decode("ascii"),
            dimensions=header["dimensions"],
            spacing=header["spacing"],
        )

@router.post("/v1/vbt/query", response_model=TabularQueryResponse, tags=["vbt"])
async def vbt_query(
    request: TabularQueryRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> TabularQueryResponse:
    """Query a VBT point dataset with optional filters, sort, and pagination.

    Returns column-oriented float64-LE base64 data, same wire format as
    /v1/vbt/subset but with full filter/sort/pagination support.
    Only ``kind='point'`` datasets are supported.
    """
    try:
        entry = _require_dataset(session, request.dataset_id)
        if entry["kind"] != "vbt":
            raise HTTPException(422, "This endpoint requires a VBT dataset.")
    except HTTPException:
        raise

    header = entry["header"]
    if header["kind"] != "point":
        return TabularQueryResponse(
            valid=False, error="vbt/query only supports point datasets; use vbt/subset for volumes."
        )

    field_names: list[str] = header["field_names"]
    # Use the pre-opened memmap – shape (field_count, num_rows).
    arr: np.memmap = entry["memmap"]

    # Determine which columns are referenced by filters/sort so we only copy
    # those slices from the memmap rather than materialising the entire table.
    filter_fields: set[str] = {f.field for f in request.filters}
    sort_field: str | None = request.sort.field if request.sort else None
    needed: set[str] = filter_fields | ({sort_field} if sort_field else set())
    # If no filters/sort are active we still need all columns for the response,
    # so just build them all lazily.
    if not needed:
        needed = set(field_names)

    try:
        # Build DataFrame only from needed columns; copy out of memmap.
        df = pd.DataFrame(
            {
                name: np.array(arr[i], dtype=float)
                for i, name in enumerate(field_names)
                if name in needed
            }
        )
        df, total = _apply_tabular_query(
            df, request.filters, request.sort, request.limit, request.offset
        )
    except Exception as exc:
        return TabularQueryResponse(valid=False, error=str(exc))

    # After filtering we know the surviving row indices.  For columns that were
    # not loaded into the DataFrame (non-filter, non-sort columns when filters
    # were active), read only the filtered rows from the memmap now.
    row_index = df.index  # integer positions in the original array
    columns: dict[str, str] = {}
    for i, name in enumerate(field_names):
        if name in df.columns:
            col = df[name].to_numpy().astype("<f8")
        else:
            # Read only the rows that survived filtering.
            col = np.array(arr[i][row_index], dtype="<f8")
        columns[name] = base64.b64encode(col.tobytes()).decode("ascii")

    return TabularQueryResponse(
        valid=True,
        error="",
        total_rows=total,
        returned_rows=len(df),
        num_rows=len(df),
        fields=field_names,
        columns=columns,
    )
