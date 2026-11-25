"""The leaf cells of an AMR hierarchy as a table of points (issue #550).

A "gas cells" component sits next to a run's particle families through the
same seam (app/point_table.py: ``Component`` of gadget_snapshot, of
ramses_output): the duck-typed columns, ``take``, units and physical scales
the point renderers, the catalogue tools, the copilot and VR read. It is built
on :class:`app.amr_source.AmrHierarchy`, so a RAMSES output and any other
producer (#554) get it with no code of their own.

Columns
-------
``x y z`` (the cell centre, box units), ``level``, ``dx`` (the cell side, box
units: ``2**-level``), the hydro fields as the producer names them, and
``mass``, the cell's gas mass ``density * dx**3`` in box-unit volumes (the
producer's :meth:`AmrHierarchy.scales` says what a unit of it is physically,
the box length cubed included). ``dx`` and ``mass`` are not stored: they
follow from ``level`` and ``density`` when read.

Rows are the leaves in the order :meth:`AmrHierarchy.leaf_blocks` yields them;
a column is read (one pass over the mesh) when first asked for and kept, so a
renderer that needs ``x y z dx`` and one field never reads the others. A
column costs 8 bytes a cell (``level``: 2).

Size
----
The component declares ``size_column = "dx"``: a cell is not a point but a
cube, and the renderers (routers/particle_render.py) draw it with that side
as its kernel's size instead of estimating one from its neighbours, with
:func:`app.particle_raytrace.cell_radius_factor` as the conversion.
"""
from __future__ import annotations

import threading

import numpy as np

from .amr_source import AmrHierarchy
from .gadget_snapshot import LazyColumns

#: The column whose values are the cells' sizes (see the module docstring).
SIZE_COLUMN = "dx"

_GEOMETRY = ("x", "y", "z")


class AmrCells:
    """The leaf cells of ``hierarchy`` as named columns, in box units.

    ``snapshot``: what states the run (``describe()``; a RAMSES output, whose
    ``info`` the projected maps read); ``reference``: the string the table is
    opened by (it keys what the user typed for its units); ``periodic``:
    whether positions wrap at the box."""

    size_column = SIZE_COLUMN

    def __init__(self, hierarchy: AmrHierarchy, snapshot=None, reference: str = "",
                 periodic: bool = True, name: str = "gas cells"):
        self.hierarchy = hierarchy
        self.output = snapshot if snapshot is not None else hierarchy
        self.reference = reference
        self.name = name
        self._periodic = periodic
        self.num_rows = hierarchy.n_leaf()
        self._fields = tuple(hierarchy.fields)
        self.names = ["x", "y", "z", "dx", "level", *self._fields]
        if "density" in self._fields:
            self.names.insert(5, "mass")
        self.derived: dict[str, str] = {"dx": "2^-level"}
        if "mass" in self.names:
            self.derived["mass"] = "density * dx^3"
        self._kept: dict[str, np.ndarray] = {}
        self._lock = threading.RLock()

    @property
    def snapshot(self):
        return self.output

    # ── reading ──

    def column_dtype(self, name: str) -> np.dtype:
        """The dtype of a column, without reading any value."""
        if name == "level":
            return np.dtype(np.int16)
        if name in self.names:
            return np.dtype(np.float64)
        raise KeyError(name)

    def _stored(self, name: str) -> np.ndarray:
        """A stored column (a coordinate, ``level`` or a hydro field), read in
        one pass over the mesh when first asked for."""
        with self._lock:
            if name not in self._kept:
                self._read([name])
            return self._kept[name]

    def _read(self, wanted: list[str]) -> None:
        """Read the columns ``wanted`` and the geometry (always: a field's
        rows mean nothing without it, and it is one pass)."""
        fields = [n for n in wanted if n in self._fields]
        with_geometry = any(n in _GEOMETRY or n == "level" for n in wanted)
        columns = {n: np.empty(self.num_rows, dtype=np.float64) for n in fields}
        if with_geometry:
            for n in (*_GEOMETRY, "level"):
                if n not in self._kept:
                    columns[n] = np.empty(self.num_rows, dtype=np.int16 if n == "level"
                                          else np.float64)
        start = 0
        for block in self.hierarchy.leaf_blocks(fields):
            stop = start + block.size
            if stop > self.num_rows:
                raise ValueError("The mesh holds more leaf cells than it counted.")
            for n in fields:
                columns[n][start:stop] = block.fields[n]
            if "level" in columns:
                columns["level"][start:stop] = block.level
                for d, n in enumerate(_GEOMETRY):
                    columns[n][start:stop] = block.centre[:, d]
            start = stop
        if start != self.num_rows:
            raise ValueError(f"The mesh gave {start} leaf cells, {self.num_rows} were counted.")
        self._kept.update(columns)

    def take(self, name: str, rows) -> np.ndarray:
        """Selected rows of a column."""
        rows = np.asarray(rows, dtype=np.int64)
        scalar = rows.ndim == 0
        rows = rows.reshape(-1)
        if rows.size and (rows.min() < 0 or rows.max() >= self.num_rows):
            raise IndexError("cell row outside the component")
        if name not in self.names:
            raise KeyError(name)
        if name == "dx":
            out = 2.0 ** -self._stored("level")[rows].astype(np.float64)
        elif name == "mass":
            out = self._stored("density")[rows] * self.take("dx", rows) ** 3
        else:
            out = self._stored(name)[rows]
        return out[0] if scalar else out

    def column(self, name: str) -> np.ndarray:
        return self.take(name, np.arange(self.num_rows, dtype=np.int64))

    def columns(self) -> LazyColumns:
        return LazyColumns(self)

    # ── description ──

    def periods(self) -> dict[str, float]:
        """Positions wrap at the box (box units) in a periodic run."""
        if not self._periodic:
            return {}
        return {n: float(e) for n, e in zip(_GEOMETRY, self.hierarchy.extent)}

    def units(self) -> dict[str, str]:
        """Each column's unit as stored (code units, stated)."""
        table = {"x": "box units (fraction of the box)", "y": "box units (fraction of the box)",
                 "z": "box units (fraction of the box)",
                 "dx": "box units (the cell side, 2^-level)",
                 "level": "refinement level",
                 "mass": "box-unit volume x code density (density * dx^3)",
                 "density": "code density (unit_d)"}
        return {n: table.get(n, "code units (physical conversion not declared)")
                for n in self.names}

    def physical(self) -> dict[str, dict]:
        """Per column, the factor that makes it physical and the unit then
        (the hierarchy's scales), what the user typed replacing it."""
        from . import physical_units  # noqa: PLC0415
        table = {n: s for n, s in self.hierarchy.scales().items() if n in self.names}
        if not self.reference:
            return table
        return physical_units.with_typed(self.reference, table, columns=tuple(self.names))

    def describe(self) -> dict:
        return {"type": "cells", "name": self.name, "family": "cells",
                "num_rows": self.num_rows, "catalogue_kind": "particles",
                "columns": list(self.names), "units": self.units(),
                "physical": self.physical(), "derived": dict(self.derived),
                "mass_from_table": False, "size_column": SIZE_COLUMN,
                "levels": list(self.hierarchy.levels)}
