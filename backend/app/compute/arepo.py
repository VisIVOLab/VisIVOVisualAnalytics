"""AREPO snapshots — inspect the HDF5, and extract a uniform cube from it.

AREPO is a moving-mesh code: a snapshot is an unstructured set of cells, not a
grid, so nothing can display it directly. The legacy ViaLactea client shipped
``Utils/arepo-extractor.py``, which used yt's ``covering_grid`` to resample a
window of the simulation onto a regular grid and wrote it out as FITS. That is
the operation ported here — the resampling is yt's, and the parts that were
wrong are not.

What was wrong, and is fixed:

* The FITS header described a **hardcoded** 25 × 25 × 5 kpc box (``sizexy =
  25000``, ``sizez = 5000``) whatever window was actually extracted, so every
  cube except the default carried a WCS that did not match its own data. Here
  the header is built from the window and level that were used.
* ``CRPIX`` was 0 on all three axes. FITS pixel coordinates are 1-based, so
  that names a pixel outside the array, and with ``CRVAL = -size/2`` the whole
  grid was off by one cell. The reference pixel is now the centre of the array.
* The unit of the data was dropped (``BUNIT`` was commented out) even though yt
  knows it. It is carried through.
* A dozen keywords from some other tool's header (``LATPOLE``, ``ABS_X``,
  ``LOS_Z``, ``XCENTER``…) are gone; the provenance of the extraction is
  recorded instead.

``yt`` is an optional dependency: inspecting a snapshot needs only h5py, which
the backend already has, so the browse-and-plan half of the feature works
everywhere and only the extraction itself requires the extra package.
"""
from __future__ import annotations

import math
import os
import tempfile
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

import numpy as np

KPC_IN_CM = 3.085677581e21
PC_PER_KPC = 1000.0

# A covering grid is dense: every cell is materialised. 512³ float64 is 1 GiB,
# which is where the legacy's own warning started ("level 9+ … about 1+ GB").
_MAX_CELLS = int(os.environ.get("VISIVO_AREPO_MAX_CELLS", str(600_000_000)))
_MAX_LEVEL = int(os.environ.get("VISIVO_AREPO_MAX_LEVEL", "14"))
# Attribute values longer than this are summarised rather than returned whole.
_MAX_ATTR_ITEMS = 32


class ArepoError(ValueError):
    """Invalid AREPO request (bad level, window, field, oversized grid…)."""


# ── inspection ────────────────────────────────────────────────────────────────

@dataclass
class ArepoNode:
    """One entry of the snapshot's HDF5 tree."""

    name: str
    kind: str                       # "group" | "dataset"
    shape: list[int] = field(default_factory=list)
    dtype: str = ""
    attributes: dict = field(default_factory=dict)
    children: list["ArepoNode"] = field(default_factory=list)

    def as_dict(self) -> dict:
        return {
            "name": self.name,
            "kind": self.kind,
            "shape": self.shape,
            "dtype": self.dtype,
            "attributes": self.attributes,
            "children": [child.as_dict() for child in self.children],
        }


def _attribute_value(value):
    """An HDF5 attribute as something JSON can carry.

    JSON has no NaN and no Infinity, and h5py hands back numpy scalars, byte
    strings, structured records and object references as readily as plain
    numbers. Anything that survives to the response has to be one of the types
    ``json.dumps`` accepts, or the whole reply fails to serialise after the work
    is done.
    """
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    if isinstance(value, np.ndarray):
        flat = value.ravel()
        if flat.size > _MAX_ATTR_ITEMS:
            return f"<{value.dtype} array, shape {list(value.shape)}>"
        if value.dtype.names:          # a structured/compound array
            return [_attribute_value(item) for item in flat]
        return [_attribute_value(item) for item in flat.tolist()]
    if isinstance(value, np.void):     # one compound record
        return [_attribute_value(item) for item in value.tolist()]
    if isinstance(value, (list, tuple)):
        return [_attribute_value(item) for item in value]
    if isinstance(value, (np.bool_, bool)):
        return bool(value)
    if isinstance(value, np.integer):
        return int(value)
    if isinstance(value, (np.floating, float)):
        out = float(value)
        return out if np.isfinite(out) else None
    if isinstance(value, (int, str)) or value is None:
        return value
    return repr(value)


# An HDF5 file can name the same object twice, or link to itself; and a big
# snapshot has thousands of datasets. The walk is bounded on all three counts.
_MAX_TREE_NODES = 5000
_MAX_TREE_DEPTH = 24


