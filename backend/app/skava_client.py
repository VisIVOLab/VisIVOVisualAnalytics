"""Helpers for the ``/v1/datasets/open_skava`` route.

The desktop client has already queried SKAVA before talking to us — it
already knows the access URL for the obs_id it wants opened. So this
module is intentionally minimal: it just parses the access URL the
client supplied and turns it into a local filesystem path the backend
can ``fits.open()``.

No HTTP, no httpx, no SKAVA round-trip from the backend. The trust
model is: an authenticated client passes (obs_id, access_url), and the
backend only enforces what schemes / paths it is willing to honour.

Path jail: when ``VISIVO_DATA_ROOT`` is set, the resolved local path
MUST be inside that directory. Without it any file:// URL the client
chooses is opened — fine for a single-tenant SRC where the backend
already runs unprivileged with restricted FS access, dangerous in a
shared deployment. Operators are expected to set the jail in
production.
"""
from __future__ import annotations

import logging
import os
from pathlib import Path
from urllib.parse import unquote, urlparse

logger = logging.getLogger("visivo.skava")


class SkavaResolveError(RuntimeError):
    """Raised when an access URL cannot be turned into a local path."""


def _data_roots() -> list[Path]:
    """The directories a SKAVA access URL may point into.

    ``VISIVO_DATA_ROOT`` (one directory) is SKAVA's historical name; the rest of
    the backend confines paths with ``VISIVO_DATA_ROOTS`` (os.pathsep-separated).
    Either is honoured, so a node needs to declare its data only once.
    """
    roots = []
    single = (os.environ.get("VISIVO_DATA_ROOT") or "").strip()
    if single:
        roots.append(Path(single).expanduser().resolve())
    for part in (os.environ.get("VISIVO_DATA_ROOTS") or "").split(os.pathsep):
        if part.strip():
            roots.append(Path(part.strip()).expanduser().resolve())
    return roots


def access_url_to_local_path(access_url: str) -> str:
    """Convert a SKAVA access_url into a filesystem path the backend can open.

    Supported schemes:
    * ``file://``  — strip the prefix, return the absolute path. This is
                     the canonical "compute next to data" case: the file
                     lives on the same node as the backend.
    * ``http(s)://`` — REJECTED at this layer. The desktop already has
                     ``/v1/datasets/open_url`` for that flow; mixing the
                     download path into ``open_skava`` would defeat the
                     point of compute-next-to-data.

    When ``VISIVO_DATA_ROOT`` or ``VISIVO_DATA_ROOTS`` is configured, the
    resolved path is rejected if it escapes those directories (defence
    against path traversal in client-supplied URLs).
    """
    if not access_url:
        raise SkavaResolveError("access_url is empty.")
    if not access_url.startswith("file://"):
        raise SkavaResolveError(
            f"access_url scheme is not supported for compute-next-to-data: "
            f"{access_url!r}. Use /v1/datasets/open_url for HTTP downloads."
        )
    parsed = urlparse(access_url)
    if parsed.netloc and parsed.netloc not in ("", "localhost"):
        raise SkavaResolveError(
            f"file:// URL points at a non-local host: {access_url!r}")
    resolved = Path(unquote(parsed.path)).resolve()

    roots = _data_roots()
    if roots and not any(resolved == root or root in resolved.parents for root in roots):
        raise SkavaResolveError(
            f"Resolved path {resolved} is outside the node's data roots "
            f"({', '.join(str(r) for r in roots)}).")

    return str(resolved)
