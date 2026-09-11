"""The copilot agent loop.

Runs a bounded tool-use conversation: the provider either asks for a tool call
(which we execute against the real workers and feed back) or returns a final
answer. Every tool call is recorded with its parameters, result and any units so
the answer is auditable, and an equivalent reproducibility script is emitted.

Grounding rule (enforced by the system prompt and surfaced via provenance): the
copilot must not state a quantitative result that did not come from a tool call.
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any, Callable

from .provider import AssistantProvider, ProviderResponse, ToolCall

if TYPE_CHECKING:   # pragma: no cover - typing only, never imported at runtime
    from collections.abc import Mapping

    from .tools import AssistantTool

_MAX_ROUNDS = 6

SYSTEM_PROMPT = """You are the VisIVO scientific copilot for radio/astro data cubes and cosmological catalogues.

Hard rules:
- NEVER state a numeric result (flux, velocity, dispersion, distance, statistic) that did not come from a tool call. If a fact needs a number, call a tool.
- Establish the data's shape first: for a cube/image call dataset_metadata (units/frame/beam); for a point catalogue call catalogue_summary (columns/coordinate system), before interpreting values.
- To MEASURE, call the right tool: noise_estimate for the cube's σ/RMS (and to justify a k×σ threshold); aperture_photometry for the flux at a sky position; source_gaussfit to characterise a source (peak, integrated flux, FWHM, PA); fit_line to extract a spectrum at a position and fit its line (peak/center/σ/FWHM/integrated flux); find_sources to run SoFiA-2 detection over the cube (heavy - only when asked to find sources); cross_match to report which catalogued sources (SIMBAD/2MASS/NVSS/FIRST) are already known in the field; angular_power_spectrum for a 2-D map's RELATIVE (un-calibrated) radial power spectrum; never call it a calibrated angular C(ℓ). These need WCS (RA/DEC in degrees) where a position is involved.
- Surface caveats explicitly: moment 2 is the velocity DISPERSION σ by default (not variance); fluxes are only in Jy when beam-corrected; a frequency axis needs a rest frequency for km/s; report uncertainties when available.
- Be concise. When you state a number, name its unit. If the data can't answer, say so.
- Prefer calling a tool over guessing. Do not fabricate file paths, beams, or redshifts.
- MULTIPLE views may be open. Tools act on the ACTIVE dataset by default; to correlate or act on ANOTHER view (e.g. a cube and a 2-D image of the same field), call list_datasets to get their ids and pass a tool's dataset_id argument. Actions are routed to the targeted view.