def _local_group(parent, name):
    """``parent[name]`` — but only when it lives in THIS file.

    An external link points into another file, which the request was never
    authorised to read; the tree walk already refuses to follow them, and the
    places that reach for a group by name have to refuse too.
    """
    try:
        import h5py  # noqa: PLC0415

        link = parent.get(name, getlink=True)
        if link is None or isinstance(link, h5py.ExternalLink):
            return None
        return parent.get(name)
    except (KeyError, OSError, RuntimeError):
        return None


def _object_identity(obj):
    """A value that is equal for two names of the same HDF5 object.

    ``obj.id.id`` is the open handle and differs between aliases; the object
    header address does not.
    """
    try:
        import h5py  # noqa: PLC0415

        info = h5py.h5o.get_info(obj.id)
        return (getattr(info, "fileno", 0), info.addr)
    except Exception:   # noqa: BLE001 — identity is an optimisation, not a contract
        return None


class _TreeBudget:
    """Shared limits for one walk of a file's tree."""

    def __init__(self) -> None:
        self.nodes = 0
        self.truncated = False

    def take(self) -> bool:
        self.nodes += 1
        if self.nodes > _MAX_TREE_NODES:
            self.truncated = True
            return False
        return True


def _read_node(name: str, obj, budget: "_TreeBudget", visited: set, depth: int = 0) -> ArepoNode:
    import h5py  # noqa: PLC0415

    attributes = {key: _attribute_value(value) for key, value in obj.attrs.items()}
    if isinstance(obj, h5py.Dataset):
        return ArepoNode(name=name, kind="dataset", shape=list(obj.shape),
                         dtype=str(obj.dtype), attributes=attributes)

    node = ArepoNode(name=name, kind="group", attributes=attributes)
    if depth >= _MAX_TREE_DEPTH:
        node.attributes["…"] = "tree deeper than this listing goes"
        budget.truncated = True
        return node

    # An HDF5 file may hard-link a group into itself, and following that is an
    # infinite walk (reproduced: RecursionError). The identity has to be the
    # object's ADDRESS in the file — obj.id.id is the handle, which differs
    # between two aliases of the same group — and it is tracked as an ANCESTOR
    # set, removed on the way out, so a group legitimately linked in two places
    # is still listed under both.
    identity = _object_identity(obj)
    if identity is not None:
        if identity in visited:
            node.attributes["…"] = "already an ancestor of this node (link cycle)"
            return node
        visited.add(identity)

    for key in obj:
        if not budget.take():
            node.attributes["…"] = "listing truncated"
            break
        try:
            # getlink first: an EXTERNAL link points into another file, which
            # this request was never authorised to read. It is named, not opened.
            link = obj.get(key, getlink=True)
            if isinstance(link, h5py.ExternalLink):
                node.children.append(ArepoNode(
                    name=key, kind="group",
                    attributes={"external link": f"{link.filename}:{link.path} (not followed)"}))
                continue
            node.children.append(_read_node(key, obj[key], budget, visited, depth + 1))
        except (KeyError, OSError, RuntimeError):
            # A broken link is a fact about the file, not a reason to refuse it.
            node.children.append(ArepoNode(name=key, kind="group",
                                           attributes={"error": "unreadable"}))
    if identity is not None:
        visited.discard(identity)
    return node


def inspect_snapshot(path: str) -> dict:
    """Structure, box size and available fields of an AREPO snapshot.

    Needs only h5py: browsing a snapshot and planning an extraction must work
    on a deployment that has not installed yt.
    """
    import h5py  # noqa: PLC0415

    try:
        handle = h5py.File(path, "r")
    except OSError as exc:
        raise ArepoError(f"Not a readable HDF5 file: {exc}") from exc

    with handle as snapshot:
        budget = _TreeBudget()
        root = _read_node("/", snapshot, budget, set())

        box_size_kpc = None
        unit_length_cm = None
        for group in (_local_group(snapshot, "Parameters"), _local_group(snapshot, "Header")):
            if group is None:
                continue
            if unit_length_cm is None and "UnitLength_in_cm" in group.attrs:
                unit_length_cm = float(np.ravel(group.attrs["UnitLength_in_cm"])[0])
            if box_size_kpc is None and "BoxSize" in group.attrs:
                box_size_kpc = float(np.ravel(group.attrs["BoxSize"])[0])
        if box_size_kpc is not None and unit_length_cm:
            box_size_kpc = box_size_kpc * unit_length_cm / KPC_IN_CM

        # Particle types and their per-particle fields, straight from the file:
        # yt would give prettier names, and is not required to be installed.
        particles: dict[str, list[str]] = {}
        for key in snapshot:
            if not key.startswith("PartType"):
                continue
            group = _local_group(snapshot, key)
            if isinstance(group, h5py.Group):
                particles[key] = sorted(group.keys())

    return {
        "path": path,
        "tree": root.as_dict(),
        "box_size_kpc": box_size_kpc,
        "unit_length_cm": unit_length_cm,
        "particle_types": particles,
        "tree_truncated": budget.truncated,
        "yt_available": yt_available(),
    }


