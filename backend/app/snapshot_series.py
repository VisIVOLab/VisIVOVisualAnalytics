"""A simulation's snapshots as a series of files (issue #259).

There is no time-series data model in the app: a simulation's outputs are
separate files, one per snapshot, named by a number (``snap_000.bin``,
``snap_001.bin``, ...; ``output_0042.csv``). The minimum the headset needs to
step through them is that series: the files beside an open one whose names
differ from it only in that number, in the order of the number. Each step
opens the next file as any table is opened; nothing is kept between them.

The number is the last run of digits in the name's stem (the name up to its
first dot), so an extension's digits (``.hdf5``) never count; a name whose
stem has no digits is a series of one.
"""
from __future__ import annotations

import os
import re

#: At most this many names are read from a directory (a series longer than
#: that is cut to the files nearest the open one in the listing's order).
MAX_LISTED = 100_000
#: At most this many members are returned.
MAX_MEMBERS = 10_000


def pattern(name: str) -> re.Pattern | None:
    """The regular expression of ``name``'s series (its stem's last digit
    run as a group), or None when its stem has no digits."""
    stem, dot, rest = name.partition(".")
    m = None
    for m in re.finditer(r"\d+", stem):
        pass
    if m is None:
        return None
    return re.compile(re.escape(stem[:m.start()]) + r"(\d+)" + re.escape(stem[m.end():] + dot + rest) + r"\Z")


def members(name: str, names) -> list[tuple[int, str]]:
    """``(number, name)`` of the names in ``names`` that belong to ``name``'s
    series (``name`` itself included when listed), by number then name."""
    pat = pattern(name)
    if pat is None:
        return [(0, name)]
    out = []
    for n in names:
        m = pat.match(n)
        if m:
            out.append((int(m.group(1)), n))
    if not any(n == name for _, n in out):
        m = pat.match(name)
        out.append((int(m.group(1)) if m else 0, name))
    out.sort()
    return out


def snapshot_series(path: str) -> dict:
    """The series of the file at ``path``: ``paths`` (absolute, in order),
    ``numbers``, ``index`` (of ``path``) and ``count``; a series of one when
    the directory cannot be listed or the name has no number."""
    path = os.path.abspath(path)
    folder, name = os.path.split(path)
    names = []
    try:
        with os.scandir(folder) as it:
            for k, e in enumerate(it):
                if k >= MAX_LISTED:
                    break
                if e.is_file():
                    names.append(e.name)
    except OSError:
        names = [name]
    found = members(name, names)
    index = next(i for i, (_, n) in enumerate(found) if n == name)
    if len(found) > MAX_MEMBERS:
        lo = max(0, min(index - MAX_MEMBERS // 2, len(found) - MAX_MEMBERS))
        found = found[lo:lo + MAX_MEMBERS]
        index -= lo
    return {"paths": [os.path.join(folder, n) for _, n in found], "numbers": [k for k, _ in found],
            "index": index, "count": len(found)}
