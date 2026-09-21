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

logger = logging.getLogger("visivo.backend")

#: How much of a text file to look at when deciding whether it is a table.
_SNIFF_BYTES = 8192

#: What the client should do with the file.
#:   "dataset"     → POST /v1/datasets/open  (image, cube, dynamic spectrum)
#:   "catalogue"   → POST /v1/catalogue/open (CSV, VOTable, IPAC, speck, FITS table)
#:   "vbt"         → POST /v1/vbt/open
#:   "ask"         → the content is ambiguous or unknown; let the user choose
#:   "unsupported" → we know what it is and no opener takes it; say so, don't ask
OPENERS = ("dataset", "catalogue", "vbt", "ask", "unsupported")


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
    if suffix == ".head":
        return _result("vbt", "vbt", True, "VisIVO Binary Table header.")
    if suffix == ".bin" and path.with_suffix(".bin.head").exists():
        # The opener parses the header, not the binary: hand it the sidecar.
        return _result("vbt", "vbt", True, "VisIVO Binary Table (header found beside it).",
                       open_path=str(path.with_suffix(".bin.head")))
    if stem_suffixes.endswith(".bin.head"):
        return _result("vbt", "vbt", True, "VisIVO Binary Table header.")

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
        return _classify_fits(path, head)
    if head[:2] == b"\x1f\x8b" or stem_suffixes.endswith((".fits.gz", ".fit.gz", ".fts.gz")):
        # Astropy would read a gzipped FITS, but none of the open routes accept
        # one (`_is_supported_dataset` matches on the suffix), so classifying it
        # as an image would only move the failure one dialog later.
        return _result("compressed", "unsupported", True,
                       "Compressed file. Decompress it first — the backend opens "
                       "uncompressed FITS, HDF5, catalogues and VBT.")

    # ── Text tables ──────────────────────────────────────────────────────────
    text = head.decode("utf-8", errors="replace").lstrip()
    if text.startswith("<?xml") or "<VOTABLE" in text.upper():
        return _result("catalogue", "catalogue", True, "VOTable.")
    if text.startswith(("|", "\\")):
        return _result("catalogue", "catalogue", True, "IPAC table.")
    if _looks_like_delimited_text(text):
        return _result("catalogue", "catalogue", True,
                       "Delimited text — opened as a catalogue.")

    return _result("unknown", "ask", False,
                   "The content does not identify itself; choose how to open it.")


def _classify_fits(path: Path, head: bytes) -> dict:
    """Image, cube or table — from the header alone, without reading data."""
    try:
        from astropy.io import fits  # noqa: PLC0415
    except ImportError:
        return _result("image", "dataset", False, "FITS (astropy unavailable to look closer).")

    try:
        with fits.open(path, memmap=False, lazy_load_hdus=True) as hdul:
            primary = hdul[0].header
            naxis = int(primary.get("NAXIS", 0))
            # The real axes: FITS routinely carries degenerate ones (a Stokes
            # axis of length 1), and counting those would call a map a cube.
            axes = [int(primary.get(f"NAXIS{i}", 0)) for i in range(1, naxis + 1)]
            significant = [n for n in axes if n > 1]

            if naxis == 0 or not significant:
                # No image in the primary HDU: the content is in an extension,
                # and for these files that is almost always a table.
                for hdu in hdul[1:]:
                    kind = str(hdu.header.get("XTENSION", "")).strip().upper()
                    if kind in ("BINTABLE", "TABLE"):
                        return _result(
                            "fits-table", "ask", True,
                            "FITS table — it can be a source catalogue, or a table "
                            "to overlay on an image.")
                return _result("unknown", "ask", False,
                               "FITS with no image and no table in it.")

            ctypes = [str(primary.get(f"CTYPE{i}", "")).upper() for i in range(1, naxis + 1)]
            if any(c.startswith(("TIME", "UTC")) for c in ctypes) \
                    and any(c.startswith("FREQ") for c in ctypes):
                return _result("dynspec", "dataset", True,
                               "FITS with time and frequency axes — a dynamic spectrum.")
            if len(significant) >= 3:
                return _result("cube", "dataset", True,
                               f"FITS cube ({' × '.join(str(n) for n in significant)}).")
            return _result("image", "dataset", True,
                           f"FITS image ({' × '.join(str(n) for n in significant)}).")
    except Exception as exc:  # noqa: BLE001 — a malformed header is a fact, not a crash
        logger.info("[classify] %s: %s", path, exc)
        return _result("image", "dataset", False,
                       f"FITS, but its header could not be read ({exc}).")


def _looks_like_delimited_text(text: str) -> bool:
    """Whether this reads as a table: several lines with the same field count.

    One line with commas is a sentence; ten lines that parse into the same
    number of fields is a catalogue.

    The fields are PARSED, not counted as characters. A quoted field containing
    the separator — an object name with a comma, a contour written as
    "10.4_10.6,0.0_0.2" — makes the raw counts differ between the header and the
    rows, and a character count then calls a perfectly ordinary CSV unknown.
    """
    import csv  # noqa: PLC0415
    import io  # noqa: PLC0415

    lines = [line for line in text.splitlines() if line.strip()
             and not line.lstrip().startswith("#")]
    if len(lines) < 2:
        return False
    # The last line of a truncated read is usually half a row.
    sample = lines[:10] if len(lines) > 10 else lines
    for separator in (",", "\t", ";"):
        try:
            rows = list(csv.reader(io.StringIO("\n".join(sample)), delimiter=separator))
        except csv.Error:
            continue
        counts = [len(row) for row in rows if row]
        if len(counts) >= 2 and counts[0] >= 2 and len(set(counts)) == 1:
            return True
    return False


def _result(kind: str, opener: str, confident: bool, detail: str,
            open_path: str = "") -> dict:
    return {"kind": kind, "opener": opener, "confident": confident, "detail": detail,
            "open_path": open_path}
