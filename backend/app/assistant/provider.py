"""Pluggable LLM providers for the copilot — user-selectable.

The engine speaks a NEUTRAL conversation history (see engine.py); each provider
translates it to its own wire format, so the same grounded, tool-calling copilot
works across three back-ends the user can choose between:

  * ``anthropic``  — Claude, via the ``anthropic`` SDK + an API key.
  * ``openai``     — ChatGPT (OpenAI cloud) OR any OpenAI-compatible endpoint
    (vLLM / Ollama / a self-hosted model on the user's own GPU server), via the
    ``openai`` SDK with a base_url. Same code path — only the base_url differs.
  * ``null``       — no LLM configured; the tool catalogue still works.

Selection is driven by a runtime-settable config (initialised from env), so the
desktop app can offer a settings panel: pick the provider and paste a key or a
server URL. Keys live in server memory only — never logged, never returned.
"""
from __future__ import annotations

import json
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
    """One provider turn: tool calls to run, and/or a final answer."""
    tool_calls: list[ToolCall] = field(default_factory=list)
    final_text: str | None = None


class AssistantProvider:
    name: str = "base"
    configured: bool = False

    def complete(
        self, system: str, history: list[dict[str, Any]], tools: list[dict[str, Any]]
    ) -> ProviderResponse:
        raise NotImplementedError


class NullProvider(AssistantProvider):
    name = "null"
    configured = False

    def complete(self, system, history, tools) -> ProviderResponse:
        return ProviderResponse(final_text=(
            "The AI copilot is not configured on this server: no LLM provider is "
            "selected. The tool catalogue and deterministic analysis tools are "
            "still available. To enable natural-language answers, choose a "
            "provider (Claude / OpenAI / your own server) and supply a key or URL."
        ))


class AnthropicProvider(AssistantProvider):
    name = "anthropic"
    configured = True

    def __init__(self, api_key: str, model: str | None = None) -> None:
        import anthropic  # noqa: PLC0415

        self._client = anthropic.Anthropic(api_key=api_key)
        self._model = model or os.environ.get("VISIVO_ASSISTANT_MODEL", "claude-opus-4-8")

    @staticmethod
    def _to_messages(history: list[dict[str, Any]]) -> list[dict[str, Any]]:
        msgs: list[dict[str, Any]] = []
        for turn in history:
            role = turn["role"]
            if role == "user":
                msgs.append({"role": "user", "content": turn["text"]})
            elif role == "assistant":
                if "tool_calls" in turn:
                    msgs.append({"role": "assistant", "content": [
                        {"type": "tool_use", "id": c.id, "name": c.name, "input": c.arguments}
                        for c in turn["tool_calls"]
                    ]})
                else:   # a prior conversation answer (follow-up context)
                    msgs.append({"role": "assistant", "content": turn.get("text", "")})
            elif role == "tool":
                blocks = []
                for r in turn["results"]:
                    b = {"type": "tool_result", "tool_use_id": r["id"], "content": r["content"]}
                    if r.get("is_error"):
                        b["is_error"] = True
                    blocks.append(b)
                msgs.append({"role": "user", "content": blocks})
        return msgs

    def complete(self, system, history, tools) -> ProviderResponse:
        resp = self._client.messages.create(
            model=self._model, max_tokens=1024, system=system, tools=tools,
            messages=self._to_messages(history))
        calls, text_parts = [], []
        for block in resp.content:
            if getattr(block, "type", None) == "tool_use":
                calls.append(ToolCall(id=block.id, name=block.name,
                                      arguments=dict(block.input or {})))
            elif getattr(block, "type", None) == "text":
                text_parts.append(block.text)
        text = "".join(text_parts) or None
        if text and getattr(resp, "stop_reason", None) == "max_tokens":
            text += "\n\n[answer truncated at the token limit]"
        return ProviderResponse(tool_calls=calls, final_text=text)


