"""VisIVO scientific copilot (R-22).

A provenance-bound, tool-calling assistant that answers natural-language
questions about a loaded dataset by driving the SAME validated compute workers
the rest of the backend uses — never by inventing numbers in free text. Every
quantitative claim it makes is backed by a recorded tool call with its
parameters and units, and each answer emits an equivalent reproducibility
script so the result can be regenerated headless.

The LLM provider is pluggable (see ``provider.py``): with no API key configured
the assistant still exposes its tool catalogue and runs deterministic tool calls,
but cannot reason in natural language. Wiring a key/provider is a deployment step.
"""
from .engine import AssistantEngine, AssistantResult  # noqa: F401
from .provider import (  # noqa: F401
    AssistantProvider,
    provider_from_env,
    configure,
    public_config,
)
from .tools import TOOLS, AssistantTool  # noqa: F401
