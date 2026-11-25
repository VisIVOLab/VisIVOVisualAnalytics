"""Physical units of a simulation volume (issue #524).

A PLUTO run writes code units: the density, length and velocity of the run are
three numbers (``UNIT_DENSITY``, ``UNIT_LENGTH``, ``UNIT_VELOCITY``, cgs) set in
its ``definitions.h``, and every other quantity follows from them the way PLUTO
derives them (time = L / V, pressure = rho V^2, magnetic field =
V sqrt(4 pi rho)). The data is never rewritten: the factors are given to the
viewers, which show ``value * factor`` in the unit named here, so a slice, a
probe or a colour bar can be read in g/cm^3 while every computation on the
file stays in code units.

One shape serves every reader: ``{column: {"factor", "unit"[, "source"]}}``,
the factor that makes the column physical and the unit it is then in, as
Gadget's ``Component.physical()`` has always given it. A producer states its
own columns (Gadget through its ``UnitSystem``; a PLUTO or VBT volume through
the run's code units below) and :func:`scales` adds what the user typed, so
every consumer (a route, a slice, an export, a client) reads one table. A
volume has one data column, ``"value"``, its coordinates ``"x"``, ``"y"``,
``"z"`` and, for PLUTO, ``"time"``; a column that is absent has no known
conversion (code units).

The factors come from, in this order of priority:

1. what the user typed (:func:`set_user`), kept in a small JSON file that every
   process of the backend reads, so it survives reopening; the coordinates and
   the time of a PLUTO run are shared by all its variables;
2. the producer: for a PLUTO run, its ``definitions.h`` (or a ``pluto.ini`` that
   carries the same ``UNIT_*`` names) found next to the data, up to two
   directories above it; for a Gadget snapshot, its unit system;
3. nothing: the values and coordinates stay in code units.

A typed factor and unit replaces what the producer gives for that one column
(any format: a Gadget column, a PLUTO variable, a VBT field, a FITS cube).
"""
from __future__ import annotations

import ast
import json
import logging
import math
import operator
import os
import re
import threading
from dataclasses import dataclass
from pathlib import Path

from .vbt_byte_order_store import _exclusive, _write

logger = logging.getLogger("visivo.backend")

_LOCK = threading.Lock()
_cache: tuple[tuple, dict] | None = None

#: Constants of PLUTO's pluto.h (cgs) a definitions.h expression may use.
PLUTO_CONSTANTS = {
    "CONST_amu": 1.66053886e-24, "CONST_mp": 1.67262171e-24, "CONST_mH": 1.6733e-24,
    "CONST_me": 9.1093826e-28, "CONST_kB": 1.3806505e-16, "CONST_c": 2.99792458e10,
    "CONST_G": 6.6726e-8, "CONST_pc": 3.0856775807e18, "CONST_au": 1.49597870691e13,
    "CONST_ly": 9.4607e17, "CONST_Msun": 1.9891e33, "CONST_sigma": 5.6704e-5,
    "CONST_NA": 6.0221415e23, "CONST_PI": math.pi, "CONST_pi": math.pi,
}
UNIT_NAMES = ("UNIT_DENSITY", "UNIT_LENGTH", "UNIT_VELOCITY")
#: The three user-typed run units: cgs, with the quantity they scale.
RUN_UNIT_KEYS = ("unit_density", "unit_length", "unit_velocity")
DEFINITIONS_NAMES = ("definitions.h", "pluto.ini")
_DEFINE = re.compile(r"^\s*(?:#\s*define\s+)?(UNIT_(?:DENSITY|LENGTH|VELOCITY))\b\s*=?\s*(.+?)\s*$")
_OPS = {ast.Add: operator.add, ast.Sub: operator.sub, ast.Mult: operator.mul,
        ast.Div: operator.truediv, ast.Pow: operator.pow}


