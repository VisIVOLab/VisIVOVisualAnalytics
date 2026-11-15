"""The path a file system stores for a file, letter case included.

On a case-insensitive file system (macOS by default, Windows) the same file
can be named with any mix of upper and lower case, and ``Path.resolve()`` on
macOS keeps the case it was given. A cache keyed on that text gives the same
file a second entry: a second brick pyramid of tens of gigabytes, a second
decompressed copy (issue #325). Keys are therefore made from the stored name.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path


def canonical_path(path: str | os.PathLike) -> Path:
    """``path`` resolved (symlinks followed) and spelled as the file system
    stores it. On macOS the stored name comes from ``fcntl(F_GETPATH)``; on
    Windows ``Path.resolve()`` already returns it; elsewhere names are case
    sensitive and the resolved path is the stored one. A path that cannot be
    opened is returned resolved, as before."""
    resolved = Path(path).resolve()
    if sys.platform != "darwin":
        return resolved
    import fcntl  # noqa: PLC0415

    getpath = getattr(fcntl, "F_GETPATH", None)
    if getpath is None:
        return resolved
    try:
        fd = os.open(resolved, os.O_RDONLY)
    except OSError:
        return resolved
    try:
        raw = fcntl.fcntl(fd, getpath, bytes(1024))
    except OSError:
        return resolved
    finally:
        os.close(fd)
    stored = os.fsdecode(raw.split(b"\0", 1)[0])
    return Path(stored) if stored else resolved
