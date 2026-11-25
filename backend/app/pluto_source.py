"""PLUTO's native binary output read in place as volumes (issue #315).

PLUTO writes each output as raw samples with two text files beside them:

* ``dbl.out`` / ``flt.out``: one line per output, ``index time dt step mode
  endianness var1 var2 ...``. ``mode`` is ``single_file`` (every variable of
  the output in ``data.NNNN.dbl``, one after the other) or ``multiple_files``
  (one ``<var>.NNNN.dbl`` per variable). ``.dbl`` holds float64 samples,
  ``.flt`` float32.
* ``grid.out``: a ``#`` header (``DIMENSIONS``, ``GEOMETRY``) and, for each of
  the three directions, the cell count and one ``i x_left x_right`` line per
  cell.

A variable is ``nx1 * nx2 * nx3`` samples with x1 fastest: a C array of shape
(nx3, nx2, nx1), the (z, y, x) convention of the cube path and of VBT volumes
(app/volume_source.py). It is memory-mapped at its offset, never converted, so
a 32 GB density field opens from its headers alone.

A variable is named by a reference, ``<data file>::pluto-var=<name>``, which
is a plain string: it pickles to the pyramid's worker processes and the render
sessions, which reopen it. In a pyramid signature the variable is its index in
the ``.out`` line (the "HDU" of the cube path).

What PLUTO writes is taken at its word only where the files agree: a data file
whose size is not the one the grid and the variables describe is refused, and
in ``multiple_files`` mode only the variables whose files are present are
offered (a 2048^3 run is often copied one variable at a time).
"""
from __future__ import annotations

import os
import re
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path

import numpy as np

#: The data file's extension -> (its ``.out`` file, its sample type).
EXTENSIONS = {".dbl": ("dbl.out", "f8"), ".flt": ("flt.out", "f4")}
OUT_NAMES = tuple(out for out, _ in EXTENSIONS.values())
GRID_NAME = "grid.out"
MODES = ("single_file", "multiple_files")
ENDIANS = ("little", "big")

#: ``<data file>`` + REF_SEP + variable name.
REF_SEP = "::pluto-var="

#: Cell widths equal to this relative tolerance make a uniform axis: grid.out
#: prints 13 significant digits, so a uniform grid agrees to ~1e-12.
UNIFORM_RTOL = 1e-6

_DATA_NAME = re.compile(r"^(?P<var>.+)\.(?P<num>\d+)\.(?P<ext>dbl|flt)$")
#: Face-centred magnetic (and electric) fields of a constrained-transport run:
#: one more sample along their own direction, so they are not cells of the
#: grid. They take their room in a single_file output and are not offered.
_STAGGERED = re.compile(r"^[BE]x(?P<dir>[123])s$")


# ── dbl.out / flt.out ────────────────────────────────────────────────────────

@dataclass(frozen=True)
class PlutoOutput:
    """One line of ``dbl.out`` / ``flt.out``."""
    index: int
    time: float
    dt: float
    step: int
    mode: str
    endian: str
    variables: tuple[str, ...]


def parse_out(path: str | Path) -> list[PlutoOutput]:
    """Every output ``dbl.out`` / ``flt.out`` lists, in file order. A last
    line that does not parse and has no newline is one a running simulation
    is still writing (on a parallel file system the write need not be
    atomic): skipped, so the outputs before it still open."""
    records = []
    with open(path, "r", encoding="ascii", errors="replace") as fh:
        lines = fh.readlines()
    for number, line in enumerate(lines, start=1):
        tokens = line.split()
        if not tokens or tokens[0].startswith("#"):
            continue
        try:
            records.append(_out_line(path, number, line, tokens))
        except ValueError:
            if number == len(lines) and not line.endswith("\n"):
                break
            raise
    return records


def _out_line(path, number: int, line: str, tokens: list[str]) -> PlutoOutput:
    """One line of the .out as a record; ValueError when it is not one."""
    if len(tokens) < 7:
        raise ValueError(f"{path}, line {number}: expected 'index time dt step mode "
                         f"endianness variables...', got {line.strip()!r}")
    try:
        index, time, dt, step = int(tokens[0]), float(tokens[1]), \
            float(tokens[2]), int(tokens[3])
    except ValueError as exc:
        raise ValueError(f"{path}, line {number}: {exc}") from None
    mode, endian = tokens[4].lower(), tokens[5].lower()
    if mode not in MODES:
        raise ValueError(f"{path}, line {number}: unknown output mode {tokens[4]!r}")
    if endian not in ENDIANS:
        raise ValueError(f"{path}, line {number}: unknown endianness {tokens[5]!r}")
    return PlutoOutput(index, time, dt, step, mode, endian, tuple(tokens[6:]))


