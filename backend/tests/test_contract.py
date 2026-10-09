"""Client<->backend contract tests.

These lock down the two things that silently break the desktop client when they
drift, because both travel out-of-band of the normal typed JSON bodies:

  1. The set of REST routes the client calls (the "router refactor dropped
     endpoints" incident). We snapshot every (METHOD PATH) from the live
     OpenAPI schema and fail if it changes without the snapshot being updated.

  2. The ``X-*`` response-header names that binary endpoints use to ship
     metadata (including scientific units). We assert every ``X-*`` literal in
     the router sources is declared in ``app.response_headers`` and that the
     declared set matches the shared manifest the C++ side mirrors.

After an *intentional* API change, run::

    backend/.venv/bin/python backend/scripts/gen_contract.py

to refresh the snapshots, then review the diff.
"""
from __future__ import annotations

import json
import re
from pathlib import Path

import pytest

from app.main import app
from app.response_headers import KNOWN_RESPONSE_HEADERS

BACKEND = Path(__file__).resolve().parents[1]
CONTRACT = BACKEND / "tests" / "contract"
_REGEN = "backend/.venv/bin/python backend/scripts/gen_contract.py"


def _live_routes() -> list[str]:
    schema = app.openapi()
    return sorted(
        f"{method.upper()} {path}"
        for path, ops in schema.get("paths", {}).items()
        for method in ops
        if method.lower() in ("get", "post", "put", "patch", "delete")
    )


def test_openapi_route_snapshot() -> None:
    snapshot = json.loads((CONTRACT / "openapi_routes.json").read_text())["routes"]
    live = _live_routes()
    removed = sorted(set(snapshot) - set(live))
    added = sorted(set(live) - set(snapshot))
    assert not removed and not added, (
        "REST route contract drift — the desktop client pins these paths.\n"
        f"REMOVED (client calls will 404): {removed}\n"
        f"ADDED (not yet snapshotted): {added}\n"
        f"If intentional, regenerate: {_REGEN}"
    )


# Headers whose disappearance from the backend would silently break the client
# (buffer geometry + the scientific units/labels it displays). The client also
# treats these as expected — see visivoHeader() in BackendClient.cpp.
REQUIRED_HEADERS = frozenset({
    "X-Visivo-Width",
    "X-Visivo-Height",
    "X-Visivo-Depth",
    "X-Visivo-Bunit",
    "X-Visivo-Spectral-Axis-Unit",
    "X-Fwhm-Unit",
    "X-Ew-Unit",
})

# Single- and double-quoted string literals, scanned across the whole app tree
# (not just routers) so a header emitted from a helper module is still seen.
_HEADER_LITERAL = re.compile(r'''["'](X-[A-Za-z0-9-]+)["']''')
_IGNORE_HEADERS = {"X-Forwarded-For", "X-Forwarded-Proto", "X-Real-IP"}


def _emitted_header_literals() -> set[str]:
    scanned: set[str] = set()
    for src in (BACKEND / "app").rglob("*.py"):
        scanned |= set(_HEADER_LITERAL.findall(src.read_text()))
    return scanned - _IGNORE_HEADERS


def test_all_emitted_headers_are_declared() -> None:
    """Every X-* literal in the app sources must be a declared constant.

    Forces a rename/addition of a response header to also update
    ``response_headers.py`` (and hence the manifest + the C++ mirror), instead
    of silently diverging from what the client reads.
    """
    undeclared = sorted(_emitted_header_literals() - set(KNOWN_RESPONSE_HEADERS))
    assert not undeclared, (
        f"Response headers emitted in source but not declared in "
        f"app.response_headers: {undeclared}. Add them there and run {_REGEN}."
    )


def test_required_headers_still_emitted() -> None:
    """A required header must still be emitted *somewhere* in the app.

    Guards against this failure: a header can vanish from a response yet
    stay declared, so ``test_all_emitted_headers_are_declared`` alone would pass
    while the client silently reads it back as 0/"". If a required header is no
    longer emitted anywhere, that is either a real regression or an intentional
    removal that must also drop it from REQUIRED_HEADERS (and the client).
    """
    emitted = _emitted_header_literals()
    gone = sorted(REQUIRED_HEADERS - emitted)
    assert not gone, (
        f"Required response headers no longer emitted anywhere in backend/app: "
        f"{gone}. The desktop client depends on these; if the removal is "
        f"intentional, update REQUIRED_HEADERS and the client reads."
    )


def test_header_manifest_matches_declarations() -> None:
    manifest = json.loads((CONTRACT / "visivo_headers.json").read_text())["headers"]
    assert sorted(KNOWN_RESPONSE_HEADERS) == sorted(manifest), (
        "response_headers.KNOWN_RESPONSE_HEADERS is out of sync with the shared "
        f"manifest visivo_headers.json. Regenerate: {_REGEN}"
    )


def test_manifest_mirrored_to_docs() -> None:
    a = json.loads((CONTRACT / "visivo_headers.json").read_text())["headers"]
    b = json.loads((BACKEND.parent / "docs/contract/visivo_headers.json").read_text())["headers"]
    assert a == b, f"docs/contract mirror is stale. Regenerate: {_REGEN}"
