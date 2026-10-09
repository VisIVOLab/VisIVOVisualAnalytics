"""Edit a FITS header into a new file — the legacy's header modifier, widened.

The legacy dialog (``fitsheadermodifierdialog.cpp``) appeared only when a file
failed to open, offered exactly three groups of keywords — CTYPE, CUNIT, CDELT —
and asked the user to type values blind: it copied the file, wrote what was
typed, and said "File has been saved!" whether or not the result was a header
anything could read. If the guess was wrong you found out by opening the copy.

What is here instead:

* **any keyword**, not three. A missing ``BUNIT``, a wrong ``EQUINOX`` or a
  ``BMAJ`` that was never written are the same kind of problem;
* **structural keywords are refused.** ``SIMPLE``, ``BITPIX``, ``NAXISn`` and
  friends describe the bytes on disk, not the science. Letting them be typed
  over produces a file that no longer matches its own data;
* **the answer comes back with the result.** After the patch, the WCS is built
  from the new header and the reply says what it is — a celestial solution, a
  degraded one, or still nothing — so "did that fix it?" is answered here
  rather than by reopening the copy and hoping.

The input file is never modified: the patch is always written to a new file in
the workspace, as the legacy also did.
"""
from __future__ import annotations

import re
from pathlib import Path
from typing import Any

from astropy.io import fits

#: Keywords that describe the file's structure rather than its science. Editing
#: them through this route would make the header disagree with the data that
#: follows it; the shape of a file is changed by rewriting the data, not by
#: typing a new NAXIS2.
STRUCTURAL_KEYS = frozenset({
    "SIMPLE", "BITPIX", "EXTEND", "END", "XTENSION", "PCOUNT", "GCOUNT", "TFIELDS",
    # BSCALE/BZERO/BLANK are the same category one level up: they say how the
    # stored integers map to physical values. Editing one does not annotate the
    # data, it silently multiplies it.
    "BSCALE", "BZERO", "BLANK",
})

#: A FITS keyword: up to eight characters, upper case, digits, hyphen and
#: underscore. (HIERARCH long keywords are deliberately out of scope here.)
_KEY_PATTERN = re.compile(r"^[A-Z0-9_-]{1,8}$")

#: Keywords whose value is free text and which may repeat, so they are appended
#: rather than replaced.
_APPEND_ONLY = frozenset({"HISTORY", "COMMENT"})


class HeaderEditError(ValueError):
    """A patch that must not be applied, with the reason the user needs."""


def validate_key(raw: str) -> str:
    """Normalise and check one keyword, or raise :class:`HeaderEditError`."""
    key = str(raw or "").strip().upper()
    if not key:
        raise HeaderEditError("A keyword cannot be empty.")
    if not _KEY_PATTERN.match(key):
        raise HeaderEditError(
            f"'{raw}' is not a FITS keyword: up to 8 characters, letters, digits, "
            f"'-' or '_'.")
    if key in STRUCTURAL_KEYS or re.match(r"^NAXIS\d*$", key):
        raise HeaderEditError(
            f"{key} describes the file's structure and cannot be edited here — "
            f"it has to match the data on disk.")
    return key


def coerce_value(raw: Any) -> Any:
    """Turn a JSON value into what should be written into the card.

    A string that reads as a number is written as a number: ``CDELT1`` is a
    floating-point card, and a quoted "-0.001" in its place is a string card
    that every WCS parser then ignores — which is precisely the failure the
    legacy dialog existed to repair.
    """
    if isinstance(raw, bool) or raw is None:
        return raw
    if isinstance(raw, (int, float)):
        return raw
    text = str(raw).strip()
    if text == "":
        return ""
    lowered = text.lower()
    if lowered in ("true", "t"):
        return True
    if lowered in ("false", "f"):
        return False
    # Quoted explicitly: the user wants a string, even if it looks numeric.
    if len(text) >= 2 and text[0] == text[-1] and text[0] in ("'", '"'):
        return text[1:-1]
    try:
        if re.match(r"^[+-]?\d+$", text):
            return int(text)
        return float(text)
    except ValueError:
        return text


