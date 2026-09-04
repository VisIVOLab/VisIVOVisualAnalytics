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

Attribute access is LAZY (PEP 562). ``provider.py`` imports nothing third-party,
but ``tools.py`` needs numpy and (via ``..fits_dataset``) astropy, so eagerly
re-exporting everything here forced ~119 MB of scientific dependencies onto any
import of this package — including ``from app.assistant import provider``, which
needs none of them. Resolving each name on first use keeps the public surface
identical while letting a caller import only what it actually uses.

The surface is unchanged: every name below is importable exactly as before, is
listed by ``dir()``, and is exported by ``from app.assistant import *``.
"""
import importlib as _importlib

# Public name -> the submodule that defines it.
_LAZY_ATTRS = {
    "AssistantEngine": ".engine",
    "AssistantResult": ".engine",
    "AssistantProvider": ".provider",
    "provider_from_env": ".provider",
    "configure": ".provider",
    "public_config": ".provider",
    "TOOLS": ".tools",
    "AssistantTool": ".tools",
}

# Submodules that the previous eager imports bound as package attributes. They
# stay part of the surface: ``app.assistant.tools`` resolved without a prior
# ``import app.assistant.tools`` before this change, and still does.
_LAZY_SUBMODULES = ("engine", "provider", "tools")

# Explicit, and equal to what ``import *`` exported when the re-exports were
# eager — so star-import behaviour is unchanged too, not just attribute access.
__all__ = sorted(set(_LAZY_ATTRS) | set(_LAZY_SUBMODULES))


def __getattr__(name: str):
    """Resolve a public name on first access, then cache it in the module dict."""
    if name in _LAZY_SUBMODULES:
        value = _importlib.import_module(f".{name}", __name__)
    elif name in _LAZY_ATTRS:
        value = getattr(_importlib.import_module(_LAZY_ATTRS[name], __name__), name)
    else:
        raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
    globals()[name] = value   # subsequent lookups skip __getattr__ entirely
    return value


def __dir__() -> list[str]:
    return sorted(set(__all__) | set(globals()))
