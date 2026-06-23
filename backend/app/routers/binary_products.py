"""Binary-transport variants of the moment-map and PV-diagram products.

Restored after a router refactor dropped these routes while the desktop client
(BackendClient::requestMomentBinary / requestPvBinary) kept POSTing to them.

Both reuse the cached product helpers (``_moment_product_payload`` /
``_pv_product_payload``) so they share the exact same compute + product cache
as the JSON/task variants — a second identical request is served from cache and
the ``X-Visivo-From-Cache`` header reflects it. The cached payload's base64
array is unpacked back to raw float32 and re-emitted in the 12-byte-header
Visivo binary wire format that ``BackendClient::parseBinaryFrame()`` expects.
"""
import base64
import logging
import zlib

import numpy as np
from fastapi import APIRouter, Depends, HTTPException, Response

from ..auth import verify_token
from ..dependencies import (
    _moment_product_payload,
    _pv_product_payload,
    _require_cube_path,
    _require_dataset,
)
from ..schemas import CubePvRequest, MomentProductRequest
from ..sessions import Session, get_session
from .spectral import _array_to_binary

router = APIRouter(tags=["binary"])
logger = logging.getLogger("visivo.backend")
_auth = Depends(verify_token)


def _decode_payload_floats(result: dict) -> np.ndarray:
    """Decode a product payload's ``data_base64`` to a flat float32 array.

    Handles both the plain (moment) and qt-zlib-compressed (PV) encodings. The
    qt-zlib stream is a 4-byte big-endian uncompressed-length prefix followed by
    a raw zlib stream (Qt's qCompress convention).
    """
    raw = base64.b64decode(result["data_base64"])
    if result.get("compression") == "qt-zlib":
        raw = zlib.decompress(raw[4:])
    return np.frombuffer(raw, dtype="<f4")


@router.post("/v1/moment/result/binary", tags=["binary"])
async def moment_result_binary(
    request: MomentProductRequest,
    response: Response,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> Response:
    """Moment map as a raw Visivo binary frame (height × width float32)."""
    entry = _require_dataset(session, request.dataset_id)
    try:
        result, cache_hit = await _moment_product_payload(entry, request)
    except HTTPException:
        raise
    except ValueError as exc:
        raise HTTPException(422, str(exc)) from exc
    except Exception as exc:  # noqa: BLE001
        logger.warning("[moment/binary] failed dataset_id=%s error=%s", request.dataset_id, exc)
        raise HTTPException(500, str(exc)) from exc

    h, w = int(result["height"]), int(result["width"])
    arr = _decode_payload_floats(result).reshape(h, w)
    return Response(
        content=_array_to_binary(h, w, arr),
        media_type="application/octet-stream",
        headers={"X-Visivo-From-Cache": "true" if cache_hit else "false"},
    )


@router.post("/v1/pv/result/binary", tags=["binary"])
async def pv_result_binary(
    request: CubePvRequest,
    response: Response,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> Response:
    """PV diagram as a raw Visivo binary frame (depth × num_samples float32)."""
    # _require_cube_path enforces the 3-D-cube precondition with a 422.
    _require_cube_path(session, request.dataset_id)
    entry = _require_dataset(session, request.dataset_id)
    try:
        result, cache_hit = await _pv_product_payload(entry, request)
    except HTTPException:
        raise
    except ValueError as exc:
        raise HTTPException(422, str(exc)) from exc
    except Exception as exc:  # noqa: BLE001
        logger.warning("[pv/binary] failed dataset_id=%s error=%s", request.dataset_id, exc)
        raise HTTPException(500, str(exc)) from exc

    depth, cols = int(result["depth"]), int(result["num_samples"])
    arr = _decode_payload_floats(result).reshape(depth, cols)
    return Response(
        content=_array_to_binary(depth, cols, arr),
        media_type="application/octet-stream",
        headers={"X-Visivo-From-Cache": "true" if cache_hit else "false"},
    )