def apply_patch(header: fits.Header, cards: list[dict[str, Any]],
                remove: list[str] | None = None) -> list[str]:
    """Apply the edits to ``header`` in place; return one line per change.

    The lines are what the user is shown afterwards, so they name the old value
    as well as the new: "CDELT1: -0.0005 → -0.001" is checkable, "CDELT1
    updated" is not.
    """
    changes: list[str] = []

    for key in remove or []:
        checked = validate_key(key)
        if checked in header:
            old = header[checked]
            del header[checked]
            changes.append(f"{checked}: removed (was {old!r})")
        else:
            changes.append(f"{checked}: not present, nothing removed")

    for card in cards or []:
        key = validate_key(card.get("key", ""))
        value = coerce_value(card.get("value", ""))
        # None means "leave the comment as it is"; a string — including the
        # empty one — means "make it this". Without that distinction a comment
        # can be written but never cleared, and every caller that omits the
        # field would wipe the comments it never mentioned.
        raw_comment = card.get("comment", None)
        comment = None if raw_comment is None else str(raw_comment)
        if key in _APPEND_ONLY:
            header[key] = str(value)
            changes.append(f"{key}: added")
            continue
        if key in header:
            old = header[key]
            old_comment = str(header.comments[key] or "")
            # A comment is part of the card: changing only the comment is still
            # a change, and reporting "unchanged" would drop it silently.
            if old == value and (comment is None or comment == old_comment):
                changes.append(f"{key}: unchanged ({value!r})")
                continue
            header[key] = value if comment is None else (value, comment)
            if old == value:
                changes.append(f"{key}: comment {old_comment!r} → {comment!r}")
            else:
                changes.append(f"{key}: {old!r} → {value!r}")
        else:
            header[key] = value if comment is None else (value, comment)
            changes.append(f"{key}: added as {value!r}")

    return changes


def describe_wcs(header: fits.Header) -> dict[str, Any]:
    """What the WCS of this header amounts to, in one sentence plus flags.

    This is the part the legacy could not tell you: whether the values just
    typed actually produce a solution.
    """
    try:
        from astropy.wcs import WCS  # noqa: PLC0415
    except ImportError:
        return {"status": "unknown", "message": "astropy.wcs is unavailable on this server.",
                "celestial": False}
    import warnings  # noqa: PLC0415

    try:
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            wcs = WCS(header, relax=True, fix=True)
    except Exception as exc:  # noqa: BLE001 — a bad header is the input, not a crash
        return {"status": "none", "celestial": False,
                "message": f"No WCS could be built from this header: {exc}"}

    celestial = bool(wcs.has_celestial)
    axes = [str(t or "").strip() for t in (wcs.wcs.ctype or [])]
    named = ", ".join(a for a in axes if a) or "no CTYPE values"
    if celestial:
        return {"status": "celestial", "celestial": True,
                "message": f"Celestial WCS ({named})."}
    return {"status": "degraded", "celestial": False,
            "message": f"A WCS was built but it has no celestial axes ({named}). "
                       f"Pixel coordinates only."}


def worker_patch_header(input_path: str, output_path: str, cards: list[dict[str, Any]],
                        remove: list[str] | None = None) -> dict[str, Any]:
    """Copy ``input_path`` to ``output_path`` with the header edits applied."""
    if not cards and not remove:
        raise HeaderEditError("No changes were given.")

    with fits.open(input_path, memmap=False) as hdul:
        hdu_index = next(
            (i for i, h in enumerate(hdul)
             if isinstance(h, (fits.PrimaryHDU, fits.ImageHDU, fits.CompImageHDU))
             and getattr(h, "data", None) is not None),
            0)
        # A patch is written into a copy of the whole file, extensions included:
        # the edited header is one HDU of it, and dropping the rest would lose
        # data the user never asked to lose.
        hdul = fits.HDUList([h.copy() for h in hdul])
        changes = apply_patch(hdul[hdu_index].header, cards, remove)
        hdul[hdu_index].header["HISTORY"] = "VisIVO: header edited"
        wcs = describe_wcs(hdul[hdu_index].header)
        Path(output_path).parent.mkdir(parents=True, exist_ok=True)
        hdul.writeto(output_path, overwrite=True)

    return {
        "output_path": str(output_path),
        "changes": changes,
        "hdu_index": int(hdu_index),
        "wcs_status": wcs["status"],
        "wcs_message": wcs["message"],
        "wcs_celestial": bool(wcs["celestial"]),
    }