@dataclass(frozen=True)
class RunUnits:
    """The code units of a run, cgs (None: not known)."""
    density: float | None = None
    length: float | None = None
    velocity: float | None = None
    source: str = "none"          # "none", "definitions" or "user"
    origin: str = ""              # the file the factors were read from

    @property
    def time(self) -> float | None:
        return self.length / self.velocity if self.length and self.velocity else None

    @property
    def pressure(self) -> float | None:
        return self.density * self.velocity ** 2 if self.density and self.velocity else None

    @property
    def magnetic(self) -> float | None:
        """The field in Gauss, PLUTO's Gaussian convention: V sqrt(4 pi rho)."""
        if not (self.density and self.velocity):
            return None
        return self.velocity * math.sqrt(4.0 * math.pi * self.density)


#: One column's conversion: ``value * factor`` is the value in ``unit``;
#: ``source`` ("definitions" or "user") is present when the producer or the
#: user stated it rather than a reader's own table.
ColumnScale = dict
VALUE = "value"
COORDINATES = ("x", "y", "z")
TIME = "time"
#: Columns shared by every variable of a PLUTO run.
RUN_COLUMNS = COORDINATES + (TIME,)


# ── definitions.h ───────────────────────────────────────────────────────────

def _eval(node: ast.AST) -> float:
    if isinstance(node, ast.Expression):
        return _eval(node.body)
    if (isinstance(node, ast.Constant) and isinstance(node.value, (int, float))
            and not isinstance(node.value, bool)):
        return float(node.value)
    if isinstance(node, ast.Name) and node.id in PLUTO_CONSTANTS:
        return PLUTO_CONSTANTS[node.id]
    if isinstance(node, ast.UnaryOp) and isinstance(node.op, (ast.USub, ast.UAdd)):
        value = _eval(node.operand)
        return -value if isinstance(node.op, ast.USub) else value
    if isinstance(node, ast.BinOp) and type(node.op) in _OPS:
        return _OPS[type(node.op)](_eval(node.left), _eval(node.right))
    raise ValueError("unsupported expression")


def evaluate_define(text: str) -> float | None:
    """The number a ``#define`` right-hand side stands for: a literal, or an
    arithmetic expression of literals and PLUTO's ``CONST_*``; None when it is
    anything else (a function call, an unknown name) or not a positive number."""
    text = re.sub(r"/\*.*?\*/|//.*$", "", text).strip()
    text = re.sub(r"(?<=[0-9.])[fFlL]\b", "", text)      # C suffixes: 1.0e18L
    if len(text) > 200:     # a unit is a short expression; a long one is not worth parsing
        return None
    try:
        value = _eval(ast.parse(text, mode="eval"))
    except (SyntaxError, ValueError, ZeroDivisionError, OverflowError, TypeError,
            RecursionError, MemoryError):
        return None
    return value if math.isfinite(value) and value > 0 else None


def parse_definitions(text: str) -> dict[str, float]:
    """``{UNIT_DENSITY: ..., UNIT_LENGTH: ..., UNIT_VELOCITY: ...}`` of those a
    definitions.h (or pluto.ini) text sets to a usable number. A later line
    overrides an earlier one, as the C preprocessor would after an ``#undef``."""
    out: dict[str, float] = {}
    for line in text.splitlines():
        m = _DEFINE.match(line)
        if m is None:
            continue
        value = evaluate_define(m.group(2))
        if value is not None:
            out[m.group(1)] = value
    return out


def definitions_file(data_path: str | os.PathLike) -> Path | None:
    """The run's definitions.h (else pluto.ini that sets a UNIT_*) beside the
    data file or up to two directories above it, nearest first."""
    here = Path(data_path).resolve().parent
    for directory in (here, *list(here.parents)[:2]):
        for name in DEFINITIONS_NAMES:
            candidate = directory / name
            try:
                if candidate.is_file() and parse_definitions(
                        candidate.read_text("utf-8", errors="replace")):
                    return candidate
            except OSError:
                continue
    return None


