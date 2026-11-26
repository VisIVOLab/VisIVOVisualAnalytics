"""What is this file? — a cheap probe, so the user does not have to know.

The Data Hub used to ask the question the other way round: *Open Remote
Dataset…* for FITS images and cubes, *Open 3D Catalogue…* for CSV / VOTable /
FITS **tables**, *Open VBT…* for a `.bin` + `.head` pair. Three entries, and
two of them accept `.fits`, so opening a file meant knowing in advance which
kind of file it was and picking the matching menu item — a split that described
this backend's three open endpoints, not anybody's work.

This module answers "what is it" from the file itself, so there can be one
*Open…*. It is deliberately cheap: magic bytes, an extension, and at most one
FITS header — no conversion, no session, no data read. When it cannot tell, it
says so (``confident: false``) and the client asks rather than guessing.
"""
from __future__ import annotations

import logging
from pathlib import Path

from .text_table import SNIFF_BYTES, sniff_separator

logger = logging.getLogger("visivo.backend")

#: How much of a text file to look at when deciding whether it is a table:
#: the readers judge the separator on the same bytes (app.text_table).
_SNIFF_BYTES = SNIFF_BYTES

#: What the client should do with the file.
#:   "dataset"     → POST /v1/datasets/open  (image, cube, dynamic spectrum)
#:   "catalogue"   → POST /v1/catalogue/open (CSV, VOTable, IPAC, speck, FITS table)
#:   "vbt"         → POST /v1/vbt/open
#:   "velocity"    → POST /v1/velocity/open (a 3-component vector field)
#:   "ask"         → the content is ambiguous or unknown; let the user choose
#:   "unsupported" → we know what it is and no opener takes it; say so, don't ask
OPENERS = ("dataset", "catalogue", "vbt", "velocity", "ask", "unsupported")


def _classify_vbt(head: Path, reason: str, open_path: str = "") -> dict:
    from .volume_source import parse_vbt_header  # noqa: PLC0415
    try:
        volume = parse_vbt_header(head)["kind"] == "volume"
    except (OSError, ValueError):
        volume = False   # the VBT opener reports what is wrong with it
    if volume:
        return _result("cube", "dataset", True,
                       "VisIVO Binary Table volume: each field opens as a cube.",
                       open_path=open_path)
    # A table of points is a cartesian catalogue (issue #83): the 3-D
    # catalogue viewer, read in place (dependencies._read_vbt_points).
    return _result("vbt", "catalogue", True, reason, open_path=open_path)


def _classify_pluto(path: Path) -> dict | None:
    """The verdict on a PLUTO output, or None for a file that is not one: a
    ``dbl.out`` / ``flt.out``, or a ``<var>.NNNN.dbl|flt`` with one beside it.
    ``open_path`` is the reference of the variable to open: the file's own,
    or, for the ``.out``, the newest output's first variable present."""
    from . import pluto_source as ps  # noqa: PLC0415

    is_out = path.name in ps.OUT_NAMES
    if not is_out and not (path.suffix.lower() in ps.EXTENSIONS and ps.is_pluto(path)):
        return None
    if not (path.parent / ps.GRID_NAME).is_file():
        return _result("pluto", "unsupported", True,
                       f"PLUTO output, but the {ps.GRID_NAME} that gives its grid is not "
                       "beside it: copy it next to the data to open it.")
    ref = ps.pluto_dataset_path(path)
    if ref is None:
        what = ("none of the newest output's variables is beside it, or its files do "
                "not match the grid") if is_out else \
            f"the file does not match {ps.GRID_NAME} and its .out file"
        return _result("pluto", "unsupported", True,
                       f"PLUTO output that cannot be read: {what}.")
    var = ps.resolve(ref)
    axes = [n for n in var.shape[::-1] if n > 1]
    if len(axes) < 2:
        return _result("pluto", "unsupported", True,
                       f"PLUTO output of a 1-D run ({var.grid.cells} cells): no image or "
                       "cube to show.")
    count = len(ps.pluto_fields(ref))
    more = f", {count - 1} more variable(s) as fields" if count > 1 else ""
    detail = (f"PLUTO output {var.record.index} ({' × '.join(str(n) for n in axes)}, "
              f"{var.grid.geometry.lower()}): {var.name}{more}.")
    notice = var.grid.index_reason()
    if notice:
        detail += f" Shown on cell indices ({notice})."
    return _result("cube" if len(axes) == 3 else "image", "dataset", True, detail,
                   open_path=ref)


