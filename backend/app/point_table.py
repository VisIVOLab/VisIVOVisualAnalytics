"""A table of points as the backend draws it (issues #83, #101): a VBT point
table, read in place, or a catalogue file (several of one format
concatenated). Shared by the point render session and the ray-traced
particle render, so both read a dataset the same way.

``dataset_path`` is what the render route hands a point session: a VBT
header's path, or a JSON catalogue spec ``{"paths", "format",
"coordinates"}``.
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import pandas as pd


@dataclass
class PointTable:
    names: list[str]
    columns: dict[str, np.ndarray]      # numeric columns only
    num_rows: int
    axes: tuple[str, str, str]          # the default X/Y/Z columns
    frame: pd.DataFrame | None = None   # a catalogue's table, for its positions
    sky: tuple[str, str] | None = None  # (ra, dec) columns of a sky catalogue
    sky_settings: dict = field(default_factory=dict)

    def set_sky_positions(self, frame_name: str = "fk5", distance_mode: str = "comoving",
                          cosmology: str = "Planck18") -> None:
        """A sky catalogue's scene positions, as the desktop computes them
        (app/catalogue_positions.py), in the columns ``@x``, ``@y``, ``@z``."""
        from .catalogue_positions import sky_positions   # noqa: PLC0415
        if self.sky is None:
            raise ValueError("Not a sky catalogue: its positions are columns.")
        ra, dec = self.sky
        xyz = sky_positions(self.frame, ra, dec, frame_name=frame_name,
                            distance_mode=distance_mode, cosmology=cosmology)
        for i, axis in enumerate(("@x", "@y", "@z")):
            self.columns[axis] = xyz[:, i]
        self.sky_settings = {"frame": frame_name, "distance_mode": distance_mode,
                             "cosmology": cosmology}


def default_axes(names: list[str]) -> tuple[str, str, str]:
    """X/Y/Z by name when the table has them, else its first three columns —
    the desktop viewer's choice."""
    upper = {n.upper(): n for n in names}
    picks = [upper.get(a) for a in ("X", "Y", "Z")]
    if all(picks):
        return picks[0], picks[1], picks[2]
    first = (names + names[:1] * 3)[:3]
    return first[0], first[1], first[2]


def open_point_table(dataset_path: str) -> PointTable:
    if str(dataset_path).startswith("{"):
        return _open_catalogue(json.loads(dataset_path))
    from .volume_source import parse_vbt_header, split_order   # noqa: PLC0415
    # The header's path (the VBT route registers it); its binary beside it.
    # It may carry the byte order the dataset was opened in (issue #165).
    head, order = split_order(dataset_path)
    if not head.endswith(".head"):
        head += ".head"
    header = parse_vbt_header(head, order or None)
    if header["kind"] != "point":
        raise ValueError("Point rendering needs a VBT point table.")
    binary = head[: -len(".head")]
    table = np.memmap(binary, dtype=np.dtype(header["np_endian_code"]), mode="r",
                      shape=(header["field_count"], header["num_rows"]))
    names = list(header["field_names"])
    return PointTable(names, {n: table[i] for i, n in enumerate(names)},
                      int(header["num_rows"]), default_axes(names))


def _open_catalogue(spec: dict) -> PointTable:
    """A catalogue file (CSV, VOTable, FITS table, IPAC, .speck), or several of
    one format concatenated; its numeric columns are drawn. A sky catalogue's
    positions are computed in the columns ``@x``, ``@y``, ``@z``."""
    from .dependencies import _catalogue_load_dataframe   # noqa: PLC0415
    paths = [Path(p) for p in spec.get("paths") or []]
    if not paths:
        raise ValueError("No catalogue file to draw.")
    fmt = str(spec.get("format", "csv"))
    frames = [_catalogue_load_dataframe(p, fmt) for p in paths]
    frame = frames[0] if len(frames) == 1 else pd.concat(frames, ignore_index=True)
    names = [str(c) for c in frame.columns]
    columns = {}
    for name in names:
        if pd.api.types.is_numeric_dtype(frame[name]) and not pd.api.types.is_bool_dtype(frame[name]):
            columns[name] = frame[name].to_numpy(dtype=float, na_value=np.nan)
    coords = spec.get("coordinates") or {}
    table = PointTable(names, columns, len(frame), default_axes(names), frame=frame)
    if coords.get("system") == "cartesian":
        table.axes = (coords["x"], coords["y"], coords["z"])
    else:
        table.sky = (str(coords.get("ra", "")), str(coords.get("dec", "")))
        table.set_sky_positions()
        table.axes = ("@x", "@y", "@z")
    return table


def filter_mask(columns, filters: list[dict], rows: np.ndarray) -> np.ndarray:
    """Rows (of ``rows``) passing every numeric filter ``{"field", "op", "value"}``."""
    from .render_session.points import _filter_mask   # noqa: PLC0415
    return _filter_mask(columns, filters, rows)
