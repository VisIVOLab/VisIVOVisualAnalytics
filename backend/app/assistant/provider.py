"""Pluggable LLM provider for the copilot.

The engine is provider-agnostic: it asks a provider to either (a) request a tool
call or (b) return a final answer, given the running message history and the
tool catalogue. This module defines the contract plus:

  * ``NullProvider`` — the default when no API key is configured. It cannot
    reason, so it returns a clear "not configured" answer. The rest of the
    system (tool catalogue, deterministic tool execution, provenance) still works.
  * ``AnthropicProvider`` — a thin adapter used when the ``anthropic`` SDK is
    importable AND ``ANTHROPIC_API_KEY`` is set. It is intentionally minimal;
    outbound network access is a deployment concern.

Wiring a real provider is a deployment step — no key ships with the app.
"""
from __future__ import annotations

import logging
import os
from dataclasses import dataclass, field
from typing import Any

logger = logging.getLogger("visivo.assistant")


@dataclass
class ToolCall:
    """A provider's request to run one tool."""
    id: str
    name: str
    arguments: dict[str, Any]


@dataclass
class ProviderResponse:
    """One provider turn: either tool calls to run, or a final answer (or both;
    the engine runs the tools first when present)."""
    tool_calls: list[ToolCall] = field(default_factory=list)
    final_text: str | None = None


class AssistantProvider:
    """Contract a provider must satisfy."""

    name: str = "base"
    configured: bool = False

    def complete(
        self, system: str, messages: list[dict[str, Any]], tools: list[dict[str, Any]]
    ) -> ProviderResponse:
        raise NotImplementedError


class NullProvider(AssistantProvider):
    """Fallback used when no LLM is configured."""

    name = "null"
    configured = False

    def complete(
        self, system: str, messages: list[dict[str, Any]], tools: list[dict[str, Any]]
    ) -> ProviderResponse:
        return ProviderResponse(final_text=(
            "The AI copilot is not configured on this server: no LLM API key is "
            "set. The tool catalogue and deterministic analysis tools are still "
            "available via /v1/assistant/tools and can be called directly. To "
            "enable natural-language answers, set ANTHROPIC_API_KEY (or wire "
            "another provider) and restart the backend."
        ))


class AnthropicProvider(AssistantProvider):
    """Minimal Anthropic Messages adapter (tool use). Active only when the SDK
    is importable and an API key is present."""

    name = "anthropic"
    configured = True

    def __init__(self, api_key: str, model: str | None = None) -> None:
        import anthropic  # noqa: PLC0415

        self._client = anthropic.Anthropic(api_key=api_key)
        self._model = model or os.environ.get("VISIVO_ASSISTANT_MODEL", "claude-opus-4-8")

    def complete(
        self, system: str, messages: list[dict[str, Any]], tools: list[dict[str, Any]]
    ) -> ProviderResponse:
        resp = self._client.messages.create(
            model=self._model,
            max_tokens=1024,
            system=system,
            tools=tools,
            messages=messages,
        )
        calls: list[ToolCall] = []
        text_parts: list[str] = []
        for block in resp.content:
            btype = getattr(block, "type", None)
            if btype == "tool_use":
                calls.append(ToolCall(id=block.id, name=block.name,
                                      arguments=dict(block.input or {})))
            elif btype == "text":
                text_parts.append(block.text)
        text = "".join(text_parts) or None
        # Surface a truncated answer rather than returning a cut-off sentence as
        # if it were complete.
        if text and getattr(resp, "stop_reason", None) == "max_tokens":
            text += "\n\n[answer truncated at the token limit]"
        return ProviderResponse(tool_calls=calls, final_text=text)


def provider_from_env() -> AssistantProvider:
    """Pick a provider from the environment, else the NullProvider.

    Distinguishes "no key" from "key set but the ``anthropic`` SDK is missing /
    misconfigured" in the logs, so a deployment that sets a key but forgot to
    install the SDK isn't silently degraded without explanation.
    """
    key = os.environ.get("ANTHROPIC_API_KEY")
    if not key:
        return NullProvider()
    try:
        return AnthropicProvider(key)
    except ImportError:
        logger.warning(
            "[assistant] ANTHROPIC_API_KEY is set but the 'anthropic' SDK is not "
            "installed — falling back to the null provider. `pip install anthropic`."
        )
        return NullProvider()
    except Exception as exc:  # noqa: BLE001
        logger.warning("[assistant] Anthropic provider init failed (%s) — null provider.", exc)
        return NullProvider()