def _classify_raw(path: Path, allowed=None) -> dict | None:
    """The verdict on a legacy-importer descriptor (``rawPointsDesc``,
    ``rawGridsDesc``, ``flyDesc``) or a bare ``.fly`` file (issue #551); None
    for any other file. The first file of the list is the one to open
    (``open_path``, a reference); the others are its series. A bare FLY file
    whose type, byte order and size the file cannot tell is ``ask``: the
    importer takes those as options, and here they are written in a
    descriptor."""
    from . import raw_descriptor as rd  # noqa: PLC0415

    kind = rd.sniff(path)
    if kind is None:
        return None
    grid = kind == "rawgrids"
    label = {"rawpoints": "raw points", "rawgrids": "raw grid", "fly": "FLY",
             "bare-fly": "FLY"}[kind]
    try:
        ref = (rd.grid_dataset_path if grid else rd.points_dataset_path)(path, allowed)
        rf = rd.open_file(ref)
    except PermissionError:
        raise
    except (OSError, ValueError) as exc:
        if kind == "bare-fly" and isinstance(exc, rd.RawFormatError):
            return _result("fly", "ask", False, str(exc))
        return _result("grid" if grid else "particles", "unsupported", True,
                       f"{label} descriptor that cannot be read: {exc}")
    d = rf.desc
    steps = len(d.entries)
    series = f", {steps} files as a series" if steps > 1 else ""
    kind_of = f"{'float64' if d.dtype.itemsize == 8 else 'float32'}, {d.endian} endian"
    if grid:
        axes = [n for n in rf.shape[::-1] if n > 1]
        if len(axes) < 2:
            return _result("grid", "unsupported", True,
                           f"Raw grid of one dimension ({int(max(rf.shape))} cells): no image "
                           "or cube to show.")
        return _result("cube" if len(axes) == 3 else "image", "dataset", True,
                       f"Raw grid {d.name} ({' × '.join(str(n) for n in axes)}, {kind_of}"
                       f"{series}): read in place; each listed file is one dataset.",
                       open_path=ref)
    what = "FLY file with no descriptor (type and byte order read from its values)" \
        if d.bare else f"{label} descriptor"
    scalars = f", {rf.scalars} scalar field(s)" if rf.kind == "rawpoints" else ", velocities"
    return _result("particles", "catalogue", True,
                   f"{what}: {d.rows} points{scalars} ({kind_of}{series}). Each listed file "
                   "opens as a table of points.", open_path=ref)


def _classify_hdf5(path: Path) -> dict | None:
    """The verdict on a generic HDF5 file (issue #552), from its layout alone:
    one thing that opens is opened; several are offered as ``components`` (the
    client asks which, and classifies the reference it is given). None for a
    file that is a dynamic spectrum or cannot be read as HDF5 (the legacy
    answer stands)."""
    from . import hdf5_source as h5s  # noqa: PLC0415

    layout = h5s.generic(path)
    if layout is None:
        return None
    found = layout.candidates
    if not found:
        return _result("hdf5", "unsupported", True,
                       "HDF5 with no table (1-D datasets of one length in a group, or a "
                       "compound dataset) and no 2-D or 3-D numeric dataset to show.")

    def ref(c) -> str:
        return h5s.make_ref(str(path), "table" if c.kind == "table" else "volume", c.path)

    if len(found) == 1:
        c = found[0]
        if c.kind == "table":
            return _result("catalogue", "catalogue", True,
                           f"HDF5 table {c.path}: {c.shape[0]} rows, {len(c.columns)} columns "
                           "(read in place).", open_path=ref(c))
        return _result(c.kind, "dataset", True,
                       f"HDF5 {c.kind} {c.label.strip()} (read in place).", open_path=ref(c))
    components = [{"name": c.label, "path": ref(c), "num_rows": c.rows} for c in found]
    return _result("hdf5", "ask", False,
                   f"HDF5 file with {len(found)} datasets that can open; pick one. A hyperslab "
                   f"can follow the name: {h5s.VOLUME_SEP}/group/dataset[offset:count,...].",
                   open_path=components[0]["path"], components=components)


