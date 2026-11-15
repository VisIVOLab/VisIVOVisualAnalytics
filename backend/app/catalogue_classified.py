"""A catalogue classified in the headset, as a VOTable (issue #279, epic #278).

The VR page lists the sources of a catalogue placed over a cube
(app/cube_catalogue.py), and the user flags each one (real, doubtful,
spurious by default: a configurable set) with a note picked from presets, and
adds sources found by eye that the catalogue does not hold. This module
writes that work as a table:

* a catalogue open in the session (a SoFiA list, a CSV of RA/Dec): every
  original column and every row, in the file's order, plus the classification
  columns; the sources found by eye are appended as rows whose original
  columns are empty (null in the VOTable) except the sky columns and the name;
* an overlay with no table behind it (VLKB compact sources, a cross-match):
  the sources the page placed (name, sky position in their own frame), plus
  the classification columns, and the sources found by eye.

The classification columns: ``<flag_column>`` (the flag, '' when the source
was not classified), ``vr_note``, ``vr_origin`` (catalogue, overlay or user),
``vr_x``/``vr_y``/``vr_z`` (0-based pixels of the cube, NaN when not placed;
z NaN for a source placed on the sky only) and ``vr_velocity_kms`` (the cube's
velocity at vr_z, as the page shows it, NaN when unknown).
"""
from __future__ import annotations

import math
from typing import Any

import numpy as np

#: The classification columns other than the flag's own.
EXTRA_COLUMNS = ("vr_note", "vr_origin", "vr_x", "vr_y", "vr_z", "vr_velocity_kms")


def _num(v: Any) -> float:
    try:
        f = float(v)
    except (TypeError, ValueError):
        return math.nan
    return f if math.isfinite(f) else math.nan


def classified_table(sources: list[dict], flag_column: str = "vr_flag", frame=None,
                     coordinates: dict | None = None, label_field: str | None = None,
                     source_frame: str = "icrs"):
    """The classified table (an astropy Table, masked).

    ``sources``: the page's rows, each a dict with ``origin`` (catalogue,
    overlay or user), ``row`` (the catalogue's row, for origin catalogue),
    ``label``, ``lon``/``lat`` (degrees, in the source's own ``frame``, else
    ``source_frame``), ``x``/``y``/
    ``z``, ``velocity_kms``, ``flag``, ``note``. ``frame``: the catalogue's
    pandas frame, or None for an overlay with no table behind it;
    ``coordinates``: its coordinate fields (``{"system": "sky", "ra", "dec"}``
    or ``{"system": "galactic", "glon", "glat"}``); ``label_field``: its name
    column. A row number out of the frame is refused (ValueError).
    """
    from astropy.table import MaskedColumn, Table   # noqa: PLC0415
    if flag_column in EXTRA_COLUMNS:
        raise ValueError(f"{flag_column!r} is the name of another classification column.")
    by_row: dict[int, dict] = {}
    rest: list[dict] = []
    for s in sources:
        if s.get("origin") == "catalogue" and frame is not None and s.get("row") is not None:
            r = int(s["row"])
            if not 0 <= r < len(frame):
                raise ValueError(f"Row {r} is not in the catalogue ({len(frame)} rows).")
            by_row[r] = s
        else:
            rest.append(s)
    if frame is not None:
        if flag_column in frame.columns or any(c in frame.columns for c in EXTRA_COLUMNS):
            raise ValueError(f"The catalogue already has a column named {flag_column!r} or vr_*: "
                             "give another flag column name.")
        base = Table.from_pandas(frame)
        n0 = len(base)
        system = (coordinates or {}).get("system")
        lon_col = (coordinates or {}).get("ra" if system == "sky" else "glon")
        lat_col = (coordinates or {}).get("dec" if system == "sky" else "glat")
        want = "icrs" if system == "sky" else "galactic" if system == "galactic" else None
        same_frame = [want is not None and (s.get("frame") or source_frame) == want for s in rest]
        cols = []
        for name in base.colnames:
            c = base[name]
            data = list(c) + [None] * len(rest)
            mask = list(np.ma.getmaskarray(c)) + [True] * len(rest)
            if name in (lon_col, lat_col):
                for k, s in enumerate(rest):
                    if not same_frame[k]:
                        continue
                    v = _num(s.get("lon" if name == lon_col else "lat"))
                    if math.isfinite(v):
                        data[n0 + k], mask[n0 + k] = v, False
            elif label_field and name == label_field:
                for k, s in enumerate(rest):
                    data[n0 + k], mask[n0 + k] = str(s.get("label", "")), False
            filled = [d if not m else (c.info.dtype.type(0) if c.info.dtype.kind in "iufb"
                                       else "") for d, m in zip(data, mask)]
            dtype = c.info.dtype if c.info.dtype.kind in "iufb" else str
            cols.append(MaskedColumn(np.array(filled, dtype=dtype), name=name, mask=mask,
                                     unit=c.info.unit, description=c.info.description))
        out = Table(cols, masked=True)
        rows = [by_row.get(i) for i in range(n0)] + rest
        origins = ["catalogue"] * n0 + [str(s.get("origin") or "user") for s in rest]
    else:
        lon_name, lat_name = ("ra", "dec") if source_frame == "icrs" else ("glon", "glat")
        # A source given in the other frame (a found source's ICRS position
        # over a Galactic overlay) leaves these two NaN; vr_x/y/z place it.
        mine = [(s.get("frame") or source_frame) == source_frame for s in rest]
        out = Table(masked=True)
        out["name"] = [str(s.get("label", "")) for s in rest]
        out[lon_name] = np.array([_num(s.get("lon")) if m else math.nan for s, m in zip(rest, mine)], dtype=float)
        out[lat_name] = np.array([_num(s.get("lat")) if m else math.nan for s, m in zip(rest, mine)], dtype=float)
        out[lon_name].unit = out[lat_name].unit = "deg"
        rows = list(rest)
        origins = [str(s.get("origin") or "overlay") for s in rest]
    get = lambda key: [("" if s is None else str(s.get(key) or "")) for s in rows]  # noqa: E731
    numb = lambda key: np.array([math.nan if s is None else _num(s.get(key))  # noqa: E731
                                 for s in rows], dtype=float)
    out[flag_column] = get("flag")
    out["vr_note"] = get("note")
    out["vr_origin"] = origins
    for k in ("x", "y", "z"):
        out[f"vr_{k}"] = numb(k)
    out["vr_velocity_kms"] = numb("velocity_kms")
    out["vr_velocity_kms"].unit = "km/s"
    out[flag_column].description = "Classification made in the VR headset ('' not classified)"
    out["vr_note"].description = "Note picked in the VR headset"
    out["vr_origin"].description = "catalogue: a row of the catalogue; user: found by eye in VR"
    for k in ("x", "y", "z"):
        out[f"vr_{k}"].description = f"0-based {k} pixel of the cube (NaN: not placed)"
    out["vr_velocity_kms"].description = "The cube's velocity at vr_z, as shown in VR"
    return out


def write_votable(table, path: str, meta: dict[str, Any]) -> None:
    """``table`` as a VOTable at ``path`` (overwriting the reserved name)."""
    table.meta.update({k: str(v) for k, v in meta.items()})
    table.write(path, format="votable", overwrite=True)
