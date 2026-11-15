"""The application's version, from ``app/VERSION`` (one line, ``X.Y.Z``).

That file is the single source: CMakeLists.txt reads it into
``project(VERSION ...)`` (so the desktop's About box and the package names),
and the backend reports it (FastAPI's ``version``, ``/v1/vr/info``, the node
status). It sits inside ``app/`` because every way the backend is shipped
copies that directory whole: the release packages, the Apptainer images, a
``--backend-src`` overlay on a cluster, a source checkout; a file at the
repository's root would be missing from most of them. The release workflow
refuses a tag that differs from it or from CITATION.cff.
"""
from __future__ import annotations

import re
from pathlib import Path

VERSION_FILE = Path(__file__).resolve().parent / "VERSION"
_VERSION = re.compile(r"^\d+\.\d+\.\d+$")
UNKNOWN = "0.0.0+unknown"


def read_version(path: Path = VERSION_FILE) -> str:
    """The version in ``path``; :data:`UNKNOWN` when the file is missing or
    is not ``X.Y.Z`` (a backend copied without it still starts)."""
    try:
        text = path.read_text(encoding="utf-8").strip()
    except OSError:
        return UNKNOWN
    return text if _VERSION.match(text) else UNKNOWN


__version__ = read_version()