def classify_path(path: str | Path, allowed=None) -> dict:
    """``{kind, opener, confident, detail, open_path}`` for one path.

    ``kind`` names the content ("image", "cube", "dynspec", "fits-table",
    "catalogue", "vbt", "pluto" (an output that cannot be opened), "particles"
    (a particle snapshot: Gadget, or a RAMSES output), "amr" (a RAMSES output
    with a mesh and no particles), "unknown"); ``opener`` names the route that
    opens it.
    ``components``: a snapshot's particle types, each with its own path.
    ``open_path`` is the path the opener should be given when it differs from
    the one asked about — a VBT selected by its ``.bin`` opens by its ``.head``
    sidecar, which is the file that actually describes the table.
    """
    path = Path(path)
    if path.is_dir():
        # A snapdir: the snapshot it holds (issue #31). One whose files
        # reach outside the data roots is shown as a plain directory.
        try:
            particles = _classify_ramses(path, allowed) or _classify_particles(path, allowed)
        except PermissionError:
            particles = None
        if particles is not None:
            return particles
        return _result("directory", "ask", False, "This is a directory.")
    try:
        size = path.stat().st_size
    except OSError as exc:
        return _result("unknown", "ask", False, f"The file cannot be read: {exc}")
    if size == 0:
        return _result("unknown", "ask", False, "The file is empty.")

    suffix = path.suffix.lower()
    stem_suffixes = "".join(path.suffixes).lower()

    # ── VBT: a header file, or a .bin with a .head beside it ──────────────────
    # A volume opens as a cube, read in place, as a FITS cube is (issue #83);
    # a table of points in the 3-D catalogue viewer.
    if suffix == ".head" or stem_suffixes.endswith(".bin.head"):
        return _classify_vbt(path, "VisIVO Binary Table header.")
    if suffix == ".bin" and path.with_suffix(".bin.head").exists():
        # The opener parses the header, not the binary: hand it the sidecar.
        return _classify_vbt(path.with_suffix(".bin.head"),
                             "VisIVO Binary Table (header found beside it).",
                             open_path=str(path.with_suffix(".bin.head")))

    # ── PLUTO: a data file, or the dbl.out / flt.out listing the outputs ─────
    # Read in place as a cube (an image for a 2-D run), one variable at a time
    # (issue #315).
    pluto = _classify_pluto(path)
    if pluto is not None:
        return pluto

    # ── A RAMSES output: any file of its directory (info, amr, part, ...) ────
    ramses = _classify_ramses(path, allowed)
    if ramses is not None:
        return ramses

    # ── The legacy importer's raw descriptors and a bare FLY file ────────────
    # Read in place (issue #551): points as a table, a grid as a cube or image.
    raw = _classify_raw(path, allowed)
    if raw is not None:
        return raw

    # ── A particle snapshot (Gadget binary or HDF5), by its header record ────
    # Read in place as a table of points, one particle type at a time (#31).
    particles = _classify_particles(path, allowed)
    if particles is not None:
        return particles

    if suffix == ".speck":
        return _result("catalogue", "catalogue", True, "speck point catalogue.")

    with open(path, "rb") as handle:
        head = handle.read(_SNIFF_BYTES)

    # ── HDF5: a table, a volume or an image of any file that is not a Gadget
    # snapshot (above) or a beamformed dynamic spectrum, which comes this way ─
    if head.startswith(b"\x89HDF\r\n\x1a\n"):
        generic = _classify_hdf5(path)
        if generic is not None:
            return generic
        return _result("dynspec", "dataset", True,
                       "HDF5 — opened as a dynamic spectrum.")

    # ── FITS ─────────────────────────────────────────────────────────────────
    if head.startswith(b"SIMPLE"):
        verdict = _classify_fits(path, head)
        if verdict["opener"] == "dataset" and _image_is_tile_compressed(path):
            verdict["detail"] += (" Tile-compressed: decompressed into the workspace "
                                  "cache when opened.")
        return verdict
    if head[:2] == b"\x1f\x8b" or stem_suffixes.endswith((".fits.gz", ".fit.gz", ".fts.gz")):
        # A gzipped FITS opens from its decompressed copy (issue #57): classified
        # by what it holds, read through the gzip stream (one header, at its
        # start). Anything else gzipped (a CSV, a VOTable) no opener takes.
        if _gunzip_head(path).startswith(b"SIMPLE"):
            verdict = _classify_fits(path, head, gzipped=True)
            if verdict["opener"] in ("dataset", "velocity"):
                verdict["detail"] += (" gzip-compressed: decompressed once into the "
                                      "workspace cache when opened.")
            # A table is read whole by the catalogue opener, which reads gzip.
            return verdict
        return _result("compressed", "unsupported", True,
                       "Compressed file. Decompress it first — the backend opens "
                       "FITS (also .fits.gz and .fz), HDF5, catalogues and VBT.")

    # ── Text tables ──────────────────────────────────────────────────────────
    text = head.decode("utf-8", errors="replace").lstrip()
    if text.startswith("<?xml") or "<VOTABLE" in text.upper():
        return _result("catalogue", "catalogue", True, "VOTable.")
    if text.startswith(("|", "\\")):
        return _result("catalogue", "catalogue", True, "IPAC table.")
    # Judged on the text as read, not stripped: a leading tab is an empty
    # first field (a pandas index column), and the readers see it too.
    if _looks_like_delimited_text(head.decode("utf-8", errors="replace"),
                                  truncated=size > _SNIFF_BYTES):
        return _result("catalogue", "catalogue", True,
                       "Delimited text — opened as a catalogue.")

    return _result("unknown", "ask", False,
                   "The content does not identify itself; choose how to open it.")