def domain_width_kpc(path: str) -> tuple[float, float, float]:
    """Side of the simulation box, in kpc, from the snapshot's own header.

    Read with h5py rather than through yt: the *planning* half of the feature
    has to work on a backend that cannot run the extraction, and this is the one
    number a plan needs from the file.
    """
    import h5py  # noqa: PLC0415

    try:
        handle = h5py.File(path, "r")
    except OSError as exc:
        raise ArepoError(f"Not a readable HDF5 file: {exc}") from exc

    with handle as snapshot:
        box = None
        unit_length_cm = None
        for name in ("Parameters", "Header"):
            group = _local_group(snapshot, name)
            if group is None:
                continue
            if box is None and "BoxSize" in group.attrs:
                box = float(np.ravel(group.attrs["BoxSize"])[0])
            if unit_length_cm is None and "UnitLength_in_cm" in group.attrs:
                unit_length_cm = float(np.ravel(group.attrs["UnitLength_in_cm"])[0])

    if box is None or not unit_length_cm:
        raise ArepoError(
            "This snapshot does not declare BoxSize and UnitLength_in_cm, so the "
            "extraction window cannot be placed in physical units.")
    side = box * unit_length_cm / KPC_IN_CM
    if not np.isfinite(side) or side <= 0:
        raise ArepoError("The snapshot declares a box with no size.")
    # AREPO's box is cubic; yt would report the same three numbers.
    return (side, side, side)


def yt_available() -> bool:
    """Whether the extraction half of the feature can run here."""
    try:
        import yt  # noqa: F401,PLC0415
    except Exception:   # noqa: BLE001 — yt fails to import in many ways
        return False
    return True


# ── planning ──────────────────────────────────────────────────────────────────

@dataclass
class ExtractionPlan:
    """What an extraction would produce, worked out without touching yt."""

    shape: tuple[int, int, int]
    cell_size_pc: tuple[float, float, float]
    #: What the caller asked for…
    window_kpc: tuple[float, float, float]
    #: …and what it will actually get. A covering grid's cell size is fixed by
    #: the refinement level, so the extracted extent is the requested window
    #: rounded to a whole number of cells. They differ, and both are reported.
    extent_kpc: tuple[float, float, float]
    offset_kpc: tuple[float, float, float]
    level: int
    base_dimensions: tuple[int, int, int]
    cells: int
    bytes_estimate: int

    def as_dict(self) -> dict:
        return {
            "shape": list(self.shape),
            "cell_size_pc": list(self.cell_size_pc),
            "window_kpc": list(self.window_kpc),
            "extent_kpc": list(self.extent_kpc),
            "offset_kpc": list(self.offset_kpc),
            "level": self.level,
            "base_dimensions": list(self.base_dimensions),
            "cells": self.cells,
            "bytes_estimate": self.bytes_estimate,
        }


def _triplet(values, name: str) -> tuple[float, float, float]:
    try:
        out = tuple(float(v) for v in values)
    except (TypeError, ValueError) as exc:
        raise ArepoError(f"{name} must be three numbers") from exc
    if len(out) != 3:
        raise ArepoError(f"{name} must be three numbers")
    if not all(np.isfinite(v) for v in out):
        raise ArepoError(f"{name} must be finite")
    return out