You may also ACT on the visualisation and produce results: tools whose description
starts with "ACTION:" (set_colormap, set_scaling, set_color_field, set_threshold,
set_render_mode, set_blend_mode, goto_channel, reset_camera, compute_moment,
show_sources, plot_spectrum) change the active view or add a product. To SHOW
detections in the viewer use show_sources (find_sources just reports numbers); to
PIN a spectrum panel at a position use plot_spectrum (fit_line just reports the fit). Pick the one the active viewer supports (e.g.
set_scaling=log to reveal faint image structure; set_color_field to colour a point
cloud by a field). A directive the active viewer can't apply is safely ignored.
Use them proactively and autonomously to carry out what the user asks ("show this
source better", "highlight the brightest emission", "compute/show the moment-0", …):
chain several if needed (e.g. inspect cube_statistics, then set a threshold at a
few × RMS and a MIP blend). When the user asks to compute or SEE a moment map, use
compute_moment (which displays it), not moment_map_summary (stats only). Display
changes are reversible (the user can Undo). After acting, briefly say what you did.
- Write like a colleague, not like a brochure: plain punctuation only. Use commas,
  parentheses or a plain hyphen; never an em-dash or en-dash."""


@dataclass
class ToolInvocation:
    name: str
    arguments: dict[str, Any]
    result: dict[str, Any] | None = None
    error: str | None = None


@dataclass
class AssistantResult:
    answer: str
    provider: str
    configured: bool
    # Whether ≥1 tool call is recorded for this answer. This is a factual trace
    # flag, NOT a guarantee that every number in the prose is tool-backed — we
    # cannot parse the prose to verify that (a proper verifier or answer-level
    # citations would be needed). Named honestly so the UI can't overclaim.
    tool_calls_recorded: bool
    tool_calls: list[ToolInvocation] = field(default_factory=list)
    reproducibility_script: str = ""
    rounds: int = 0
    # Viewer directives the copilot asked for (from ``is_action`` tools). The
    # desktop app applies them, in order, to the active viewer — display-only and
    # reversible. Empty for a pure question/answer turn.
    actions: list[dict[str, Any]] = field(default_factory=list)

    def to_dict(self) -> dict[str, Any]:
        return {
            "answer": self.answer,
            "provider": self.provider,
            "configured": self.configured,
            "tool_calls_recorded": self.tool_calls_recorded,
            "rounds": self.rounds,
            "tool_calls": [
                {"name": t.name, "arguments": t.arguments,
                 "result": t.result, "error": t.error}
                for t in self.tool_calls
            ],
            "reproducibility_script": self.reproducibility_script,
            "actions": self.actions,
        }


def _render_context(context: str | dict[str, Any]) -> str:
    """Render a caller-supplied per-request context into the prompt suffix.

    A string is used as given (whitespace-trimmed); a mapping is rendered as
    deterministic JSON. Either way exactly ONE blank-line separator is inserted
    ahead of it, so a caller can neither jam the context onto the question nor
    double the separator by supplying their own.

    An empty rendering means "no context at all" — which is how a caller asks
    for none, and is distinct from passing ``None`` (use the dataset-specific
    builder, i.e. today's behaviour).
    """
    if isinstance(context, str):
        rendered = context.strip()
    else:
        rendered = json.dumps(context, sort_keys=True, default=str)
    return f"\n\n{rendered}" if rendered else ""


def _repro_script(dataset_id: str | None, calls: list[ToolInvocation]) -> str:
    """A runnable Python sketch reproducing the tool calls headless."""
    lines = [
        "# Reproduces the copilot's analysis via the same backend workers.",
        "# Regenerate any number below deterministically (no LLM involved).",
        "from app.fits_dataset import ScientificFitsDataset",
    ]
    if dataset_id:
        lines.append(f'dataset_id = {dataset_id!r}  # resolve to a path server-side')
    for i, c in enumerate(calls):
        lines.append(f"# step {i + 1}: {c.name}({json.dumps(c.arguments)})")
    if not calls:
        lines.append("# (no tools were called)")
    return "\n".join(lines) + "\n"


class AssistantEngine:
    """The bounded tool-use loop.

    ``system_prompt`` and ``tools`` let a DIFFERENT application reuse this loop
    without the scientific catalogue or its prompt. Both default to today's
    behaviour when omitted, so existing callers are unaffected.
    """

    def __init__(self, provider: AssistantProvider,
                 system_prompt: str | None = None,
                 tools: "Mapping[str, AssistantTool] | None" = None) -> None:
        self.provider = provider
        # Omitted -> the module constant, i.e. byte-identical prompt behaviour.
        self.system_prompt = SYSTEM_PROMPT if system_prompt is None else system_prompt
        self._tools = tools

    @property
    def tools(self) -> "Mapping[str, AssistantTool]":
        """The active tool registry: injected, or the default catalogue.

        The default is resolved LAZILY. That is the point: an engine built with
        an injected registry never imports ``.tools``, so it never pulls that
        module's scientific edge (``numpy`` at tools.py:L14, ``astropy`` via
        ``..fits_dataset`` at tools.py:L16). See BE-01 Finding 3 — the
        dependency is inverted by injection, not severed by editing tools.py.

        An injected mapping must be name -> object satisfying the same
        ``AssistantTool`` interface (``spec()``, ``executor``, ``is_action``,
        ``wants_context``).
        """
        if self._tools is None:
            from .tools import TOOLS  # noqa: PLC0415 - lazy by design, see above

            self._tools = TOOLS
        return self._tools

    def run(self, question: str, dataset_path: str | None,
            dataset_id: str | None = None,
            dataset_entry: dict[str, Any] | None = None,
            prior_turns: list[dict[str, str]] | None = None,
            progress: "Callable[[str, str], None] | None" = None,
            datasets: dict[str, dict[str, Any]] | None = None,
            *,
            context: str | dict[str, Any] | None = None) -> AssistantResult:
        """Run the agent loop. ``progress(event, detail)`` — when given — is called
        as work happens ("tool", <tool name>) so a streaming caller can show which
        analysis is running; it must be cheap and non-blocking.

        ``datasets`` maps every OPEN dataset id → its session entry. ``dataset_id``
        is the ACTIVE view (the default target). A tool call may carry a
        ``dataset_id`` argument to operate on any other open view instead — so the
        copilot can correlate a cube with a 2-D image of the same field, etc.

        ``context`` (keyword-only) supplies the per-request context block
        directly. When it is ``None`` the dataset-specific builder below runs
        exactly as it always has. When it is given it REPLACES that builder --
        a string is used as-is, a mapping is rendered as deterministic JSON,
        and an empty rendering means no context block at all. Keyword-only so
        that no existing positional call site can shift."""
        def _emit(event: str, detail: str = "") -> None:
            if progress is not None:
                try:
                    progress(event, detail)
                except Exception:  # noqa: BLE001 — progress is best-effort telemetry
                    pass
        datasets = datasets or {}
        # Same list tool_specs() produces for the default registry, and the
        # injected registry's specs when one was supplied.
        specs = [t.spec() for t in self.tools.values()]
        if context is not None:
            context_text = _render_context(context)
        else:
            context_text = ""
            if dataset_id:
                kind = (dataset_entry or {}).get("kind")
                if kind == "catalogue":
                    context_text = (f"\n\n[Active dataset id={dataset_id} is a point catalogue. "
                                    "Call catalogue_summary first; use cosmology_distance "
                                    "for redshift → distance.]")
                elif kind == "vbt":
                    context_text = (f"\n\n[Active dataset id={dataset_id} is a VBT point/volume "
                                    "table. Call vbt_summary first.]")
                else:
                    context_text = f"\n\n[Active dataset id={dataset_id}. Tools operate on it by default.]"
            if len(datasets) > 1:
                context_text += ("\n\n[Several datasets are open. Call list_datasets to see them, "
                                 "and pass a tool's optional dataset_id argument to target a "
                                 "specific view (e.g. compare a cube with a 2-D image of the same "
                                 "field). Omit dataset_id to use the active view.]")
        # NEUTRAL conversation history — provider-agnostic. Each provider
        # translates it to its own API shape (Anthropic tool_use/tool_result
        # blocks, OpenAI tool_calls/tool messages), so the engine works
        # unchanged across Claude / OpenAI / a self-hosted OpenAI-compatible LLM.
        # Prior conversation turns (plain user/assistant text) let the copilot
        # answer FOLLOW-UP questions in context; the tool-call trace of earlier
        # turns is deliberately NOT replayed (only their final answers).
        history: list[dict[str, Any]] = []
        for t in (prior_turns or []):
            role = t.get("role")
            text = t.get("text", "")
            if role in ("user", "assistant") and text:
                history.append({"role": role, "text": text})
        history.append({"role": "user", "text": question + context_text})
        invocations: list[ToolInvocation] = []
        actions: list[dict[str, Any]] = []
        rounds = 0
        final_text: str | None = None

        while rounds < _MAX_ROUNDS:
            rounds += 1
            _emit("thinking")
            resp: ProviderResponse = self.provider.complete(
                self.system_prompt, history, specs)
            if not resp.tool_calls:
                final_text = resp.final_text or "(no answer)"
                break
            # Execute each requested tool against the real workers.
            results: list[dict[str, Any]] = []
            for call in resp.tool_calls:
                _emit("tool", call.name)   # surface which analysis is running
                # Resolve which OPEN dataset this call targets — the tool's optional
                # dataset_id argument selects another view; otherwise the active one.
                args = call.arguments if isinstance(call.arguments, dict) else {}
                requested = args.get("dataset_id")
                # Only an ABSENT / None / empty-string dataset_id falls back to the
                # active view. Any other explicit value must be a valid open id —
                # never silently retarget (that could act on the wrong window).
                if requested not in (None, ""):
                    if not isinstance(requested, str) or requested not in datasets:
                        inv = ToolInvocation(
                            call.name, args,
                            error=f"unknown dataset_id {requested!r}; call list_datasets "
                                  "for the open views.")
                        invocations.append(inv)
                        results.append({"id": call.id, "name": call.name,
                                        "content": json.dumps({"error": inv.error}),
                                        "is_error": True})
                        continue
                    target_id = requested
                    r_entry = datasets[requested]
                    r_path = r_entry.get("path")
                else:
                    target_id = dataset_id
                    r_entry = dataset_entry
                    r_path = dataset_path
                inv = self._execute(call, r_path, r_entry, datasets, dataset_id)
                invocations.append(inv)
                # Action tools emit a viewer directive rather than data; collect
                # it for the app to apply. The FULL directive (which may carry a
                # large payload, e.g. a pinned spectrum's samples) goes only into
                # ``actions`` for the desktop — the model and the provenance trace
                # get a compact ack, never the raw arrays.
                tool = self.tools.get(call.name)
                if tool is not None and tool.is_action and inv.result \
                        and isinstance(inv.result.get("action"), dict):
                    action = inv.result["action"]
                    # Tag the directive with the view it targets so the desktop
                    # routes it to the right window (default = the active view).
                    if target_id:
                        action.setdefault("target", target_id)
                    actions.append(action)
                    inv.result = {"status": inv.result.get("status", "queued"),
                                  "action": action.get("type"),
                                  "target": target_id}
                results.append({
                    "id": call.id,
                    "name": call.name,
                    "content": json.dumps({"error": inv.error} if inv.error else inv.result),
                    "is_error": bool(inv.error),
                })
            # Record the assistant's tool-request turn + our tool results, then
            # loop. We deliberately do NOT adopt any text emitted alongside a
            # tool-use turn as the final answer — only a turn with no tool calls
            # ends the loop — so an exhausted loop never returns a half sentence.
            history.append({"role": "assistant", "tool_calls": resp.tool_calls})
            history.append({"role": "tool", "results": results})
        else:
            final_text = final_text or (
                "Stopped after the maximum number of tool rounds without a final answer."
            )

        return AssistantResult(
            answer=final_text or "(no answer)",
            provider=self.provider.name,
            configured=self.provider.configured,
            tool_calls_recorded=bool(invocations),
            tool_calls=invocations,
            reproducibility_script=_repro_script(dataset_id, invocations),
            rounds=rounds,
            actions=actions,
        )

    def _execute(self, call: ToolCall, dataset_path: str | None,
                 dataset_entry: dict[str, Any] | None = None,
                 datasets: dict[str, dict[str, Any]] | None = None,
                 active_id: str | None = None) -> ToolInvocation:
        tool = self.tools.get(call.name)
        if tool is None:
            return ToolInvocation(call.name, call.arguments,
                                  error=f"unknown tool {call.name!r}")
        try:
            if tool.wants_context:
                # The resolved entry, plus the full open-dataset registry so a tool
                # like list_datasets can enumerate every view (keys prefixed with _
                # to avoid clashing with entry fields the summary tools read).
                context = dict(dataset_entry or {})
                context["_datasets"] = datasets or {}
                context["_active_id"] = active_id
                result = tool.executor(dataset_path, call.arguments, context)
            else:
                result = tool.executor(dataset_path, call.arguments)
            return ToolInvocation(call.name, call.arguments, result=result)
        except Exception as exc:  # noqa: BLE001 — surfaced to the model, not swallowed
            return ToolInvocation(call.name, call.arguments, error=str(exc))