def _gunzip_head(path: Path) -> bytes:
    """The first bytes of what a gzip file holds; empty when it is unreadable."""
    import gzip  # noqa: PLC0415
    import zlib  # noqa: PLC0415
    try:
        with gzip.open(path, "rb") as stream:
            return stream.read(80)
    except (OSError, EOFError, zlib.error):
        return b""


def _image_is_tile_compressed(path: Path) -> bool:
    from .compressed_fits import _image_is_tile_compressed as tiled  # noqa: PLC0415
    return tiled(path)


def _classify_fits(path: Path, head: bytes, gzipped: bool = False) -> dict:
    """Image, cube or table — from the header alone, without reading data."""
    try:
        from astropy.io import fits  # noqa: PLC0415
    except ImportError:
        return _result("image", "dataset", False, "FITS (astropy unavailable to look closer).")

    try:
        if gzipped:
            # Read off the gzip stream: astropy would decompress all of it to
            # skip the image's data (#57). None: a stream this cannot read
            # (a gzipped fpack file, damage), which astropy is asked about.
            from .compressed_fits import gzip_image_headers  # noqa: PLC0415
            hdus = gzip_image_headers(path)
            if hdus is not None:
                return _classify_hdus(hdus)
        with fits.open(path, memmap=False, lazy_load_hdus=True) as hdul:
            return _classify_hdus(hdul)
    except Exception as exc:  # noqa: BLE001 — a malformed header is a fact, not a crash
        logger.info("[classify] %s: %s", path, exc)
        return _result("image", "dataset", False,
                       f"FITS, but its header could not be read ({exc}).")