def run_units_from_files(data_path: str | os.PathLike) -> RunUnits:
    found = definitions_file(data_path)
    if found is None:
        return RunUnits()
    values = parse_definitions(found.read_text("utf-8", errors="replace"))
    return RunUnits(values.get("UNIT_DENSITY"), values.get("UNIT_LENGTH"),
                    values.get("UNIT_VELOCITY"), "definitions", found.name)


# ── The user's typed values ─────────────────────────────────────────────────

def store_path() -> Path:
    explicit = os.environ.get("VISIVO_PHYSICAL_UNITS")
    if explicit:
        return Path(explicit)
    root = os.environ.get("VISIVO_EXPORTS_DIR", str(Path.home() / ".visivo" / "exports"))
    return Path(root) / ".physical-units.json"


def _load() -> dict:
    global _cache
    path = store_path()
    try:
        st = os.stat(path)
    except OSError:
        return {}
    stamp = (str(path), st.st_size, st.st_mtime_ns, st.st_ino)
    with _LOCK:
        if _cache is not None and _cache[0] == stamp:
            return _cache[1]
    try:
        entries = json.loads(path.read_text("utf-8"))
        if not isinstance(entries, dict):
            raise ValueError("not an object")
    except (OSError, ValueError) as exc:
        logger.warning("[physical-units] ignoring unreadable store %s: %s", path, exc)
        entries = {}
    with _LOCK:
        _cache = (stamp, entries)
    return entries


def pluto_ref_of(path: str | os.PathLike) -> str:
    """The PLUTO reference whose units ``path`` is shown in: a Cartesian view
    of a variable (issue #527) is that variable's, in the run's code units;
    any other path is its own."""
    from . import pluto_source  # noqa: PLC0415
    return pluto_source.view_source(path) or str(path)


def _pluto_split(path: str | os.PathLike) -> tuple[str, str | None]:
    from . import pluto_source  # noqa: PLC0415
    return pluto_source.split_ref(pluto_ref_of(path))


def _gadget_split(path: str | os.PathLike) -> tuple[str, int | None]:
    from . import gadget_snapshot  # noqa: PLC0415
    return gadget_snapshot.split_ref(path)


def _ramses_split(path: str | os.PathLike) -> tuple[str, str | None]:
    from . import ramses_output  # noqa: PLC0415
    return ramses_output.split_ref(path)


def run_key(path: str | os.PathLike) -> str:
    """What the run-wide units are kept under: the directory of a PLUTO run
    (every variable and every output of it shares them), else the file (a
    RAMSES output's directory, whichever family names it)."""
    base, name = _pluto_split(_ramses_split(path)[0])
    real = os.path.realpath(base)
    return ("dir:" + os.path.dirname(real)) if name is not None else ("file:" + real)


def dataset_key(path: str | os.PathLike) -> str:
    """What one dataset's own columns are kept under: its reference, symlinks
    resolved in the file part; every particle type of a Gadget snapshot shares
    the snapshot's, as its columns do, and every family of a RAMSES output the
    output directory's."""
    path = _ramses_split(path)[0]
    base, ptype = _gadget_split(path)
    if ptype is not None:
        return os.path.realpath(base)
    base, name = _pluto_split(path)
    return os.path.realpath(base) + (f"::{name}" if name is not None else "")


def _scope(path: str | os.PathLike, column: str) -> str:
    """Where a typed column is kept: the coordinates and the time of a PLUTO
    run are the run's; every other column is its dataset's."""
    return run_key(path) if column in RUN_COLUMNS and _pluto_split(path)[1] is not None \
        else dataset_key(path)


def _positive(value, what: str) -> float:
    value = float(value)
    if not (math.isfinite(value) and value > 0):
        raise ValueError(f"{what} must be a positive number.")
    return value


