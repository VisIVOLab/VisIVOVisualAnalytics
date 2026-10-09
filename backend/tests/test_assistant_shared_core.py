"""BE-02: the five additive shared-core adaptations.

Each of the five changes gets (a) a test that the DEFAULT behaviour is what it
always was, and (b) a test that the new parameter does what it claims. The
existing suite is the arbiter of non-breakage; these tests are the arbiter of
the new surface.

Two properties cannot be proved in-process, because by the time this module
runs the session has already imported numpy, astropy and app.assistant.tools:

  * importing ``app.assistant.provider`` pulls nothing third-party;
  * an engine with an INJECTED registry never imports ``app.assistant.tools``.

Those two run in a subprocess with a ``sys.meta_path`` guard that raises on the
denied imports (the technique from BE-01), so the assertion is about a real
fresh interpreter rather than a warm module cache.
"""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import pytest

from app.assistant import AssistantEngine
from app.assistant.engine import SYSTEM_PROMPT, _render_context
from app.assistant.provider import AssistantProvider, ProviderResponse, ToolCall
from app.assistant.tools import TOOLS, AssistantTool, tool_specs

_BACKEND = Path(__file__).resolve().parents[1]


# ── helpers ──────────────────────────────────────────────────────────────────

class _RecordingProvider(AssistantProvider):
    """Answers immediately, recording the system prompt and history it saw."""

    name = "recording"
    configured = True

    def __init__(self, answer: str = "done") -> None:
        self.answer = answer
        self.seen_system: list[str] = []
        self.seen_history: list[list[dict]] = []
        self.seen_specs: list[list[dict]] = []

    def complete(self, system, history, tools) -> ProviderResponse:
        self.seen_system.append(system)
        self.seen_history.append([dict(t) for t in history])
        self.seen_specs.append(tools)
        return ProviderResponse(final_text=self.answer)


class _OneToolThenAnswer(AssistantProvider):
    """Calls one named tool on the first round, then answers."""

    name = "scripted"
    configured = True

    def __init__(self, tool_name: str, arguments: dict | None = None) -> None:
        self.tool_name = tool_name
        self.arguments = arguments or {}
        self.calls = 0
        self.seen_specs: list[list[dict]] = []

    def complete(self, system, history, tools) -> ProviderResponse:
        self.calls += 1
        self.seen_specs.append(tools)
        if self.calls == 1:
            return ProviderResponse(tool_calls=[
                ToolCall(id="c1", name=self.tool_name, arguments=self.arguments)])
        return ProviderResponse(final_text="answered from the injected registry")


def _run_guarded(body: str, denied: tuple[str, ...]) -> subprocess.CompletedProcess:
    """Execute ``body`` in a fresh interpreter where ``denied`` imports raise."""
    script = f"""
import sys

DENIED = {denied!r}

class _Guard:
    def find_spec(self, fullname, path=None, target=None):
        if fullname.split(".")[0] in DENIED or fullname in DENIED:
            raise AssertionError("DENIED IMPORT: " + fullname)
        return None

sys.meta_path.insert(0, _Guard())

{body}
print("GUARDED-OK")
"""
    return subprocess.run(
        [sys.executable, "-c", script],
        cwd=_BACKEND, capture_output=True, text=True, timeout=120,
    )


# ── 1/5 — system_prompt parameter ────────────────────────────────────────────

def test_engine_default_system_prompt_is_the_module_constant():
    """Omitted -> byte-identical prompt behaviour."""
    engine = AssistantEngine(_RecordingProvider())
    assert engine.system_prompt == SYSTEM_PROMPT
    engine.run("q", None)
    assert engine.provider.seen_system == [SYSTEM_PROMPT]


def test_engine_accepts_a_custom_system_prompt():
    provider = _RecordingProvider()
    engine = AssistantEngine(provider, system_prompt="You are a different agent.")
    engine.run("q", None)
    assert provider.seen_system == ["You are a different agent."]
    assert SYSTEM_PROMPT not in provider.seen_system


def test_empty_system_prompt_is_honoured_not_replaced():
    """'' is a deliberate choice; only None means 'use the default'."""
    provider = _RecordingProvider()
    AssistantEngine(provider, system_prompt="").run("q", None)
    assert provider.seen_system == [""]


# ── 2/5 — generic per-request context ────────────────────────────────────────

def _user_turn(provider: _RecordingProvider) -> str:
    history = provider.seen_history[0]
    user_turns = [t for t in history if t.get("role") == "user"]
    return user_turns[-1]["text"]


def test_default_context_builder_is_unchanged_for_a_cube():
    provider = _RecordingProvider()
    AssistantEngine(provider).run("What are the units?", "/tmp/x.fits",
                                  dataset_id="ds1")
    assert _user_turn(provider) == (
        "What are the units?"
        "\n\n[Active dataset id=ds1. Tools operate on it by default.]")


