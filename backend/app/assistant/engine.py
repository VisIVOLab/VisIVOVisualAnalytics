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
- Always establish units and frame first (call dataset_metadata) before interpreting values.
- Surface caveats explicitly: moment 2 is the velocity DISPERSION σ by default (not variance); fluxes are only in Jy when beam-corrected; a frequency axis needs a rest frequency for km/s; report uncertainties when available.
- Be concise. When you state a number, name its unit. If the data can't answer, say so.
- Prefer calling a tool over guessing. Do not fabricate file paths, beams, or redshifts."""


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
            dataset_id: str | None = None) -> AssistantResult:
        specs = tool_specs()
        context = ""
        if dataset_id:
            context = f"\n\n[A dataset is loaded: id={dataset_id}. Tools operate on it.]"
        messages: list[dict[str, Any]] = [
            {"role": "user", "content": question + context},
        ]
        invocations: list[ToolInvocation] = []
        rounds = 0
        final_text: str | None = None

        while rounds < _MAX_ROUNDS:
            rounds += 1
            resp: ProviderResponse = self.provider.complete(SYSTEM_PROMPT, messages, specs)
            if not resp.tool_calls:
                final_text = resp.final_text or "(no answer)"
                break
            # Execute each requested tool against the real workers.
            tool_result_blocks: list[dict[str, Any]] = []
            for call in resp.tool_calls:
                inv = self._execute(call, dataset_path)
                invocations.append(inv)
                block: dict[str, Any] = {
                    "type": "tool_result",
                    "tool_use_id": call.id,
                    "content": json.dumps({"error": inv.error} if inv.error else inv.result),
                }
                if inv.error:
                    block["is_error"] = True  # Anthropic tool-error signal
                tool_result_blocks.append(block)
            # Append the assistant's tool-use turn + our tool results, then loop.
            # We deliberately do NOT adopt any text emitted alongside a tool-use
            # turn as the final answer — only a turn with no tool calls ends the
            # loop — so an exhausted loop never returns a half-finished sentence.
            messages.append({"role": "assistant", "content": [
                {"type": "tool_use", "id": c.id, "name": c.name, "input": c.arguments}
                for c in resp.tool_calls
            ]})
            messages.append({"role": "user", "content": tool_result_blocks})
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
        )

    def _execute(self, call: ToolCall, dataset_path: str | None) -> ToolInvocation:
        tool = TOOLS.get(call.name)
        if tool is None:
            return ToolInvocation(call.name, call.arguments,
                                  error=f"unknown tool {call.name!r}")
        try:
            result = tool.executor(dataset_path, call.arguments)
            return ToolInvocation(call.name, call.arguments, result=result)
        except Exception as exc:  # noqa: BLE001 — surfaced to the model, not swallowed
            return ToolInvocation(call.name, call.arguments, error=str(exc))