def plan_extraction(level: int, window_kpc, offset_kpc, domain_width_kpc,
                    base_dimensions=(1, 1, 1)) -> ExtractionPlan:
    """Grid an extraction would produce, and whether it is a sane thing to ask.

    Separated from the extraction itself so the client can show the resulting
    cube size *before* committing to a job that materialises it — the legacy
    asked "level 9+ may take minutes and produce 1+ GB, continue?" from a
    hardcoded threshold, which told the user nothing about their own window.

    The arithmetic is yt's, not ours. A ``covering_grid`` cell is
    ``domain_width / (base_dimensions · 2^level)``: the level fixes the
    resolution, and ``dims`` only says how many of those cells to take. The
    legacy script, and this module's first version, instead divided the window
    by the rounded cell count — which quietly disagreed with the grid yt
    actually built (0.769 kpc against 0.781 kpc on a 100 kpc box at level 7),
    so the WCS did not describe the data.
    """
    level = int(level)
    if level < 0:
        raise ArepoError("level must be zero or more")
    if level > _MAX_LEVEL:
        raise ArepoError(f"level {level} is beyond the limit of {_MAX_LEVEL}")

    window = _triplet(window_kpc, "window")
    offset = _triplet(offset_kpc, "offset")
    domain = _triplet(domain_width_kpc, "domain width")
    base = tuple(max(1, int(v)) for v in base_dimensions)
    if len(base) != 3:
        raise ArepoError("base dimensions must be three integers")
    if any(v <= 0 for v in window):
        raise ArepoError("every window side must be greater than zero")
    if any(v <= 0 for v in domain):
        raise ArepoError("the simulation domain has no size")
    if any(w > d for w, d in zip(window, domain)):
        raise ArepoError(
            "the window is larger than the simulation box "
            f"({window[0]:g} × {window[1]:g} × {window[2]:g} kpc against "
            f"{domain[0]:g} × {domain[1]:g} × {domain[2]:g} kpc)")

    cell_kpc = tuple(full / (b * 2**level) for full, b in zip(domain, base))
    shape = tuple(max(1, int(round(side / dx))) for side, dx in zip(window, cell_kpc))
    extent = tuple(n * dx for n, dx in zip(shape, cell_kpc))
    cell_size_pc = tuple(dx * PC_PER_KPC for dx in cell_kpc)

    # Python ints, via math.prod: numpy would overflow int64 on an absurd
    # request and report zero cells, which sails straight through the limit.
    cells = math.prod(int(n) for n in shape)
    if cells > _MAX_CELLS:
        raise ArepoError(
            f"that window and level need {cells:,} cells "
            f"({cells * 8 / 1024**3:.1f} GiB per field); the limit is {_MAX_CELLS:,}")

    return ExtractionPlan(
        shape=(shape[0], shape[1], shape[2]),
        cell_size_pc=cell_size_pc,
        window_kpc=window,
        extent_kpc=extent,
        offset_kpc=offset,
        level=level,
        base_dimensions=(base[0], base[1], base[2]),
        cells=cells,
        bytes_estimate=cells * 8,
    )


# ── extraction ────────────────────────────────────────────────────────────────

def build_header(plan: ExtractionPlan, *, source: str, particle: str, field_name: str,
                 bunit: str):
    """The FITS header of an extracted cube: a real WCS, and its provenance.

    Axes are linear physical coordinates in parsec, centred on the window. The
    reference pixel is the centre of the array in 1-based FITS convention —
    ``(N + 1) / 2``, which is a half-integer for an even N and exactly right.
    """
    from astropy.io import fits  # noqa: PLC0415

    header = fits.Header()
    header["WCSAXES"] = 3
    for axis in (1, 2, 3):
        n = plan.shape[axis - 1]
        header[f"CTYPE{axis}"] = ("XYZ"[axis - 1], "linear physical coordinate")
        header[f"CUNIT{axis}"] = "pc"
        header[f"CRPIX{axis}"] = (n + 1) / 2.0
        header[f"CRVAL{axis}"] = 0.0
        header[f"CDELT{axis}"] = plan.cell_size_pc[axis - 1]
    if bunit:
        header["BUNIT"] = bunit

    header["CREATOR"] = "VisIVO (yt covering_grid)"
    header["DATE"] = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S")
    header["AREPOSRC"] = (Path(source).name, "AREPO snapshot this was extracted from")
    header["AREPOLVL"] = (plan.level, "yt refinement level of the covering grid")
    header["AREPOPRT"] = (particle, "particle type")
    header["AREPOFLD"] = (field_name, "extracted field")
    for axis, name in enumerate("XYZ"):
        header[f"AREPOW{name}"] = (plan.extent_kpc[axis],
                                   f"extracted extent along {name} (kpc)")
        header[f"AREPOR{name}"] = (plan.window_kpc[axis],
                                   f"requested window along {name} (kpc)")
        header[f"AREPOO{name}"] = (plan.offset_kpc[axis],
                                   f"window centre offset from the domain centre along {name} (kpc)")
    return header


