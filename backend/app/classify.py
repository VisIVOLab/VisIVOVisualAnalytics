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


def classify_path(path: str | Path) -> dict:
    """``{kind, opener, confident, detail, open_path}`` for one path.

    ``kind`` names the content ("image", "cube", "dynspec", "fits-table",
    "catalogue", "vbt", "unknown"); ``opener`` names the route that opens it.
    ``open_path`` is the path the opener should be given when it differs from
    the one asked about — a VBT selected by its ``.bin`` opens by its ``.head``
    sidecar, which is the file that actually describes the table.
    """
    path = Path(path)
    if path.is_dir():
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

    if suffix == ".speck":
        return _result("catalogue", "catalogue", True, "speck point catalogue.")

    with open(path, "rb") as handle:
        head = handle.read(_SNIFF_BYTES)

    # ── HDF5: beamformed dynamic spectra come this way ───────────────────────
    if head.startswith(b"\x89HDF\r\n\x1a\n"):
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


def _result(kind: str, opener: str, confident: bool, detail: str,
            open_path: str = "") -> dict:
    return {"kind": kind, "opener": opener, "confident": confident, "detail": detail,
            "open_path": open_path}