class OpenAIProvider(AssistantProvider):
    """OpenAI cloud OR any OpenAI-compatible server (vLLM / Ollama / self-hosted).

    The ONLY difference between "ChatGPT" and "your own model on your GPU server"
    is ``base_url`` — both speak the same chat-completions + tools protocol.
    """

    configured = True   # a built OpenAI/self-hosted provider IS configured

    def __init__(self, api_key: str | None, base_url: str | None = None,
                 model: str | None = None, name: str = "openai") -> None:
        import openai  # noqa: PLC0415

        # Self-hosted endpoints often need no real key; pass a placeholder so the
        # SDK is happy while the server ignores it.
        self._client = openai.OpenAI(api_key=api_key or "not-needed", base_url=base_url or None)
        self._model = model or os.environ.get("VISIVO_ASSISTANT_MODEL", "gpt-4o")
        self.name = name

    @staticmethod
    def _to_messages(system: str, history: list[dict[str, Any]]) -> list[dict[str, Any]]:
        msgs: list[dict[str, Any]] = [{"role": "system", "content": system}]
        for turn in history:
            role = turn["role"]
            if role == "user":
                msgs.append({"role": "user", "content": turn["text"]})
            elif role == "assistant":
                if "tool_calls" in turn:
                    msgs.append({"role": "assistant", "content": None, "tool_calls": [
                        {"id": c.id, "type": "function",
                         "function": {"name": c.name, "arguments": json.dumps(c.arguments)}}
                        for c in turn["tool_calls"]
                    ]})
                else:   # a prior conversation answer (follow-up context)
                    msgs.append({"role": "assistant", "content": turn.get("text", "")})
            elif role == "tool":
                # One tool message per result, keyed by tool_call_id.
                for r in turn["results"]:
                    msgs.append({"role": "tool", "tool_call_id": r["id"],
                                 "content": r["content"]})
        return msgs

    @staticmethod
    def _to_tools(tools: list[dict[str, Any]]) -> list[dict[str, Any]]:
        # Remap our {name, description, input_schema} → OpenAI function tools.
        return [{"type": "function", "function": {
            "name": t["name"], "description": t.get("description", ""),
            "parameters": t.get("input_schema", {"type": "object", "properties": {}}),
        }} for t in tools]

    def complete(self, system, history, tools) -> ProviderResponse:
        resp = self._client.chat.completions.create(
            model=self._model, max_tokens=1024,
            messages=self._to_messages(system, history),
            tools=self._to_tools(tools), tool_choice="auto")
        choice = resp.choices[0]
        msg = choice.message
        calls = []
        for tc in (msg.tool_calls or []):
            try:
                args = json.loads(tc.function.arguments or "{}")
            except (TypeError, ValueError):
                args = {}
            calls.append(ToolCall(id=tc.id, name=tc.function.name, arguments=args))
        text = (msg.content or None) if not calls else (msg.content or None)
        if text and choice.finish_reason == "length":
            text += "\n\n[answer truncated at the token limit]"
        return ProviderResponse(tool_calls=calls, final_text=text)


# ── User-selectable configuration (runtime-settable, env-initialised) ─────────
#
# The CLOUD OpenAI key (``openai_key``) and the SELF-HOSTED endpoint key
# (``base_url_key``) are kept SEPARATE and are never interchanged: the cloud key
# is used only when there is no base_url, and a self-hosted endpoint only ever
# receives its own base_url_key. This prevents an authenticated caller from
# setting a base_url of their choosing and having a stored cloud OpenAI key sent
# to it (key exfiltration).

_CONFIG: dict[str, Any] = {
    "provider": os.environ.get("VISIVO_ASSISTANT_PROVIDER", "auto"),  # auto|anthropic|openai|null
    "anthropic_key": os.environ.get("ANTHROPIC_API_KEY") or None,
    "openai_key": os.environ.get("OPENAI_API_KEY") or None,           # CLOUD OpenAI only
    "base_url": os.environ.get("VISIVO_LLM_BASE_URL") or None,        # self-hosted / OpenAI-compat
    "base_url_key": os.environ.get("VISIVO_LLM_API_KEY") or None,     # self-hosted endpoint key
    "model": os.environ.get("VISIVO_ASSISTANT_MODEL") or None,
}