def _out_records(path: str | Path) -> list[PlutoOutput]:
    st = os.stat(path)
    return _cached_out(str(Path(path).resolve()), st.st_size, st.st_mtime_ns)


@lru_cache(maxsize=32)
def _cached_out(path: str, _size: int, _mtime_ns: int) -> list[PlutoOutput]:
    return parse_out(path)


def output_record(out_path: str | Path, index: int) -> PlutoOutput | None:
    """The line of output ``index``; the last one when a restart wrote it again."""
    found = None
    for record in _out_records(out_path):
        if record.index == index:
            found = record
    return found


# ── grid.out ─────────────────────────────────────────────────────────────────

@dataclass(frozen=True)
class PlutoAxis:
    """One direction of the grid: its cells' edges and what follows from them."""
    left: np.ndarray
    right: np.ndarray

    @property
    def n(self) -> int:
        return int(self.left.size)

    @property
    def centres(self) -> np.ndarray:
        return 0.5 * (self.left + self.right)

    @property
    def uniform(self) -> bool:
        widths = self.right - self.left
        return bool(np.allclose(widths, widths[0], rtol=UNIFORM_RTOL, atol=0.0))

    @property
    def spacing(self) -> float:
        """The cell width of a uniform axis (the mean width otherwise)."""
        return float((self.right[-1] - self.left[0]) / self.n)

    @property
    def origin(self) -> float:
        """The first cell's centre."""
        return float(self.centres[0])


@dataclass(frozen=True)
class PlutoGrid:
    dimensions: int
    geometry: str
    axes: tuple[PlutoAxis, PlutoAxis, PlutoAxis]   # x1, x2, x3

    @property
    def shape(self) -> tuple[int, int, int]:
        """(nx3, nx2, nx1): the C shape of one variable, x1 fastest."""
        return (self.axes[2].n, self.axes[1].n, self.axes[0].n)

    @property
    def cells(self) -> int:
        return self.axes[0].n * self.axes[1].n * self.axes[2].n

    def index_reason(self) -> str:
        """Why the grid is shown on cell indices, "" when it is not: a
        non-Cartesian geometry, or a direction whose cells differ in width."""
        if self.geometry != "CARTESIAN":
            return f"{self.geometry.lower()} geometry"
        stretched = [f"x{d + 1}" for d, axis in enumerate(self.axes)
                     if axis.n > 1 and not axis.uniform]
        return f"non-uniform cells along {', '.join(stretched)}" if stretched else ""


def parse_grid(path: str | Path) -> PlutoGrid:
    """The grid ``grid.out`` describes. The three directions are always there
    (a direction the run does not have is one cell); a file listing fewer gets
    one-cell directions for the missing ones."""
    st = os.stat(path)
    return _cached_grid(str(Path(path).resolve()), st.st_size, st.st_mtime_ns)