def test_default_context_builder_is_unchanged_for_a_catalogue():
    provider = _RecordingProvider()
    AssistantEngine(provider).run("Describe it", None, dataset_id="cat1",
                                  dataset_entry={"kind": "catalogue"})
    text = _user_turn(provider)
    assert "[Active dataset id=cat1 is a point catalogue." in text
    assert "use cosmology_distance for redshift → distance.]" in text


def test_default_context_builder_still_appends_the_multi_dataset_note():
    provider = _RecordingProvider()
    AssistantEngine(provider).run(
        "Compare them", None, dataset_id="ds1",
        datasets={"ds1": {"kind": "cube"}, "ds2": {"kind": "image"}})
    text = _user_turn(provider)
    assert "[Active dataset id=ds1." in text
    assert "[Several datasets are open. Call list_datasets" in text


def test_no_context_at_all_when_no_dataset_is_given():
    provider = _RecordingProvider()
    AssistantEngine(provider).run("hello", None)
    assert _user_turn(provider) == "hello"


def test_supplied_string_context_replaces_the_builder():
    provider = _RecordingProvider()
    AssistantEngine(provider).run(
        "What is that?", "/tmp/x.fits", dataset_id="ds1",
        context="[Scene: orion. Gaze: orion.m42]")
    text = _user_turn(provider)
    assert text == "What is that?\n\n[Scene: orion. Gaze: orion.m42]"
    # The dataset-specific builder did NOT also run.
    assert "Active dataset id=ds1" not in text


def test_supplied_mapping_context_is_rendered_as_sorted_json():
    provider = _RecordingProvider()
    AssistantEngine(provider).run(
        "What is that?", None,
        context={"scene": "orion", "gaze_target": "orion.m42", "selected": None})
    text = _user_turn(provider)
    payload = json.loads(text.split("\n\n", 1)[1])
    assert payload == {"scene": "orion", "gaze_target": "orion.m42",
                       "selected": None}
    # Deterministic ordering, so a cached prompt prefix stays stable.
    assert text.split("\n\n", 1)[1].index('"gaze_target"') < \
        text.split("\n\n", 1)[1].index('"scene"')


def test_empty_context_means_no_context_block():
    """'' suppresses the builder; None would have run it."""
    provider = _RecordingProvider()
    AssistantEngine(provider).run("hello", "/tmp/x.fits", dataset_id="ds1",
                                  context="")
    assert _user_turn(provider) == "hello"


def test_context_separator_is_normalised_exactly_once():
    assert _render_context("[X]") == "\n\n[X]"
    assert _render_context("\n\n[X]") == "\n\n[X]"
    assert _render_context("   [X]  ") == "\n\n[X]"
    assert _render_context("") == ""
    assert _render_context("   ") == ""


def test_context_is_keyword_only():
    """A new positional slot would shift existing positional call sites."""
    import inspect
    sig = inspect.signature(AssistantEngine.run)
    assert sig.parameters["context"].kind is inspect.Parameter.KEYWORD_ONLY
    # The parameters existing callers pass positionally keep their order.
    positional = [n for n, p in sig.parameters.items()
                  if p.kind is inspect.Parameter.POSITIONAL_OR_KEYWORD]
    assert positional == ["self", "question", "dataset_path", "dataset_id",
                          "dataset_entry", "prior_turns", "progress", "datasets"]


# ── 3/5 — tool_specs(names=None) ─────────────────────────────────────────────

def test_tool_specs_default_is_the_whole_catalogue():
    specs = tool_specs()
    assert len(specs) == len(TOOLS)
    assert [s["name"] for s in specs] == list(TOOLS)


def test_tool_specs_subsets_in_the_requested_order():
    specs = tool_specs(["cube_statistics", "dataset_metadata"])
    assert [s["name"] for s in specs] == ["cube_statistics", "dataset_metadata"]
    # Each entry is still a full provider-ready spec.
    assert set(specs[0]) == {"name", "description", "input_schema"}


def test_tool_specs_empty_list_yields_no_specs():
    assert tool_specs([]) == []


def test_tool_specs_rejects_unknown_names_explicitly():
    with pytest.raises(ValueError) as exc:
        tool_specs(["dataset_metadata", "highlight_object", "no_such_tool"])
    message = str(exc.value)
    assert "highlight_object" in message and "no_such_tool" in message
    assert "dataset_metadata" not in message.split("Registered:")[0]


def test_tool_specs_subset_matches_the_full_rendering_for_the_same_tool():
    name = "dataset_metadata"
    assert tool_specs([name]) == [s for s in tool_specs() if s["name"] == name]


