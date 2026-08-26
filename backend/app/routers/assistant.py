"""Scientific copilot endpoints (R-22).

A provenance-bound, tool-calling assistant over the existing validated workers.
The LLM provider is pluggable and defaults to a NullProvider when no API key is
configured — the tool catalogue and deterministic tools still work.
"""
from __future__ import annotations

import asyncio
import logging
import os

from fastapi import APIRouter, Depends, HTTPException, status
from pydantic import BaseModel

from ..assistant import AssistantEngine, provider_from_env
from ..assistant.tools import tool_specs
from ..auth import verify_token
from ..dependencies import _require_dataset
from ..sessions import Session, get_session

router = APIRouter(tags=["assistant"])
logger = logging.getLogger("visivo.backend")
_auth = Depends(verify_token)

# Total wall-clock budget for one copilot query (it may run several tool calls,
# each a real compute). Bounds the DoS surface of the tool-executing loop.
_ASSISTANT_TIMEOUT_S = float(os.environ.get("VISIVO_ASSISTANT_TIMEOUT_S", "120"))


class AssistantQueryRequest(BaseModel):
    question: str
    dataset_id: str | None = None


@router.get("/v1/assistant/status", tags=["assistant"])
async def assistant_status(_: None = _auth) -> dict:
    """Whether an LLM provider is configured, and which one."""
    provider = provider_from_env()
    return {
        "configured": provider.configured,
        "provider": provider.name,
        "n_tools": len(tool_specs()),
    }


@router.get("/v1/assistant/tools", tags=["assistant"])
async def assistant_tools(_: None = _auth) -> dict:
    """The tool catalogue the copilot can call (also usable directly)."""
    return {"tools": tool_specs()}


@router.post("/v1/assistant/query", tags=["assistant"])
async def assistant_query(
    request: AssistantQueryRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> dict:
    """Answer a natural-language question, grounding every number in a tool call.

    Returns the answer, the recorded tool calls (provenance), and a
    reproducibility script. With no provider configured the answer explains how
    to enable one; the tool catalogue remains available.
    """
    if not request.question or not request.question.strip():
        raise HTTPException(422, "A non-empty question is required.")

    dataset_path: str | None = None
    if request.dataset_id:
        entry = _require_dataset(session, request.dataset_id)
        dataset_path = entry["path"]

    # Per-session admission: a copilot query drives real compute (moment/cube
    # stats), so it counts against the same per-session concurrency budget as
    # other heavy tasks (HTTP 429 when exceeded) and is bounded by a total
    # wall-clock timeout (HTTP 504) — the tool loop must not run unbounded.
    if not session.try_acquire_task_slot():
        raise HTTPException(
            status.HTTP_429_TOO_MANY_REQUESTS,
            "Too many concurrent compute tasks for this session.",
        )
    engine = AssistantEngine(provider_from_env())
    try:
        result = await asyncio.wait_for(
            asyncio.to_thread(engine.run, request.question, dataset_path, request.dataset_id),
            timeout=_ASSISTANT_TIMEOUT_S,
        )
    except asyncio.TimeoutError:
        raise HTTPException(
            status.HTTP_504_GATEWAY_TIMEOUT,
            f"Assistant query exceeded the {_ASSISTANT_TIMEOUT_S:.0f}s budget.",
        ) from None
    except Exception as exc:  # noqa: BLE001
        logger.warning("[assistant] query failed: %s", exc)
        raise HTTPException(500, "Assistant query failed.") from exc
    finally:
        session.release_task_slot()
    return result.to_dict()
