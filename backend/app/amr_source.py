"""An adaptive-mesh refinement hierarchy, whatever code wrote it (issue #549).

RAMSES (app/ramses_output.py) is the first producer; Enzo, FLASH, Athena++ or a
yt-loaded dataset are further ones (issue #554). What the AMR views build on
(cells as points, a uniform cube, a level map, a refine box: issue #550) is
this small interface and nothing of any code's file layout.

The mesh
--------
Cells are cubes. A cell of level ``l`` has the side ``dx = 2**-l`` in *box
units*, the units in which the coarse mesh's cell has side 1 (``extent`` is
the box in them: (1, 1, 1) for a RAMSES run with nx = ny = nz = 1, so a
position is a fraction of the box). Box units are not metres: a producer says
what one is in physical units through :meth:`AmrHierarchy.scales` (the same
``{column: {"factor", "unit"}}`` table as ``app.physical_units.scales``).

Only *leaf* cells are exposed: a cell is a leaf when it is not refined (a
RAMSES cell is refined when its son index is not 0); the leaves tile the box
exactly once, so the sum of their volumes is the box's. Coarser cells hold the
average of their children and are not needed to draw a field.

Reading
-------
Nothing is read at construction. :meth:`AmrHierarchy.leaf_blocks` yields
:class:`LeafBlock` s, each the leaves of one level in one piece of the
domain decomposition (a RAMSES CPU file), at most ``max_block`` cells, so a
view reads a column or a level subset in bounded memory however many cells the
run has; :meth:`AmrHierarchy.leaf_cells` joins them for a selection that is
known to be small (it refuses more than ``max_cells``).

A single level
--------------
A run that is not refined, or one whose leaves all lie on one level, is a
uniform grid: :meth:`AmrHierarchy.uniform_level` returns that level, and the
cube is ``extent * 2**level`` cells on each side, a cell's index along an axis
being ``floor(centre / dx)``. Such an output opens as a plain volume with no
AMR step (the cell values of each block are then written into a numpy cube at
those indices); this module needs nothing more for it.
"""
from __future__ import annotations

from abc import ABC, abstractmethod
from collections.abc import Iterator, Sequence
from dataclasses import dataclass, field

import numpy as np

#: Cells :meth:`AmrHierarchy.leaf_cells` joins at most (a 512^3 cube's worth
#: would be 134 million: that is a job for ``leaf_blocks``).
MAX_JOINED_CELLS = 64_000_000


@dataclass(frozen=True)
class LeafBlock:
    """Leaf cells of one level: ``centre`` (n, 3) float64 in box units, the
    common side ``dx`` (box units), and one array of n values per field."""
    level: int
    dx: float
    centre: np.ndarray
    fields: dict[str, np.ndarray] = field(default_factory=dict)

    @property
    def size(self) -> int:
        return int(self.centre.shape[0])


@dataclass(frozen=True)
class LeafCells:
    """Leaf cells of any levels joined: ``level`` (n,) int16, ``dx`` (n,)
    float64 (box units), ``centre`` (n, 3), and the fields' (n,) arrays."""
    level: np.ndarray
    dx: np.ndarray
    centre: np.ndarray
    fields: dict[str, np.ndarray]

    @property
    def size(self) -> int:
        return int(self.level.shape[0])