# ── 4/5 — injectable tool registry ───────────────────────────────────────────

def test_engine_default_registry_is_the_scientific_catalogue():
    engine = AssistantEngine(_RecordingProvider())
    assert engine.tools is TOOLS


def test_engine_default_specs_equal_tool_specs():
    """The switch to deriving specs from the registry changed no output."""
    provider = _RecordingProvider()
    AssistantEngine(provider).run("q", None)
    assert provider.seen_specs[0] == tool_specs()


def test_engine_uses_an_injected_registry_for_specs_and_dispatch():
    calls: list[dict] = []

    def _executor(path, params):
        calls.append({"path": path, "params": dict(params)})
        return {"ok": True, "echo": params.get("value")}

    injected = {"probe": AssistantTool(
        name="probe",
        description="A test-only tool.",
        parameters={"type": "object",
                    "properties": {"value": {"type": "string"}},
                    "additionalProperties": False},
        executor=_executor,
        targetable=False,
    )}

    provider = _OneToolThenAnswer("probe", {"value": "hi"})
    engine = AssistantEngine(provider, system_prompt="p", tools=injected)

    assert engine.tools is injected
    result = engine.run("go", None)

    # Only the injected tool was advertised.
    assert [s["name"] for s in provider.seen_specs[0]] == ["probe"]
    # It really ran, and the loop finished on the following turn.
    assert calls == [{"path": None, "params": {"value": "hi"}}]
    assert result.answer == "answered from the injected registry"
    assert result.tool_calls_recorded is True
    assert result.tool_calls[0].name == "probe"
    assert result.tool_calls[0].result == {"ok": True, "echo": "hi"}
    assert result.rounds == 2


def test_injected_action_tool_is_collected_into_actions():
    injected = {"do_thing": AssistantTool(
        name="do_thing",
        description="ACTION: emit a directive.",
        parameters={"type": "object", "properties": {}},
        executor=lambda path, params: {"action": {"type": "do_thing"},
                                       "status": "queued"},
        is_action=True,
        targetable=False,
    )}
    engine = AssistantEngine(_OneToolThenAnswer("do_thing"), tools=injected)
    result = engine.run("go", None)
    assert result.actions == [{"type": "do_thing"}]


def test_an_empty_injected_registry_is_honoured_not_replaced_by_the_default():
    """`tools={}` must mean "no tools", not "fall back to the catalogue".

    The resolution test is `self._tools is None`, not truthiness; an empty dict
    is falsy, so a truthiness test here would silently advertise all 26
    scientific tools to an application that asked for none.
    """
    provider = _RecordingProvider()
    engine = AssistantEngine(provider, tools={})
    assert engine.tools == {}
    assert engine.tools is not TOOLS
    engine.run("q", None)
    assert provider.seen_specs[0] == []


def test_unknown_tool_is_still_rejected_against_an_injected_registry():
    injected = {"probe": AssistantTool(
        name="probe", description="d", parameters={"type": "object"},
        executor=lambda path, params: {}, targetable=False)}
    engine = AssistantEngine(_OneToolThenAnswer("not_registered"), tools=injected)
    result = engine.run("go", None)
    assert result.tool_calls[0].error == "unknown tool 'not_registered'"


def test_engine_module_does_not_import_tools_at_module_level():
    """Static guarantee behind the runtime one: no top-level `.tools` import."""
    source = (_BACKEND / "app" / "assistant" / "engine.py").read_text()
    top_level = [ln for ln in source.splitlines()
                 if ln.startswith("from .tools") or ln.startswith("import .tools")]
    assert top_level == []


def test_injected_registry_engine_never_imports_tools_or_the_scientific_stack():
    """A fresh interpreter, with numpy/astropy/tools imports made fatal.

    This is the BE-01 Finding 3 inversion, proved end to end: a complete
    scripted turn (tool call, execution, final answer) with an injected
    registry, in a process where importing tools.py or its dependencies would
    raise.
    """
    body = '''
from app.assistant.engine import AssistantEngine
from app.assistant.provider import AssistantProvider, ProviderResponse, ToolCall


class _Tool:
    """Structural stand-in: the same interface AssistantTool exposes.

    Deliberately NOT app.assistant.tools.AssistantTool -- importing that class
    is exactly what this test forbids.
    """
    name = "probe"
    is_action = False
    wants_context = False

    def spec(self):
        return {"name": "probe", "description": "d",
                "input_schema": {"type": "object", "properties": {}}}

    @staticmethod
    def executor(path, params):
        return {"measured": 42}


class _Scripted(AssistantProvider):
    name, configured = "scripted", True

    def __init__(self):
        self.n = 0

    def complete(self, system, history, tools):
        self.n += 1
        if self.n == 1:
            return ProviderResponse(tool_calls=[ToolCall("c1", "probe", {})])
        return ProviderResponse(final_text="ok")


engine = AssistantEngine(_Scripted(), system_prompt="p", tools={"probe": _Tool()})
result = engine.run("go", None)
assert result.answer == "ok", result.answer
assert result.tool_calls[0].result == {"measured": 42}
assert result.rounds == 2
assert "app.assistant.tools" not in sys.modules
assert "numpy" not in sys.modules
assert "astropy" not in sys.modules
'''
    proc = _run_guarded(body, denied=("numpy", "astropy", "app.assistant.tools",
                                      "app.fits_dataset", "app.compute"))
    assert proc.returncode == 0, (
        f"guarded subprocess failed\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}")
    assert "GUARDED-OK" in proc.stdout