def worker_arepo_extract(path: str, level: int, particle: str, fields: list[str],
                         window_kpc, offset_kpc, out_dir: str) -> dict:
    """Resample a window of an AREPO snapshot onto a uniform grid, per field.

    Returns ``{plan, outputs:[{field, path, …}], domain_width_kpc}``.
    """
    try:
        import yt  # noqa: PLC0415
    except Exception as exc:   # noqa: BLE001
        raise ImportError(
            "Extracting an AREPO snapshot needs the 'yt' package, which is not installed "
            "on this backend. Install it with: pip install yt"
        ) from exc

    if not fields:
        raise ArepoError("name at least one field to extract")
    particle = (particle or "gas").strip()

    yt.set_log_level("error")
    dataset = yt.load(path)

    if getattr(dataset, "cosmological_simulation", 0):
        # Comoving coordinates bring in the scale factor and h, and this code
        # places the window in proper kpc. Refusing is the honest answer; a
        # silent factor of (1+z)/h in the extracted region is not.
        raise ArepoError(
            "This is a cosmological snapshot (comoving coordinates). Extraction in "
            "physical kpc is not implemented for those yet.")

    domain_width = tuple(float(v) for v in dataset.domain_width.in_units("kpc").v)
    base_dimensions = tuple(int(v) for v in dataset.domain_dimensions)
    plan = plan_extraction(level, window_kpc, offset_kpc, domain_width, base_dimensions)

    # Left corner of the window. The extracted extent is the request rounded to
    # whole cells, so the window is re-centred on THAT — otherwise the rounding
    # would shift the centre the user asked for.
    centre_kpc = dataset.domain_center.in_units("kpc").v + np.asarray(plan.offset_kpc)
    left_kpc = centre_kpc - 0.5 * np.asarray(plan.extent_kpc)
    # yt's own conversion, not BoxSize × UnitLength_in_cm by hand: the frontend
    # knows the unit system, including whatever the snapshot does with h.
    left_edge = dataset.arr(left_kpc, "kpc").to("code_length")

    right_edge = dataset.arr(left_kpc + np.asarray(plan.extent_kpc), "kpc").to("code_length")
    # arbitrary_grid, not covering_grid: covering_grid derives its own cell size
    # from the level and sets rdx = 1 on any axis with a single cell, which
    # silently turns a 0.5 kpc slab into the whole 100 kpc box. Passing both
    # edges makes the sampled region exactly the one the header describes.
    if not hasattr(dataset, "arbitrary_grid"):
        raise ArepoError(
            "This yt version has no arbitrary_grid; the extracted region could not be "
            "pinned to the requested bounds. Upgrade yt (3.4 or newer).")
    grid = dataset.arbitrary_grid(left_edge, right_edge, dims=list(plan.shape))

    from astropy.io import fits  # noqa: PLC0415

    # One directory per extraction, created atomically: the same snapshot, level
    # and field with a different window used to land on the same filename and
    # overwrite it. A timestamp plus a hash is not unique either — two identical
    # requests in the same second collide, and hash() is salted per process.
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S")
    Path(out_dir).mkdir(parents=True, exist_ok=True)
    out_root = Path(tempfile.mkdtemp(prefix=f"{_safe_name(Path(path).stem)}-{stamp}-",
                                     dir=out_dir))

    outputs = []
    for name in fields:
        key = (particle, name)
        try:
            data = grid[key]
        except Exception as exc:   # noqa: BLE001 — yt raises several field errors
            raise ArepoError(
                f"field ('{particle}', '{name}') is not in this snapshot: {exc}") from exc

        values = np.asarray(getattr(data, "value", data))
        if values.ndim != 3:
            # A vector field comes back as (Nx, Ny, Nz, 3); transposing that
            # would fold the component axis into the WCS.
            raise ArepoError(
                f"field ('{particle}', '{name}') is not scalar (shape {list(values.shape)}); "
                "extract its components separately.")

        bunit = str(getattr(data, "units", "")) or ""
        header = build_header(plan, source=path, particle=particle, field_name=name, bunit=bunit)
        out_path = out_root / f"{_safe_name(name)}.fits"
        if out_path.exists():
            # "Density" and "density." sanitise to the same file name.
            raise ArepoError(
                f"two requested fields both write to {out_path.name}; rename or "
                "extract them separately")
        # FITS is written in (z, y, x) order: NAXIS1 is the fastest axis.
        fits.PrimaryHDU(data=np.ascontiguousarray(values.astype(np.float32).T),
                        header=header).writeto(out_path, overwrite=False)
        outputs.append({
            "field": name,
            "particle": particle,
            "path": str(out_path),
            "bunit": bunit,
            "shape": list(plan.shape),
        })

    return {"plan": plan.as_dict(), "outputs": outputs,
            "domain_width_kpc": list(domain_width)}


def _safe_name(raw: str) -> str:
    """A field or file name reduced to something safe to put in a path."""
    keep = [c if (c.isalnum() or c in "-_.") else "_" for c in str(raw)]
    return "".join(keep).strip("._") or "field"
