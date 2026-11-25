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

A snapshot written as several files lives in a folder of its own, numbered
too (Gadget's ``snapdir_010/snap_010.0``, issue #31): when the folder holds
no other member and its name carries the same number, the series runs over
the sibling folders, each member the same file name with that folder's
number (``snapdir_011/snap_011.0``).

A file of a legacy-importer descriptor (``rawPointsDesc``, ``rawGridsDesc``,
``flyDesc``, issue #551) has its series written down: the descriptor lists
the snapshots and their time tags, in order, whatever their names
(app/raw_descriptor.py ``series``); the members are references to the files
of the list and ``numbers`` are their time tags.
"""
from __future__ import annotations

import os
import re

#: At most this many entries of a directory are looked at (only the names of
#: the series are kept).
MAX_LISTED = 1_000_000
#: At most this many members are returned: those nearest the open one by number.
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


def snapshot_series(path: str, allowed=None) -> dict:
    """The series of the file at ``path``: ``paths`` (absolute, in order),
    ``numbers``, ``index`` (of ``path``) and ``count``; a series of one when
    the directory cannot be listed or the name has no number. ``allowed``
    (path -> bool): a member it refuses is left out, e.g. a symbolic link
    that resolves outside the backend's data roots. Of a longer series, the
    MAX_MEMBERS nearest the open file by number."""
    from . import raw_descriptor  # noqa: PLC0415
    listed = raw_descriptor.series(path, allowed)
    if listed is not None:
        return listed
    path = os.path.abspath(path)
    folder, name = os.path.split(path)
    pat = pattern(name)
    names = []
    if pat is not None:
        try:
            with os.scandir(folder) as it:
                for k, e in enumerate(it):
                    if k >= MAX_LISTED:
                        break
                    # Only the series' names are kept, whatever the listing's order.
                    if pat.match(e.name) and e.is_file():
                        names.append(e.name)
        except OSError:
            names = []
    if allowed is not None:
        names = [n for n in names if n == name or allowed(os.path.join(folder, n))]
    found = members(name, names)
    if len(found) == 1 and pat is not None:
        folders = _folder_series(path, allowed)
        if folders is not None:
            return folders
    return _window(folder, name, found)


def _number(name: str) -> tuple[str, str, str] | None:
    """``(before, digits, after)`` of a name's stem's last digit run."""
    stem, dot, rest = name.partition(".")
    m = None
    for m in re.finditer(r"\d+", stem):
        pass
    if m is None:
        return None
    return stem[:m.start()], m.group(0), stem[m.end():] + dot + rest


def _folder_series(path: str, allowed=None) -> dict | None:
    """The series of a snapshot in a numbered folder of its own
    (``snapdir_010/snap_010.0``): the sibling folders whose names differ
    only in that number, each with the same file renumbered; None when the
    folder's number is not the file's or no sibling has the file."""
    folder, name = os.path.split(path)
    parent, dirname = os.path.split(folder)
    file_parts, dir_parts = _number(name), _number(dirname)
    if file_parts is None or dir_parts is None or int(file_parts[1]) != int(dir_parts[1]):
        return None
    dir_pat = pattern(dirname)
    dirs = []
    try:
        with os.scandir(parent) as it:
            for k, e in enumerate(it):
                if k >= MAX_LISTED:
                    break
                if dir_pat.match(e.name) and e.is_dir():
                    dirs.append(e.name)
    except OSError:
        return None
    found = []
    for d, num in ((d, int(dir_pat.match(d).group(1))) for d in dirs):
        digits = str(num).zfill(len(file_parts[1]))
        member = os.path.join(parent, d, file_parts[0] + digits + file_parts[2])
        if d == dirname:
            found.append((num, path))
        elif os.path.isfile(member) and (allowed is None or allowed(member)):
            found.append((num, member))
    if len(found) < 2:
        return None
    found.sort()
    index = next(i for i, (_, m) in enumerate(found) if m == path)
    if len(found) > MAX_MEMBERS:
        lo = max(0, min(index - MAX_MEMBERS // 2, len(found) - MAX_MEMBERS))
        found = found[lo:lo + MAX_MEMBERS]
        index -= lo
    return {"paths": [m for _, m in found], "numbers": [k for k, _ in found],
            "index": index, "count": len(found)}


def _window(folder: str, name: str, found: list) -> dict:
    index = next(i for i, (_, n) in enumerate(found) if n == name)
    if len(found) > MAX_MEMBERS:
        lo = max(0, min(index - MAX_MEMBERS // 2, len(found) - MAX_MEMBERS))
        found = found[lo:lo + MAX_MEMBERS]
        index -= lo
    return {"paths": [os.path.join(folder, n) for _, n in found], "numbers": [k for k, _ in found],
            "index": index, "count": len(found)}
