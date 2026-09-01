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
from ..assistant.provider import configure as _configure_provider, public_config
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


class AssistantConfigRequest(BaseModel):
    # User's choice of LLM back-end. Any field omitted is left unchanged; an
    # empty string clears it. Keys are held in server memory only.
    provider: str | None = None            # "auto" | "anthropic" | "openai" | "null"
    anthropic_key: str | None = None       # Claude
    openai_key: str | None = None          # ChatGPT (OpenAI cloud)
    base_url: str | None = None            # self-hosted / OpenAI-compatible server URL
    base_url_key: str | None = None        # key for the self-hosted endpoint (kept
                                           # separate from openai_key for security)
    model: str | None = None


# Cache the provider at module scope — provider_from_env() otherwise rebuilds an
# SDK client on every /status and /query call. Invalidated when the config
# changes via /v1/assistant/config.
_PROVIDER = None


def _get_provider():
    global _PROVIDER
    if _PROVIDER is None:
        _PROVIDER = provider_from_env()
    return _PROVIDER


def _invalidate_provider() -> None:
    global _PROVIDER
    _PROVIDER = None


@router.get("/v1/assistant/status", tags=["assistant"])
async def assistant_status(_: None = _auth) -> dict:
    """Which LLM provider is active + the (non-secret) config for the UI."""
    provider = _get_provider()
    return {
        "configured": provider.configured,
        "provider": provider.name,
        "n_tools": len(tool_specs()),
        "config": public_config(),
    }


@router.post("/v1/assistant/config", tags=["assistant"])
async def assistant_config(request: AssistantConfigRequest, _: None = _auth) -> dict:
    """Choose the LLM back-end at runtime — Claude, OpenAI/ChatGPT, or your own
    server (base_url of an OpenAI-compatible endpoint). Keys are stored in memory
    only (not persisted, never logged/returned). Rebuilds the active provider."""
    try:
        _configure_provider(
            provider=request.provider, anthropic_key=request.anthropic_key,
            openai_key=request.openai_key, base_url=request.base_url,
            base_url_key=request.base_url_key, model=request.model)
    except ValueError as exc:
        raise HTTPException(422, str(exc)) from exc
    _invalidate_provider()
    provider = _get_provider()
    return {
        "valid": True,
        "configured": provider.configured,
        "provider": provider.name,
        "config": public_config(),
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
    dataset_entry: dict | None = None
    if request.dataset_id:
        dataset_entry = _require_dataset(session, request.dataset_id)
        # Not every dataset kind carries a filesystem "path" (a VBT is memory-
        # mapped and exposed via the entry itself), so resolve it defensively —
        # context-aware tools use the entry directly.
        dataset_path = dataset_entry.get("path")

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
        asyncio.to_thread(engine.run, request.question, dataset_path,
                          request.dataset_id, dataset_entry))
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