def _sanitize_base_url(url: str) -> str:
    """Validate a self-hosted base URL. Rejects embedded credentials
    (``user:pass@host``) and query/fragment secrets so a stored URL can be shown
    back in the UI without leaking secrets, and can't smuggle auth to a proxy."""
    from urllib.parse import urlparse  # noqa: PLC0415

    p = urlparse(url)
    if p.scheme not in ("http", "https") or not p.netloc:
        raise ValueError("base_url must be a http(s) URL, e.g. http://host:8000/v1")
    if p.username or p.password:
        raise ValueError("base_url must not contain embedded credentials (user:pass@)")
    if p.query or p.fragment:
        raise ValueError("base_url must not contain a query string or fragment")
    return url.rstrip("/")


def configure(**kwargs: Any) -> None:
    """Update the runtime config; only the keys present (and non-None) change.
    An empty string clears a value. Secrets are held in memory, never logged."""
    for k, v in kwargs.items():
        if k not in _CONFIG or v is None:
            continue
        if v == "":
            _CONFIG[k] = None
        elif k == "base_url":
            _CONFIG[k] = _sanitize_base_url(str(v))
        else:
            _CONFIG[k] = v


def public_config() -> dict[str, Any]:
    """Non-secret view for the UI: which provider, whether keys/url are set. Keys
    are NEVER returned; base_url is sanitised (no embedded credentials)."""
    return {
        "provider": _CONFIG["provider"],
        "anthropic_key_set": bool(_CONFIG["anthropic_key"]),
        "openai_key_set": bool(_CONFIG["openai_key"]),
        "base_url": _CONFIG["base_url"],
        "base_url_key_set": bool(_CONFIG["base_url_key"]),
        "model": _CONFIG["model"],
        "available": ["anthropic", "openai", "null"],
    }


def _resolve_provider_name() -> str:
    p = (_CONFIG.get("provider") or "auto").lower()
    if p != "auto":
        return p
    # Auto: prefer a self-hosted/OpenAI endpoint or key, then Anthropic, else null.
    if _CONFIG.get("base_url") or _CONFIG.get("openai_key"):
        return "openai"
    if _CONFIG.get("anthropic_key"):
        return "anthropic"
    return "null"


def provider_from_env() -> AssistantProvider:
    """Build the currently-selected provider, degrading to NullProvider on any
    missing SDK / bad config (logged distinctly so 'SDK missing' ≠ 'no key')."""
    name = _resolve_provider_name()
    try:
        if name == "anthropic":
            if not _CONFIG.get("anthropic_key"):
                return NullProvider()
            return AnthropicProvider(_CONFIG["anthropic_key"], _CONFIG.get("model"))
        if name == "openai":
            if _CONFIG.get("base_url"):
                # Self-hosted: use ONLY the dedicated endpoint key, never the
                # cloud openai_key (which could otherwise be exfiltrated).
                return OpenAIProvider(_CONFIG.get("base_url_key"), _CONFIG["base_url"],
                                      _CONFIG.get("model"), name="self-hosted")
            if _CONFIG.get("openai_key"):
                return OpenAIProvider(_CONFIG["openai_key"], None,
                                      _CONFIG.get("model"), name="openai")
            return NullProvider()
    except ImportError:
        logger.warning("[assistant] provider '%s' selected but its SDK is not "
                       "installed — falling back to null.", name)
        return NullProvider()
    except Exception as exc:  # noqa: BLE001
        logger.warning("[assistant] provider '%s' init failed (%s) — null.", name, exc)
        return NullProvider()
    return NullProvider()