def set_user(path: str | os.PathLike, *, run: dict[str, float] | None = None,
             columns: dict[str, tuple[float, str]] | None = None,
             clear: bool = False, by: str = "") -> None:
    """Keep what the user typed for the dataset ``path``: ``run`` the cgs code
    units of the PLUTO run (keys of :data:`RUN_UNIT_KEYS`, merged into those
    already kept), ``columns`` a factor and a unit per column (``"value"``, the
    coordinates ``"x"``, ``"y"``, ``"z"``, or a Gadget column), merged into
    those already kept. ``clear`` forgets everything kept for this dataset and
    run first. A factor must be a positive finite number."""
    global _cache
    rk, dk = run_key(path), dataset_key(path)
    clean_run = {}
    for key, v in (run or {}).items():
        if key not in RUN_UNIT_KEYS:
            raise ValueError(f"Unknown run unit {key!r}.")
        clean_run[key] = _positive(v, key)
    clean_columns = {}
    for column, (factor, unit) in (columns or {}).items():
        clean_columns[column] = {"factor": _positive(factor, f"The {column} factor"),
                                 "unit": str(unit).strip()}
    store = store_path()
    with _exclusive(store):
        try:
            entries = json.loads(store.read_text("utf-8"))
            if not isinstance(entries, dict):
                entries = {}
        except (OSError, ValueError):
            entries = {}
        runs = entries.setdefault("runs", {})
        typed = entries.setdefault("columns", {})
        if clear:
            runs.pop(rk, None)
            typed.pop(rk, None)
            typed.pop(dk, None)
        if clean_run:
            runs.setdefault(rk, {}).update(clean_run)
        for column, entry in clean_columns.items():
            typed.setdefault(_scope(path, column), {})[column] = entry
        _write(store, entries)
        with _LOCK:
            _cache = None
    logger.info("[physical-units] %s: %s by %s", dk, "cleared" if clear else "set", by or "unknown")


def clear_store() -> None:
    """Forget everything typed (tests)."""
    global _cache
    try:
        os.unlink(store_path())
    except OSError:
        pass
    with _LOCK:
        _cache = None


def is_simulation_volume(path: str | os.PathLike, kind: str, ctype) -> bool:
    """A cube of a simulation rather than of the sky: a VBT field or PLUTO
    variable, or a FITS cube whose three axes are named X, Y and Z (a spectral
    cube has a sky pair and a spectral axis, never that)."""
    if kind != "cube":
        return False
    from .volume_source import is_field_ref  # noqa: PLC0415
    if is_field_ref(path):
        return True
    names = [str(c).strip().upper() for c in ctype or []]
    return len(names) == 3 and set(names) == {"X", "Y", "Z"}


# ── What a variable is shown in ─────────────────────────────────────────────

def variable_quantity(name: str) -> str:
    """The physical quantity of a PLUTO variable from its name; "" for one this
    does not know (a user's own tracer or diagnostic)."""
    n = name.strip()
    low = n.lower()
    if low == "rho":
        return "density"
    if low.startswith("vx") or low in ("vr", "vth", "vphi", "vz", "vtheta"):
        return "velocity"
    if low in ("prs", "pr", "p"):
        return "pressure"
    if low.startswith("bx") or low in ("br", "bth", "bphi", "bz", "btheta"):
        return "magnetic"
    if re.fullmatch(r"tr\d+", low):
        return "tracer"
    return ""


_QUANTITY_UNIT = {"density": "g/cm^3", "velocity": "cm/s", "pressure": "dyn/cm^2",
                  "magnetic": "G", "tracer": ""}


def _quantity_factor(run: RunUnits, quantity: str) -> float | None:
    return {"density": run.density, "velocity": run.velocity, "pressure": run.pressure,
            "magnetic": run.magnetic, "tracer": 1.0}.get(quantity)


def merged_run_units(path: str | os.PathLike) -> RunUnits:
    """The run's code units: the file's, each replaced by what the user typed."""
    base, name = _pluto_split(path)
    run = run_units_from_files(base) if name is not None else RunUnits()
    typed = _load().get("runs", {}).get(run_key(path), {})
    over = {}
    for key, attr in (("unit_density", "density"), ("unit_length", "length"),
                      ("unit_velocity", "velocity")):
        v = typed.get(key)
        if isinstance(v, (int, float)) and math.isfinite(v) and v > 0:
            over[attr] = float(v)
    if not over:
        return run
    return RunUnits(over.get("density", run.density), over.get("length", run.length),
                    over.get("velocity", run.velocity), "user", run.origin)