def _classify_hdus(hdul) -> dict:
    """Image, cube, velocity field or table, from the headers of ``hdul``
    (an astropy HDUList, or the headers read off a gzip stream)."""
    # The same choice the opener makes: the primary when it holds an
    # image, else the first image extension (an ESO phase-3 or APEX
    # product keeps the primary empty and the cube in DATA_EXT), so a
    # file is never classified from one HDU and opened from another.
    from .fits_dataset import image_hdu_index  # noqa: PLC0415
    header = hdul[image_hdu_index(hdul)].header
    naxis = int(header.get("NAXIS", 0))
    # The real axes: FITS routinely carries degenerate ones (a Stokes
    # axis of length 1), and counting those would call a map a cube.
    axes = [int(header.get(f"NAXIS{i}", 0)) for i in range(1, naxis + 1)]
    significant = [n for n in axes if n > 1]

    if naxis == 0 or not significant:
        # No image anywhere: the content is a table, or nothing we read.
        for hdu in hdul[1:]:
            kind = str(hdu.header.get("XTENSION", "")).strip().upper()
            if kind in ("BINTABLE", "TABLE"):
                return _result(
                    "fits-table", "ask", True,
                    "FITS table — it can be a source catalogue, or a table "
                    "to overlay on an image.")
        return _result("unknown", "ask", False,
                       "FITS with no image and no table in it.")

    ctypes = [str(header.get(f"CTYPE{i}", "")).upper() for i in range(1, naxis + 1)]
    # A velocity field: three components (vx, vy, vz) on a 3-D grid, the
    # layout the vector-field viewer reads. A Stokes or spectral axis of
    # length 3 is something else, so only an axis that names neither.
    # The fourth axis must not name anything but a component: an unlabelled
    # axis (the CF4 reconstructions carry none) or one that says so; a TIME,
    # STOKES or spectral axis of length 3 is a cube of something else. The
    # three grid axes must not be spectral, Stokes or time either.
    from .fits_dataset import ScientificFitsDataset  # noqa: PLC0415
    not_grid = tuple(ScientificFitsDataset._SPECTRAL_CTYPE_PREFIXES) + (
        "STOKES", "TIME", "UTC", "MJD")
    fourth = ctypes[3].strip() if naxis == 4 else "?"
    fourth_is_component = fourth == "" or "COMP" in fourth or fourth in ("V", "VEC",
                                                                          "VECTOR")
    if (naxis == 4 and axes[3] == 3 and all(n > 1 for n in axes[:3])
            and fourth_is_component
            and not any(c.startswith(not_grid) for c in ctypes[:3])):
        return _result("velocity-field", "velocity", True,
                       f"Velocity field: 3 components on a "
                       f"{axes[0]} × {axes[1]} × {axes[2]} grid.")
    if any(c.startswith(("TIME", "UTC")) for c in ctypes) \
            and any(c.startswith("FREQ") for c in ctypes):
        return _result("dynspec", "dataset", True,
                       "FITS with time and frequency axes — a dynamic spectrum.")
    if len(significant) >= 3:
        return _result("cube", "dataset", True,
                       f"FITS cube ({' × '.join(str(n) for n in significant)}).")
    return _result("image", "dataset", True,
                   f"FITS image ({' × '.join(str(n) for n in significant)}).")


def _looks_like_delimited_text(text: str, truncated: bool = False) -> bool:
    """Whether this reads as a table: several lines with the same field count.

    One line with commas is a sentence; ten lines that parse into the same
    number of fields is a catalogue. The test is the catalogue readers' own
    (:func:`app.text_table.sniff_separator`), so a file called a catalogue here
    is read with the separator seen here (issue #155).

    The fields are PARSED, not counted as characters. A quoted field containing
    the separator — an object name with a comma, a contour written as
    "10.4_10.6,0.0_0.2" — makes the raw counts differ between the header and the
    rows, and a character count then calls a perfectly ordinary CSV unknown.
    """
    return sniff_separator(text, truncated) is not None


