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
import threading
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
    #: What stops this provider from answering, for the status route; "" if
    #: nothing is known to. Never carries a secret.
    problem: str = ""

    @property
    def model_in_use(self) -> str:
        """The model questions go to now, for the status route; "" if none is
        known yet (a self-hosted server asked only at the first question)."""
        return getattr(self, "_model", None) or ""

    def complete(
        self, system: str, history: list[dict[str, Any]], tools: list[dict[str, Any]]
    ) -> ProviderResponse:
        raise NotImplementedError


class NullProvider(AssistantProvider):
    name = "null"
    configured = False

    def __init__(self, problem: str = "") -> None:
        # A reason given here (a refused VISIVO_LLM_BASE_URL) replaces the
        # generic "nothing selected", so the operator is told what to fix.
        self.problem = problem

    def complete(self, system, history, tools) -> ProviderResponse:
        if self.problem:
            return ProviderResponse(final_text=(
                f"The AI copilot is off on this server: {self.problem} The tool "
                "catalogue and deterministic analysis tools are still available."))
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
        self._model = model or os.environ.get("VISIVO_ASSISTANT_MODEL") or None
        if not self._model and not base_url:
            # Cloud OpenAI: a model name that exists there.
            self._model = "gpt-4o"
        # Self-hosted with no model: NOT "gpt-4o", which such a server rarely
        # serves (the request then fails with an opaque 404). The server is
        # asked at the first question instead; see _resolve_model.
        self.name = name
        # One discovery at a time: the provider is cached on the session and
        # two questions can arrive together.
        self._model_lock = threading.Lock()

    def _resolve_model(self) -> tuple[str | None, str]:
        """The model to request: the configured one, or the server's only one.

        Asked lazily, at the first question, so building the provider (and so
        the status route and start-up) never waits on a server that may be
        down. When the server lists exactly one model it is used: that is the
        usual vLLM deployment, and there is no ambiguity. None or several is a
        choice the operator has to make; :attr:`problem` says which, with the
        list, and a failure is not cached, so a fix on the server is picked up
        at the next question.

        Returns ``(model, "")`` or ``(None, why)``: the reason is this call's
        own, so a concurrent question cannot change it under the caller.
        """
        if self._model:
            return self._model, ""
        with self._model_lock:
            if self._model:          # resolved by a question that held the lock
                return self._model, ""
            problem = self._list_problem_or_pick()
            self.problem = problem
            return (None, problem) if problem else (self._model, "")

    def _list_problem_or_pick(self) -> str:
        """Ask the server; set ``_model`` when it serves one, else say why not."""
        hint = ("Set VISIVO_ASSISTANT_MODEL on the backend, or the Model field "
                "in Settings ▸ Scientific Copilot.")
        try:
            listing = self._client.with_options(
                timeout=_MODEL_LIST_TIMEOUT_S, max_retries=0).models.list()
            ids = sorted({str(m.id) for m in listing if getattr(m, "id", None)})
        except Exception as exc:  # noqa: BLE001 — any failure is reported, not raised
            # The class name only: an SDK message can quote the request.
            return ("No model is set for the self-hosted endpoint and its model "
                    f"list could not be read ({type(exc).__name__}). {hint}")
        if len(ids) == 1:
            self._model = ids[0]
            logger.info("[assistant] no model set; using the endpoint's only model %r",
                        self._model)
            return ""
        if not ids:
            return f"No model is set and the self-hosted endpoint lists none. {hint}"
        shown = ", ".join(ids[:_MODEL_LIST_SHOWN])
        more = (f" and {len(ids) - _MODEL_LIST_SHOWN} more"
                if len(ids) > _MODEL_LIST_SHOWN else "")
        return ("No model is set and the self-hosted endpoint serves "
                f"{len(ids)}: {shown}{more}. {hint}")

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
        model, problem = self._resolve_model()
        if model is None:
            # Said as the answer, like NullProvider: the question cannot be put
            # to a model nobody named.
            return ProviderResponse(final_text=f"The AI copilot cannot answer: {problem}")
        resp = self._client.chat.completions.create(
            model=model, max_tokens=1024,
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


#: How long the first question waits for a self-hosted server's model list.
_MODEL_LIST_TIMEOUT_S = 10.0
#: Model names quoted when a server lists several.
_MODEL_LIST_SHOWN = 10


# ── User-selectable configuration (runtime-settable, env-initialised) ─────────
#
# The CLOUD OpenAI key (``openai_key``) and the SELF-HOSTED endpoint key
# (``base_url_key``) are kept SEPARATE and are never interchanged: the cloud key
# is used only when there is no base_url, and a self-hosted endpoint only ever
# receives its own base_url_key. This prevents an authenticated caller from
# setting a base_url of their choosing and having a stored cloud OpenAI key sent
# to it (key exfiltration).

def default_config() -> dict[str, Any]:
    """A fresh provider config seeded from the environment. Each SESSION gets its
    own copy so users sharing a backend configure independent providers/keys."""
    base_url, base_url_error = _env_base_url()
    return {
        "provider": os.environ.get("VISIVO_ASSISTANT_PROVIDER", "auto"),  # auto|anthropic|openai|null
        "anthropic_key": os.environ.get("ANTHROPIC_API_KEY") or None,
        "openai_key": os.environ.get("OPENAI_API_KEY") or None,           # CLOUD OpenAI only
        "base_url": base_url,                                             # self-hosted / OpenAI-compat
        "base_url_key": os.environ.get("VISIVO_LLM_API_KEY") or None,     # self-hosted endpoint key
        "model": os.environ.get("VISIVO_ASSISTANT_MODEL") or None,
        # Why VISIVO_LLM_BASE_URL was refused, or None. Not a setting: it keeps
        # the copilot off (rather than quietly using another provider) until
        # the client sets or clears base_url for the session.
        "base_url_error": base_url_error,
    }


def _env_base_url() -> tuple[str | None, str | None]:
    """VISIVO_LLM_BASE_URL, checked like a base URL set from the client.

    A refused value disables the copilot instead of stopping the backend: the
    copilot is optional, and everything else the backend does is not. The URL
    itself is never repeated, in the message or the log, because what made it
    unacceptable may be the credentials inside it.
    """
    raw = (os.environ.get("VISIVO_LLM_BASE_URL") or "").strip()
    if not raw:
        return None, None
    try:
        return _sanitize_base_url(raw, name="VISIVO_LLM_BASE_URL"), None
    except ValueError as exc:
        return None, f"{exc}. The self-hosted copilot stays off until it is corrected."


def _sanitize_base_url(url: str, name: str = "base_url") -> str:
    """Validate a self-hosted base URL. Rejects embedded credentials
    (``user:pass@host``) and query/fragment secrets so a stored URL can be shown
    back in the UI without leaking secrets, and can't smuggle auth to a proxy.
    ``name`` is what the message calls the value: the field, or the variable."""
    from urllib.parse import urlparse  # noqa: PLC0415

    try:
        p = urlparse(url)
    except ValueError:
        # The parser's own message can quote the netloc, credentials and all.
        raise ValueError(f"{name} is not a valid URL, e.g. http://host:8000/v1") from None
    if p.scheme not in ("http", "https") or not p.netloc:
        raise ValueError(f"{name} must be a http(s) URL, e.g. http://host:8000/v1")
    if p.username or p.password:
        raise ValueError(f"{name} must not contain embedded credentials (user:pass@); "
                         "the key belongs in VISIVO_LLM_API_KEY")
    if p.query or p.fragment:
        raise ValueError(f"{name} must not contain a query string or fragment")
    return url.rstrip("/")


# Process default (env-based). Per-session configs default to a copy of this.
_CONFIG: dict[str, Any] = default_config()
if _CONFIG["base_url_error"]:
    # Once, at start-up; the copilot's answers and status route repeat it.
    logger.warning("[assistant] %s", _CONFIG["base_url_error"])


def configure(config: dict[str, Any] | None = None, **kwargs: Any) -> None:
    """Update a config (the process default, or a per-session one via ``config``);
    only the keys present (and non-None) change. An empty string clears a value.
    Secrets are held in memory, never logged."""
    cfg = _CONFIG if config is None else config
    for k, v in kwargs.items():
        if k not in cfg or v is None:
            continue
        if v == "":
            cfg[k] = None
        elif k == "base_url":
            cfg[k] = _sanitize_base_url(str(v))
        else:
            cfg[k] = v
        if k == "base_url":
            # The session chose its own endpoint (or none): a refused
            # VISIVO_LLM_BASE_URL no longer applies to it.
            cfg["base_url_error"] = None


def public_config(config: dict[str, Any] | None = None) -> dict[str, Any]:
    """Non-secret view for the UI: which provider, whether keys/url are set. Keys
    are NEVER returned; base_url is sanitised (no embedded credentials)."""
    cfg = _CONFIG if config is None else config
    return {
        "provider": cfg["provider"],
        "anthropic_key_set": bool(cfg["anthropic_key"]),
        "openai_key_set": bool(cfg["openai_key"]),
        "base_url": cfg["base_url"],
        "base_url_key_set": bool(cfg["base_url_key"]),
        "model": cfg["model"],
        "available": ["anthropic", "openai", "null"],
        "base_url_error": cfg.get("base_url_error"),
    }


def _resolve_provider_name(config: dict[str, Any] | None = None) -> str:
    cfg = _CONFIG if config is None else config
    p = (cfg.get("provider") or "auto").lower()
    if p != "auto":
        return p
    # Auto: prefer a self-hosted/OpenAI endpoint or key, then Anthropic, else null.
    # A refused VISIVO_LLM_BASE_URL still counts as "a self-hosted endpoint was
    # meant": falling through to another key would send the questions somewhere
    # the operator did not choose.
    if cfg.get("base_url") or cfg.get("base_url_error") or cfg.get("openai_key"):
        return "openai"
    if cfg.get("anthropic_key"):
        return "anthropic"
    return "null"


def provider_from_env(config: dict[str, Any] | None = None) -> AssistantProvider:
    """Build the selected provider for a config, degrading to NullProvider on any
    missing SDK / bad config (logged distinctly so 'SDK missing' ≠ 'no key')."""
    cfg = _CONFIG if config is None else config
    name = _resolve_provider_name(cfg)
    try:
        if name == "anthropic":
            if not cfg.get("anthropic_key"):
                return NullProvider()
            return AnthropicProvider(cfg["anthropic_key"], cfg.get("model"))
        if name == "openai":
            if cfg.get("base_url_error") and not cfg.get("base_url"):
                # Also with an explicit provider=openai: the operator asked for
                # a self-hosted endpoint, so its cloud key is not a stand-in.
                return NullProvider(cfg["base_url_error"])
            if cfg.get("base_url"):
                # Self-hosted: use ONLY the dedicated endpoint key, never the
                # cloud openai_key (which could otherwise be exfiltrated).
                return OpenAIProvider(cfg.get("base_url_key"), cfg["base_url"],
                                      cfg.get("model"), name="self-hosted")
            if cfg.get("openai_key"):
                return OpenAIProvider(cfg["openai_key"], None,
                                      cfg.get("model"), name="openai")
            return NullProvider()
    except ImportError:
        logger.warning("[assistant] provider '%s' selected but its SDK is not "
                       "installed, falling back to null.", name)
        return NullProvider()
    except Exception as exc:  # noqa: BLE001
        logger.warning("[assistant] provider '%s' init failed (%s), null.", name, exc)
        return NullProvider()
    return NullProvider()