def _volume_scales(path: str | os.PathLike) -> dict[str, ColumnScale]:
    """The producer of a volume (PLUTO output, VBT field, FITS cube): its
    columns from the run's code units, as Gadget's ``UnitSystem`` gives its."""
    _, name = _pluto_split(path)
    run = merged_run_units(path)
    out: dict[str, ColumnScale] = {}
    quantity = variable_quantity(name) if name is not None else ""
    factor = _quantity_factor(run, quantity) if quantity else None
    if factor is not None and quantity != "tracer":
        out[VALUE] = {"factor": factor, "unit": _QUANTITY_UNIT[quantity], "source": run.source}
    # A grid shown in cell indices (polar, spherical, non-uniform) has no
    # coordinates to convert.
    index_grid = False
    if name is not None:
        from . import pluto_source  # noqa: PLC0415
        try:
            # A Cartesian view is made of coordinates, whatever its source grid.
            index_grid = (pluto_source.view_source(path) is None
                          and bool(pluto_source.resolve(path).grid.index_reason()))
        except (OSError, ValueError, AttributeError):
            index_grid = False
    if name is not None and run.length and not index_grid:
        for column in COORDINATES:
            out[column] = {"factor": run.length, "unit": "cm", "source": run.source}
    if run.time:
        out[TIME] = {"factor": run.time, "unit": "s", "source": run.source}
    return out


def with_typed(path: str | os.PathLike, table: dict[str, ColumnScale],
               columns: tuple[str, ...] | None = None) -> dict[str, ColumnScale]:
    """``table`` (a producer's columns) with what the user typed for the
    dataset ``path`` laid over it, each typed column marked ``"source": "user"``.
    ``columns`` limits the typed ones to those the dataset has (None: any, a
    volume's columns)."""
    typed = _load().get("columns", {})
    out = dict(table)
    own, shared = typed.get(dataset_key(path), {}), typed.get(_scope(path, "x"), {})
    for column in (columns if columns is not None else (VALUE,) + RUN_COLUMNS):
        entry = (shared if column in RUN_COLUMNS else own).get(column)
        if (isinstance(entry, dict) and isinstance(entry.get("factor"), (int, float))
                and math.isfinite(entry["factor"]) and entry["factor"] > 0):
            out[column] = {"factor": float(entry["factor"]), "unit": str(entry.get("unit", "")),
                           "source": "user"}
    return out


def scales(path: str | os.PathLike) -> dict[str, ColumnScale]:
    """Every column of the dataset ``path`` that can be shown in physical units,
    ``{column: {"factor", "unit"[, "source"]}}``: the one function every
    reader feeds and every consumer reads. A Gadget component answers from its
    unit system (``Component.physical()``), a RAMSES family or output
    directory from the ``unit_l``, ``unit_d`` and ``unit_t`` of its info file
    (``RamsesOutput.unit_table``), a PLUTO or VBT volume from the run's code
    units; what the user typed replaces any of them."""
    if _ramses_split(path)[1] is not None:
        from . import ramses_output  # noqa: PLC0415
        return ramses_output.resolve(path).physical()
    if os.path.isdir(path):
        from . import ramses_output  # noqa: PLC0415
        if ramses_output.output_of(path) is not None:
            table = ramses_output.open_output(path).unit_table()
            return with_typed(path, {c: {"factor": float(f), "unit": u}
                                     for c, (f, u) in table.items()}, columns=tuple(table))
    if _gadget_split(path)[1] is not None:
        from . import gadget_snapshot  # noqa: PLC0415
        return gadget_snapshot.resolve(path).physical()
    return with_typed(path, _volume_scales(path))


def column_scale(path: str | os.PathLike, column: str) -> ColumnScale | None:
    """One column of :func:`scales`; None when its unit is not known."""
    return scales(path).get(column)