def _classify_ramses(path: Path, allowed=None) -> dict | None:
    """The verdict on a RAMSES output (issue #549): its directory, or any
    file in it (``info_*.txt``, ``amr_*.out*``, ``part_*.out*``, ...), judged
    by the info file's content. With particle files it opens as a table of
    points, one family at a time (``open_path`` the first, ``components``
    all); without, a mesh with gas opens as its gas cells (issue #550), and one
    without gas has no viewer. None for anything else."""
    from . import ramses_output as rm  # noqa: PLC0415
    from .point_table import particle_components, particle_dataset_path  # noqa: PLC0415

    directory = rm.checked_output(path, allowed)   # the data roots before any parse
    if directory is None:
        return None
    try:
        out = rm.open_output(directory)
        info = out.info
        found = particle_dataset_path(directory, allowed)   # the data-root check first
        mesh = ""
        if out.has("amr"):
            mesh = (f"a mesh of levels {info.levelmin}-{info.levelmax}"
                    + (f" with {len(out.hydro_names())} hydro variables" if out.has("hydro") else ""))
        if found is None:
            if not mesh:
                return _result("unknown", "unsupported", True,
                               f"RAMSES output {out.number} with no particle or mesh file.")
            return _result("amr", "unsupported", True,
                           f"RAMSES output {out.number} ({info.ncpu} files): {mesh} and no "
                           "particles; the mesh has no viewer yet.")
        components = particle_components(found[0])
    except PermissionError:
        raise
    except (OSError, ValueError, IndexError) as exc:
        return _result("particles", "unsupported", True,
                       f"RAMSES output that cannot be read: {exc}")
    counts = ", ".join(f"{c['num_rows']} {c['name']}" for c in components)
    z = f" at z = {info.redshift:.3g}" if info.cosmological else ""
    detail = (f"RAMSES output {out.number} ({info.ncpu} files){z}: {counts}. "
              "Each particle family, and the gas cells of the mesh, opens as a table of points.")
    if mesh:
        detail += (f" It holds {mesh}: the cells' sizes are their smoothing lengths, and a "
                   "uniform cube of any field can be resampled from them.")
    return _result("particles", "catalogue", True, detail, open_path=found[0],
                   components=components)


def _classify_particles(path: Path, allowed=None) -> dict | None:
    """The verdict on a particle snapshot (a Gadget file, issue #31, or a
    folder of one), by its header record, not its name; None for anything
    else. It opens as a table of points, one particle type at a time:
    ``open_path`` is the first type with particles, ``components`` all of
    them."""
    from . import gadget_snapshot as gs  # noqa: PLC0415
    from .point_table import particle_components, particle_dataset_path  # noqa: PLC0415

    catalogue = None
    if path.is_dir():
        first = gs.first_in_dir(path, allowed)
        if first is None:
            return None
        catalogue = gs.catalogue_in_dir(path, first, allowed)
        path = first
    elif gs.detect(path) is None:
        return None
    try:
        if allowed is not None:
            validated = particle_dataset_path(path, allowed)
            if validated is None:
                return None
            path = Path(gs.split_ref(validated[0])[0])
        snap = gs.open_snapshot(path)
    except PermissionError:
        # A member of the snapshot outside the data roots: the caller answers
        # as for a refused path, without naming where it leads.
        raise
    except (OSError, ValueError) as exc:
        return _result("particles", "unsupported", True,
                       f"Gadget snapshot that cannot be read: {exc}")
    ptype = gs.default_type(snap)
    if ptype is None:
        return _result("particles", "unsupported", True, "Gadget snapshot with no particles.")
    ref = gs.gadget_ref(snap.path, ptype)
    components = particle_components(ref)
    counts = ", ".join(f"{c['num_rows']} {c['name']}" for c in components)
    files = f"{len(snap.files)} files" if len(snap.files) > 1 else "one file"
    detail = (f"{snap.fmt} snapshot ({files}) at z = {snap.header.redshift:.3g}: {counts}. "
              "Each particle type opens as a table of points.")
    if snap.skipped:
        reasons = dict(snap.skip_reasons)
        detail += " Blocks left out: " + "; ".join(
            f"{name} ({reasons[name]})" if reasons.get(name) else name
            for name in snap.skipped) + "."
    if catalogue is not None:
        detail += (f" The folder also holds the group catalogue {catalogue.name}: open that "
                   "file for its FoF groups and subhalos as targets.")
    return _result("particles", "catalogue", True, detail, open_path=ref,
                   components=components)


def _result(kind: str, opener: str, confident: bool, detail: str,
            open_path: str = "", components: list | None = None) -> dict:
    return {"kind": kind, "opener": opener, "confident": confident, "detail": detail,
            "open_path": open_path, "components": list(components or [])}