class AmrHierarchy(ABC):
    """What every AMR producer gives. Subclasses read lazily and per piece of
    the domain; none of these methods may load a whole run."""

    #: The producing code ("ramses").
    code: str = ""

    @property
    @abstractmethod
    def extent(self) -> tuple[float, float, float]:
        """The box, in box units."""

    @property
    @abstractmethod
    def levels(self) -> tuple[int, ...]:
        """The levels that hold leaf cells, ascending."""

    @property
    @abstractmethod
    def fields(self) -> tuple[str, ...]:
        """The names of the cell values a leaf carries."""

    @abstractmethod
    def leaf_counts(self) -> dict[int, int]:
        """Leaf cells per level (read once, then kept)."""

    @abstractmethod
    def leaf_blocks(self, fields: Sequence[str] = (), levels: Sequence[int] | None = None,
                    lo: Sequence[float] | None = None, hi: Sequence[float] | None = None,
                    max_block: int = 1 << 20) -> Iterator[LeafBlock]:
        """The leaves of ``levels`` (None: all) whose centres lie in the box
        ``lo`` .. ``hi`` (box units; None: unbounded), carrying ``fields``,
        one block at a time. An unknown field raises ValueError."""

    @abstractmethod
    def scales(self) -> dict[str, dict]:
        """``{column: {"factor", "unit"}}`` over the coordinates ``x``, ``y``,
        ``z``, the cell size ``dx`` and the fields: what makes each physical."""

    @abstractmethod
    def describe(self) -> dict:
        """What the hierarchy states about itself (time, box, levels, units)."""

    # ── derived, the same for every producer ──

    def n_leaf(self, levels: Sequence[int] | None = None) -> int:
        counts = self.leaf_counts()
        wanted = None if levels is None else set(levels)
        return int(sum(n for lv, n in counts.items() if wanted is None or lv in wanted))

    def volume(self) -> float:
        """Sum of the leaves' volumes, in box units: the box's volume
        (``prod(extent)``) when the leaves tile it."""
        return float(sum(n * (2.0 ** -lv) ** 3 for lv, n in self.leaf_counts().items()))

    def uniform_level(self) -> int | None:
        """The level of a uniform grid (see the module docstring): the one
        level that holds every leaf, when the leaves fill the box at it; None
        for a refined mesh."""
        counts = {lv: n for lv, n in self.leaf_counts().items() if n}
        if len(counts) != 1:
            return None
        (level, n), = counts.items()
        cells = float(np.prod(self.extent)) * float(8 ** level)
        return int(level) if float(n) == cells else None

    def finest_level_in(self, lo: Sequence[float] | None = None,
                        hi: Sequence[float] | None = None) -> int | None:
        """The finest level among the leaves that overlap the box ``lo`` ..
        ``hi`` (box units; None: unbounded), None when none does. A leaf
        overlaps it when its extent does, not only its centre: a coarse cell
        that merely reaches into the box counts."""
        if lo is None and hi is None:
            levels = self.levels
            return int(levels[-1]) if levels else None
        coarsest = 2.0 ** -min(self.levels) if self.levels else 1.0
        pad = 0.5 * coarsest
        wide_lo = None if lo is None else [v - pad for v in lo]
        wide_hi = None if hi is None else [v + pad for v in hi]
        best = None
        for block in self.leaf_blocks((), None, wide_lo, wide_hi):
            if best is not None and block.level <= best:
                continue
            reach = 0.5 * block.dx
            keep = np.ones(block.size, dtype=bool)
            for d in range(3):
                if lo is not None:
                    keep &= block.centre[:, d] + reach > lo[d]
                if hi is not None:
                    keep &= block.centre[:, d] - reach < hi[d]
            if keep.any():
                best = block.level
        return best

    def leaf_cells(self, fields: Sequence[str] = (), levels: Sequence[int] | None = None,
                   lo: Sequence[float] | None = None, hi: Sequence[float] | None = None,
                   max_cells: int = MAX_JOINED_CELLS) -> LeafCells:
        """The selected leaves joined; ValueError when there are more than
        ``max_cells`` (select fewer levels or a smaller box, or stream
        :meth:`leaf_blocks`)."""
        blocks, total = [], 0
        for block in self.leaf_blocks(fields, levels, lo, hi):
            total += block.size
            if total > max_cells:
                raise ValueError(f"The selection holds more than {max_cells} leaf cells: "
                                 "ask for fewer levels or a smaller box.")
            blocks.append(block)
        if not blocks:
            return LeafCells(np.empty(0, np.int16), np.empty(0), np.empty((0, 3)),
                             {f: np.empty(0) for f in fields})
        return LeafCells(
            np.concatenate([np.full(b.size, b.level, dtype=np.int16) for b in blocks]),
            np.concatenate([np.full(b.size, b.dx) for b in blocks]),
            np.concatenate([b.centre for b in blocks]),
            {f: np.concatenate([b.fields[f] for b in blocks]) for f in fields})


def open_hierarchy(path, allowed=None) -> AmrHierarchy:
    """The mesh a path names: a RAMSES output (its directory, a file of it or
    a component reference). The one place a further producer (issue #554)
    is added, so the views and their routes need no change.

    ``allowed`` (path -> bool): every file of the output must pass it, else
    PermissionError, before any is parsed. ValueError when ``path`` holds no
    mesh."""
    from . import ramses_output as rm   # noqa: PLC0415
    directory = rm.checked_output(path, allowed)
    if directory is not None:
        return rm.open_output(directory).amr()
    raise ValueError(f"{path} holds no AMR mesh this backend reads (a RAMSES output).")