@lru_cache(maxsize=8)
def _cached_grid(path: str, _size: int, _mtime_ns: int) -> PlutoGrid:
    dimensions, geometry = 0, "CARTESIAN"
    body: list[str] = []
    with open(path, "r", encoding="ascii", errors="replace") as fh:
        for line in fh:
            text = line.strip()
            if not text:
                continue
            if text.startswith("#"):
                key, _, value = text.lstrip("#").partition(":")
                key = key.strip().upper()
                if key == "DIMENSIONS":
                    dimensions = int(value.split()[0])
                elif key == "GEOMETRY":
                    geometry = value.strip().split()[0].upper() if value.strip() else geometry
                continue
            body.append(text)
    axes: list[PlutoAxis] = []
    pos = 0
    while pos < len(body) and len(axes) < 3:
        try:
            n = int(body[pos].split()[0])
        except ValueError:
            raise ValueError(f"{path}: expected a cell count, got {body[pos]!r}") from None
        rows = body[pos + 1: pos + 1 + n]
        if n < 1 or len(rows) != n:
            raise ValueError(f"{path}: direction x{len(axes) + 1} announces {n} cells "
                             f"and lists {len(rows)}")
        # One parse of the whole block: 2048 lines per direction in a few ms.
        values = np.array(" ".join(rows).split(), dtype=np.float64)
        if values.size % n:
            raise ValueError(f"{path}: the cells of x{len(axes) + 1} have uneven columns")
        table = values.reshape(n, values.size // n)
        if table.shape[1] == 3:        # i  x_left  x_right (PLUTO 4)
            left, right = table[:, 1], table[:, 2]
        elif table.shape[1] == 5:      # i  x_left  x_centre  x_right  dx (PLUTO 3)
            left, right = table[:, 1], table[:, 3]
        else:
            raise ValueError(f"{path}: cells of x{len(axes) + 1} have {table.shape[1]} "
                             "columns; expected 'i x_left x_right'")
        if np.any(right <= left):
            raise ValueError(f"{path}: x{len(axes) + 1} has a cell with no width")
        axes.append(PlutoAxis(left.copy(), right.copy()))
        pos += 1 + n
    if not axes:
        raise ValueError(f"{path} lists no cells.")
    while len(axes) < 3:
        axes.append(PlutoAxis(np.zeros(1), np.ones(1)))
    return PlutoGrid(dimensions or sum(1 for a in axes if a.n > 1), geometry, tuple(axes))


# ── Data files ───────────────────────────────────────────────────────────────

@dataclass(frozen=True)
class PlutoVariable:
    """One variable of one output, where its samples are."""
    data_path: str          # the file holding it
    name: str
    index: int              # its place in the .out line
    offset: int             # bytes
    dtype: np.dtype         # sample type, byte order included
    shape: tuple[int, int, int]
    out_path: str
    grid_path: str
    record: PlutoOutput
    grid: PlutoGrid


def _parts(path: str | Path):
    """``(directory, variable or 'data', output index, extension)`` of a data
    file's name, or None for a name PLUTO does not write."""
    path = Path(path)
    match = _DATA_NAME.match(path.name)
    if match is None:
        return None
    return path.parent, match["var"], int(match["num"]), "." + match["ext"]


def _variable_shape(grid: PlutoGrid, name: str) -> tuple[int, int, int]:
    shape = list(grid.shape)
    staggered = _STAGGERED.match(name)
    if staggered:
        direction = int(staggered["dir"])
        if direction <= max(grid.dimensions, 1):
            shape[3 - direction] += 1
    return tuple(shape)


def is_cell_variable(name: str) -> bool:
    """A cell-centred variable: not a face-centred (staggered) field."""
    return _STAGGERED.match(name) is None


def _sources(path: str | Path):
    """``(out file, grid file, record, parts)`` of a data file, or None when
    it is not one: a PLUTO name, its ``.out`` beside it listing the output.
    The grid file is returned even when it is absent (the caller says so)."""
    parts = _parts(path)
    if parts is None:
        return None
    directory, var, number, ext = parts
    out = directory / EXTENSIONS[ext][0]
    if not out.is_file():
        return None
    try:
        record = output_record(out, number)
    except (OSError, ValueError):
        return None
    if record is None:
        return None
    if record.mode == "single_file" and var != "data":
        return None
    if record.mode == "multiple_files" and var not in record.variables:
        return None
    return out, directory / GRID_NAME, record, parts


def is_pluto(path: str | Path) -> bool:
    """Whether ``path`` is a PLUTO data file with its ``.out`` beside it."""
    return _sources(path) is not None


def pluto_variable(data_path: str | Path, name: str | None = None,
                   index: int | None = None) -> PlutoVariable:
    """Variable ``name`` (else number ``index`` of the ``.out`` line, else the
    file's own: ``rho.0000.flt``'s ``rho``, ``data.0000.dbl``'s first) of the
    data file ``data_path``, its file's size checked against the grid."""
    sources = _sources(data_path)
    if sources is None:
        raise ValueError(f"{data_path} is not a PLUTO output with its "
                         f"{'/'.join(OUT_NAMES)} beside it.")
    out, grid_path, record, (directory, file_var, number, ext) = sources
    if not grid_path.is_file():
        raise ValueError(f"{data_path}: the {GRID_NAME} that gives the grid is not beside it.")
    grid = parse_grid(grid_path)
    if name is None:
        if index is not None:
            if not 0 <= int(index) < len(record.variables):
                raise ValueError(f"variable {index} outside 0..{len(record.variables) - 1} "
                                 f"of {data_path}")
            name = record.variables[int(index)]
        else:
            name = record.variables[0] if record.mode == "single_file" else file_var
    if name not in record.variables:
        raise ValueError(f"output {number} has no variable {name!r} "
                         f"(it has {', '.join(record.variables)})")
    if not is_cell_variable(name):
        raise ValueError(f"{name} is a face-centred field, not one value per cell.")
    if record.mode == "multiple_files" and name != file_var:
        raise ValueError(f"{Path(data_path).name} holds {file_var}, not {name}.")
    dtype = np.dtype(("<" if record.endian == "little" else ">") + EXTENSIONS[ext][1])
    shape = _variable_shape(grid, name)
    var_bytes = int(np.prod(shape)) * dtype.itemsize
    if record.mode == "single_file":
        offset = sum(int(np.prod(_variable_shape(grid, v))) * dtype.itemsize
                     for v in record.variables[: record.variables.index(name)])
        expected = sum(int(np.prod(_variable_shape(grid, v))) * dtype.itemsize
                       for v in record.variables)
    else:
        offset, expected = 0, var_bytes
    size = Path(data_path).stat().st_size
    if size != expected:
        raise ValueError(f"{data_path} holds {size} bytes; {GRID_NAME} and "
                         f"{out.name} describe {expected}.")
    return PlutoVariable(str(data_path), name, record.variables.index(name), offset, dtype,
                         shape, str(out), str(grid_path), record, grid)


def pluto_field(data_path: str | Path, name: str | None = None) -> np.memmap:
    """Variable ``name`` memory-mapped in place as a (z, y, x) array."""
    var = pluto_variable(data_path, name)
    return np.memmap(var.data_path, dtype=var.dtype, mode="r", shape=var.shape,
                     offset=var.offset)


def variable_file(record: PlutoOutput, directory: Path, ext: str, name: str) -> Path:
    """The file holding variable ``name`` of output ``record``."""
    stem = "data" if record.mode == "single_file" else name
    return directory / f"{stem}.{record.index:04d}{ext}"


# ── References ───────────────────────────────────────────────────────────────

def pluto_ref(data_path: str | Path, name: str) -> str:
    return f"{split_ref(data_path)[0]}{REF_SEP}{name}"


def split_ref(path: str | Path) -> tuple[str, str | None]:
    """``(data path, variable)`` of a reference; ``(path, None)`` else."""
    text = str(path)
    base, sep, name = text.rpartition(REF_SEP)
    if sep and name:
        return base, name
    return text, None


def is_pluto_ref(path: str | Path) -> bool:
    """Whether ``path`` is a PLUTO variable reference, from the string alone."""
    return split_ref(path)[1] is not None


def resolve(path: str | Path, hdu: int | None = None) -> PlutoVariable | None:
    """The variable ``path`` stands for: a reference, or a data file with
    ``hdu`` the variable's index in the ``.out`` line (None: the file's own).
    None when ``path`` is neither; an error when it is one that cannot be read."""
    base, name = split_ref(path)
    if name is not None:
        return pluto_variable(base, name)
    if _parts(base) is None or not is_pluto(base):
        return None
    return pluto_variable(base, index=hdu)


def _newest_output(out_path: Path) -> PlutoOutput | None:
    records = _out_records(out_path)
    return max(records, key=lambda r: r.index) if records else None


def present_variables(record: PlutoOutput, directory: Path, ext: str) -> list[tuple[str, Path]]:
    """``(name, file)`` of the output's cell variables whose file is there."""
    found = []
    for name in record.variables:
        if not is_cell_variable(name):
            continue
        path = variable_file(record, directory, ext, name)
        if path.is_file():
            found.append((name, path))
    return found


def pluto_dataset_path(path: str | Path, name: str | None = None) -> str | None:
    """The reference /v1/datasets/open registers for a PLUTO output given by a
    data file, a reference or its ``.out`` (the newest output's first variable
    present); None when ``path`` is none of them or does not open."""
    base, ref_name = split_ref(path)
    name = name or ref_name
    base_path = Path(base)
    if base_path.name in OUT_NAMES:
        if not base_path.is_file():
            return None
        try:
            record = _newest_output(base_path)
        except (OSError, ValueError):
            return None
        if record is None:
            return None
        ext = "." + base_path.name.split(".")[0]
        present = present_variables(record, base_path.parent, ext)
        if name is not None:
            present = [(n, p) for n, p in present if n == name]
        if not present:
            return None
        base_path = present[0][1]
        name = present[0][0]
    try:
        var = pluto_variable(base_path, name)
    except (OSError, ValueError):
        return None
    return pluto_ref(var.data_path, var.name)


def pluto_fields(path: str | Path) -> list[dict]:
    """Every cell variable of the output ``path`` (a data file or reference)
    belongs to, whose file is present, with the reference opening it."""
    sources = _sources(split_ref(path)[0])
    if sources is None:
        return []
    _out, _grid, record, (directory, _var, _num, ext) = sources
    return [{"name": name, "path": pluto_ref(file, name)}
            for name, file in present_variables(record, directory, ext)]


# ── As a FITS cube ───────────────────────────────────────────────────────────

def pluto_fits_header(var: PlutoVariable):
    """The header the variable is read with as a FITS cube. A uniform Cartesian
    grid: linear X, Y, Z axes at the cells' spacing, CRVAL the first cell's
    centre. Otherwise the axes count cells (CDELT 1, CRVAL 1 at the first,
    grid.out's numbering), and PLUTOGRD = 'INDEX' with PLUTOWHY say why."""
    from astropy.io import fits  # noqa: PLC0415

    grid = var.grid
    reason = grid.index_reason()
    hdr = fits.Header()
    hdr["SIMPLE"] = True
    hdr["BITPIX"] = -32 if var.dtype.itemsize == 4 else -64
    hdr["NAXIS"] = 3
    for axis, (ax, n, name) in enumerate(zip(grid.axes, var.shape[::-1], "XYZ"), start=1):
        hdr[f"NAXIS{axis}"] = int(n)
        # X/Y/Z on both: the cube viewer then knows the axes are space, not a
        # spectrum, and keeps its spectral tools off.
        hdr[f"CTYPE{axis}"] = name
        hdr[f"CRPIX{axis}"] = 1.0
        if reason:
            hdr[f"CRVAL{axis}"] = 1.0
            hdr[f"CDELT{axis}"] = 1.0
            hdr[f"CNAME{axis}"] = f"x{axis} cell index"
        else:
            hdr[f"CRVAL{axis}"] = ax.origin
            hdr[f"CDELT{axis}"] = ax.spacing
    hdr["OBJECT"] = var.name
    hdr["PLUTOVAR"] = (var.name, "PLUTO variable")
    hdr["PLUTOTIM"] = (var.record.time, "PLUTO output time (code units)")
    hdr["PLUTOSTP"] = (var.record.step, "PLUTO integration step")
    hdr["PLUTONUM"] = (var.record.index, "PLUTO output number")
    hdr["PLUTOGEO"] = (grid.geometry, "PLUTO grid geometry")
    if reason:
        hdr["PLUTOGRD"] = ("INDEX", "axes count cells, not coordinates")
        hdr["PLUTOWHY"] = (reason[:60], "why the axes count cells")
    else:
        hdr["PLUTOGRD"] = ("UNIFORM", "axes are coordinates (code units)")
    return hdr


# ── A Cartesian view of a variable (app/compute/pluto_cartesian.py) ──────────

#: A variable resampled onto a uniform Cartesian grid is a FITS cube of this
#: name, its source variable's reference in the card below.
VIEW_SUFFIX = ".pluto-cartesian.fits"
SOURCE_KEY = "PLUTOSRC"


def view_source(path: str | Path) -> str | None:
    """The PLUTO variable reference a Cartesian-view cube was resampled from
    (its ``PLUTOSRC`` card), so its values and coordinates are shown in that
    run's physical units; None for any other file."""
    from .physical_units import view_card  # noqa: PLC0415
    return view_card(path, VIEW_SUFFIX, SOURCE_KEY)