# ── 5/5 — lazy app/assistant/__init__.py ─────────────────────────────────────

# The public surface as it was BEFORE the lazification, captured from the
# pre-change package. Hard-coded on purpose: a test that recomputes it from the
# current module could not detect a change.
_SURFACE_BEFORE_BE02 = [
    "AssistantEngine", "AssistantProvider", "AssistantResult", "AssistantTool",
    "TOOLS", "configure", "engine", "provider", "provider_from_env",
    "public_config", "tools",
]


def test_public_surface_is_identical_after_lazification():
    import app.assistant as pkg

    assert sorted(n for n in dir(pkg) if not n.startswith("_")) == \
        _SURFACE_BEFORE_BE02
    assert sorted(pkg.__all__) == _SURFACE_BEFORE_BE02


def test_star_import_surface_is_identical_after_lazification():
    namespace: dict = {}
    exec("from app.assistant import *", namespace)  # noqa: S102 - that is the test
    assert sorted(n for n in namespace if not n.startswith("_")) == \
        _SURFACE_BEFORE_BE02


def test_every_public_name_still_resolves_to_the_right_object():
    import app.assistant as pkg
    from app.assistant import engine as engine_mod
    from app.assistant import provider as provider_mod
    from app.assistant import tools as tools_mod

    assert pkg.AssistantEngine is engine_mod.AssistantEngine
    assert pkg.AssistantResult is engine_mod.AssistantResult
    assert pkg.AssistantProvider is provider_mod.AssistantProvider
    assert pkg.provider_from_env is provider_mod.provider_from_env
    assert pkg.configure is provider_mod.configure
    assert pkg.public_config is provider_mod.public_config
    assert pkg.TOOLS is tools_mod.TOOLS
    assert pkg.AssistantTool is tools_mod.AssistantTool
    assert pkg.engine is engine_mod
    assert pkg.provider is provider_mod
    assert pkg.tools is tools_mod


def test_unknown_attribute_still_raises_attribute_error():
    import app.assistant as pkg

    with pytest.raises(AttributeError) as exc:
        pkg.no_such_name
    assert "no_such_name" in str(exc.value)


def test_provider_imports_with_no_third_party_dependencies():
    """BE-01 Finding 2, closed: a minimal environment can import provider.

    Fresh interpreter with numpy, astropy, fastapi and pydantic all made fatal
    to import — and the credential-free path is exercised, not just the import.
    """
    body = '''
from app.assistant import provider

cfg = provider.default_config()
for key in ("anthropic_key", "openai_key", "base_url", "base_url_key"):
    cfg[key] = None
built = provider.provider_from_env(cfg)
assert built.name == "null", built.name
assert built.configured is False
answer = built.complete("s", [{"role": "user", "text": "hi"}], [])
assert "not configured" in answer.final_text
assert "app.assistant.tools" not in sys.modules
assert "app.assistant.engine" not in sys.modules
assert "numpy" not in sys.modules
'''
    proc = _run_guarded(body, denied=("numpy", "astropy", "fastapi", "pydantic"))
    assert proc.returncode == 0, (
        f"guarded subprocess failed\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}")
    assert "GUARDED-OK" in proc.stdout


def test_importing_the_package_alone_pulls_neither_engine_nor_tools():
    """The lazy __init__ must not eagerly resolve anything."""
    body = '''
import app.assistant
assert "app.assistant.engine" not in sys.modules
assert "app.assistant.tools" not in sys.modules
assert "app.assistant.provider" not in sys.modules
# ...and the names are still reachable on demand.
assert app.assistant.AssistantProvider.__name__ == "AssistantProvider"
assert "app.assistant.provider" in sys.modules
'''
    proc = _run_guarded(body, denied=("numpy", "astropy", "fastapi", "pydantic"))
    assert proc.returncode == 0, (
        f"guarded subprocess failed\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}")
    assert "GUARDED-OK" in proc.stdout
