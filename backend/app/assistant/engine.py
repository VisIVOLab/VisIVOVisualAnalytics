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
from typing import Any

from .provider import AssistantProvider, ProviderResponse, ToolCall
from .tools import TOOLS, tool_specs

_MAX_ROUNDS = 6

SYSTEM_PROMPT = """You are the VisIVO scientific copilot for radio/astro data cubes and cosmological catalogues.

Hard rules:
- NEVER state a numeric result (flux, velocity, dispersion, distance, statistic) that did not come from a tool call. If a fact needs a number, call a tool.
- Establish the data's shape first: for a cube/image call dataset_metadata (units/frame/beam); for a point catalogue call catalogue_summary (columns/coordinate system) — before interpreting values.
- To MEASURE, call the right tool: noise_estimate for the cube's σ/RMS (and to justify a k×σ threshold); aperture_photometry for the flux at a sky position; source_gaussfit to characterise a source (peak, integrated flux, FWHM, PA); fit_line to extract a spectrum at a position and fit its line (peak/center/σ/FWHM/integrated flux); find_sources to run SoFiA-2 detection over the cube (heavy — only when asked to find sources); cross_match to report which catalogued sources (SIMBAD/2MASS/NVSS/FIRST) are already known in the field; angular_power_spectrum for a 2-D map's RELATIVE (un-calibrated) radial power spectrum — never call it a calibrated angular C(ℓ). These need WCS (RA/DEC in degrees) where a position is involved.
- Surface caveats explicitly: moment 2 is the velocity DISPERSION σ by default (not variance); fluxes are only in Jy when beam-corrected; a frequency axis needs a rest frequency for km/s; report uncertainties when available.
- Be concise. When you state a number, name its unit. If the data can't answer, say so.
- Prefer calling a tool over guessing. Do not fabricate file paths, beams, or redshifts.

You may also ACT on the visualisation and produce results: tools whose description
starts with "ACTION:" (set_colormap, set_scaling, set_color_field, set_threshold,
set_render_mode, set_blend_mode, goto_channel, reset_camera, compute_moment) change
the active view or add a product. Pick the one the active viewer supports (e.g.
set_scaling=log to reveal faint image structure; set_color_field to colour a point
cloud by a field). A directive the active viewer can't apply is safely ignored.
Use them proactively and autonomously to carry out what the user asks ("show this
source better", "highlight the brightest emission", "compute/show the moment-0", …)
— chain several if needed (e.g. inspect cube_statistics, then set a threshold at a
few × RMS and a MIP blend). When the user asks to compute or SEE a moment map, use
compute_moment (which displays it), not moment_map_summary (stats only). Display
changes are reversible (the user can Undo). After acting, briefly say what you did."""


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


def _repro_script(dataset_id: str | None, calls: list[ToolInvocation]) -> str:
    """A runnable Python sketch reproducing the tool calls headless."""
    lines = [
        "# Reproduces the copilot's analysis via the same backend workers.",
        "# Regenerate any number below deterministically — no LLM involved.",
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
    def __init__(self, provider: AssistantProvider) -> None:
        self.provider = provider

    def run(self, question: str, dataset_path: str | None,
            dataset_id: str | None = None,
            dataset_entry: dict[str, Any] | None = None) -> AssistantResult:
        specs = tool_specs()
        context = ""
        if dataset_id:
            kind = (dataset_entry or {}).get("kind")
            if kind == "catalogue":
                context = (f"\n\n[A point catalogue is loaded: id={dataset_id}. "
                           "Call catalogue_summary first; use cosmology_distance "
                           "for redshift → distance.]")
            elif kind == "vbt":
                context = (f"\n\n[A VBT point/volume table is loaded: id={dataset_id}. "
                           "Call vbt_summary first.]")
            else:
                context = f"\n\n[A dataset is loaded: id={dataset_id}. Tools operate on it.]"
        # NEUTRAL conversation history — provider-agnostic. Each provider
        # translates it to its own API shape (Anthropic tool_use/tool_result
        # blocks, OpenAI tool_calls/tool messages), so the engine works
        # unchanged across Claude / OpenAI / a self-hosted OpenAI-compatible LLM.
        history: list[dict[str, Any]] = [{"role": "user", "text": question + context}]
        invocations: list[ToolInvocation] = []
        actions: list[dict[str, Any]] = []
        rounds = 0
        final_text: str | None = None

        while rounds < _MAX_ROUNDS:
            rounds += 1
            resp: ProviderResponse = self.provider.complete(SYSTEM_PROMPT, history, specs)
            if not resp.tool_calls:
                final_text = resp.final_text or "(no answer)"
                break
            # Execute each requested tool against the real workers.
            results: list[dict[str, Any]] = []
            for call in resp.tool_calls:
                inv = self._execute(call, dataset_path, dataset_entry)
                invocations.append(inv)
                # Action tools emit a viewer directive rather than data; collect
                # it for the app to apply (the model still gets the queued ack).
                tool = TOOLS.get(call.name)
                if tool is not None and tool.is_action and inv.result \
                        and isinstance(inv.result.get("action"), dict):
                    actions.append(inv.result["action"])
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
                 dataset_entry: dict[str, Any] | None = None) -> ToolInvocation:
        tool = TOOLS.get(call.name)
        if tool is None:
            return ToolInvocation(call.name, call.arguments,
                                  error=f"unknown tool {call.name!r}")
        try:
            if tool.wants_context:
                result = tool.executor(dataset_path, call.arguments, dataset_entry)
            else:
                result = tool.executor(dataset_path, call.arguments)
            return ToolInvocation(call.name, call.arguments, result=result)
        except Exception as exc:  # noqa: BLE001 — surfaced to the model, not swallowed
            return ToolInvocation(call.name, call.arguments, error=str(exc))
