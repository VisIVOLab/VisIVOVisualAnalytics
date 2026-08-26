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
from pydantic import BaseModel, Field

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

# A DEDICATED concurrency limit for copilot queries — deliberately NOT the global
# heavy-task semaphore: an assistant query spends much of its time waiting on the
# LLM (network I/O), and holding a heavy slot across that would starve real
# moment/isosurface compute. This bounds concurrent assistant queries instead.
_ASSISTANT_SLOTS = max(1, int(os.environ.get("VISIVO_ASSISTANT_SLOTS", "2")))
_ASSISTANT_SEM: asyncio.Semaphore | None = None


def _assistant_semaphore() -> asyncio.Semaphore:
    global _ASSISTANT_SEM
    if _ASSISTANT_SEM is None:
        _ASSISTANT_SEM = asyncio.Semaphore(_ASSISTANT_SLOTS)
    return _ASSISTANT_SEM


class AssistantQueryRequest(BaseModel):
    # Bounded so an authenticated caller can't send an arbitrarily large prompt
    # (LLM cost-DoS). 4000 chars is ample for a data question.
    question: str = Field(max_length=4000)
    dataset_id: str | None = None


# Cache the provider at module scope — provider_from_env() otherwise rebuilds an
# SDK client on every /status and /query call. An env change (e.g. setting the
# key) takes effect on restart, which is the normal deployment lifecycle.
_PROVIDER = None


def _get_provider():
    global _PROVIDER
    if _PROVIDER is None:
        _PROVIDER = provider_from_env()
    return _PROVIDER


@router.get("/v1/assistant/status", tags=["assistant"])
async def assistant_status(_: None = _auth) -> dict:
    """Whether an LLM provider is configured, and which one."""
    provider = _get_provider()
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

    # Admission control. A copilot query drives real compute (moment/cube stats),
    # so it counts against BOTH the per-session budget (HTTP 429) AND a dedicated
    # assistant-concurrency semaphore. Critically, on a timeout the underlying
    # thread cannot be cancelled and keeps running, so we hold BOTH the session
    # slot and the assistant slot until the thread actually settles (mirroring
    # _run_heavy's E-03 pattern) — releasing them at the 120 s mark would let a
    # session pile up unbounded background compute threads.
    if not session.try_acquire_task_slot():
        raise HTTPException(
            status.HTTP_429_TOO_MANY_REQUESTS,
            "Too many concurrent compute tasks for this session.",
        )
    sem = _assistant_semaphore()
    try:
        await sem.acquire()
    except BaseException:
        # Cancelled/failed while queued on the semaphore, before _release_once is
        # wired — release the session slot we already took so it can't leak.
        session.release_task_slot()
        raise
    _released = False

    def _release_once(_fut: object = None) -> None:
        nonlocal _released
        if not _released:
            _released = True
            sem.release()
            session.release_task_slot()

    engine = AssistantEngine(_get_provider())
    task = asyncio.ensure_future(
        asyncio.to_thread(engine.run, request.question, dataset_path, request.dataset_id))
    try:
        result = await asyncio.wait_for(asyncio.shield(task), timeout=_ASSISTANT_TIMEOUT_S)
        _release_once()
        return result.to_dict()
    except asyncio.TimeoutError:
        # Hold both slots until the still-running thread settles (accurate
        # admission control); the client gets 504 now.
        task.add_done_callback(_release_once)
        raise HTTPException(
            status.HTTP_504_GATEWAY_TIMEOUT,
            f"Assistant query exceeded the {_ASSISTANT_TIMEOUT_S:.0f}s budget.",
        ) from None
    except BaseException:
        if task.done():
            _release_once()
        else:
            task.add_done_callback(_release_once)
        raise
