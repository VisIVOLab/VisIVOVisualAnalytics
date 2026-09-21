"""Simple Cone Search — ask any VO service what is around a position.

The legacy client had a form for this (``SimpleConeSearchForm``) that shelled
out to a bundled ``pyvo_scs.py`` through ``QProcess``, using whichever Python
interpreter the user had configured in *setting.ini*. If that path was wrong or
the environment lacked pyvo, the search failed with the interpreter's own error
text. What came back was written to a temp file, read line by line, split on
commas, and shown in a table; the "request cutout" button next to it was
commented out in the source.

Here the search is one HTTP request made by the backend, which already knows how
to fetch safely (the URL is user-supplied, so it goes through the SSRF guard),
and the result is both returned as rows *and* saved as a CSV in the workspace —
which is what turns "a table I can look at" into a catalogue the viewers can
overlay, the thing the legacy's dead button was reaching for.
"""
from __future__ import annotations

import csv
import io
import math
from pathlib import Path
from typing import Any
from urllib.parse import parse_qsl, urlencode, urlsplit, urlunsplit

#: Rows returned to the client. A cone on a crowded field can be enormous, and
#: the saved CSV keeps everything — this cap is for the table on screen.
DEFAULT_MAX_ROWS = 5000

#: A starting list of services. The legacy had none: you typed a URL, and a
#: mistyped one failed as a network error.
KNOWN_SERVICES: tuple[dict[str, str], ...] = (
    {"name": "VizieR — 2MASS Point Source Catalogue",
     "url": "https://vizier.cds.unistra.fr/viz-bin/conesearch/II/246/out"},
    {"name": "VizieR — Gaia DR3",
     "url": "https://vizier.cds.unistra.fr/viz-bin/conesearch/I/355/gaiadr3"},
    {"name": "VizieR — Hi-GAL compact sources (Elia+ 2021)",
     "url": "https://vizier.cds.unistra.fr/viz-bin/conesearch/J/MNRAS/504/2742/table1"},
    {"name": "VizieR — NVSS",
     "url": "https://vizier.cds.unistra.fr/viz-bin/conesearch/VIII/65/nvss"},
    {"name": "NED — objects",
     "url": "https://ned.ipac.caltech.edu/cgi-bin/NEDobjsearch?search_type=Near+Position+Search"},
)


class ConeSearchError(ValueError):
    """Something the user can fix, phrased for them."""


def _finite(name: str, value: Any) -> float:
    try:
        number = float(value)
    except (TypeError, ValueError) as exc:
        raise ConeSearchError(f"{name} must be a number.") from exc
    if not math.isfinite(number):
        raise ConeSearchError(f"{name} must be a finite number.")
    return number


def build_url(service_url: str, ra: float, dec: float, radius: float, verbosity: int = 2) -> str:
    """The SCS request URL: the service's own query string, plus RA/DEC/SR/VERB.

    A VizieR cone-search URL already carries parameters of its own, so the
    standard ones are merged into what is there rather than appended blindly —
    ``?x=1?RA=…`` is not a URL, and string concatenation is how that happens.
    """
    raw = str(service_url or "").strip()
    if not raw:
        raise ConeSearchError("Enter the URL of a cone-search service.")
    parts = urlsplit(raw)
    if parts.scheme not in ("http", "https"):
        raise ConeSearchError("The service URL must start with http:// or https://.")
    if not parts.netloc:
        raise ConeSearchError("The service URL has no host.")

    ra_value = _finite("RA", ra) % 360.0
    dec_value = _finite("Dec", dec)
    if not -90.0 <= dec_value <= 90.0:
        raise ConeSearchError("Dec must be between −90 and +90 degrees.")
    radius_value = _finite("Radius", radius)
    if radius_value <= 0 or radius_value > 90.0:
        raise ConeSearchError("The search radius must be greater than 0 and at most 90 degrees.")
    if int(verbosity) not in (1, 2, 3):
        raise ConeSearchError("Verbosity is 1, 2 or 3.")

    query = [(key, value) for key, value in parse_qsl(parts.query, keep_blank_values=True)
             if key.upper() not in ("RA", "DEC", "SR", "VERB")]
    query += [("RA", f"{ra_value:.8f}"), ("DEC", f"{dec_value:.8f}"),
              ("SR", f"{radius_value:.8f}"), ("VERB", str(int(verbosity)))]
    return urlunsplit((parts.scheme, parts.netloc, parts.path, urlencode(query), parts.fragment))


def _refuse_external_streams(payload: bytes) -> None:
    """Refuse a VOTable that keeps its data somewhere else.

    A ``STREAM`` element may carry an ``href``, and astropy will go and fetch it
    — with no guard of ours in the way. A public, allowlisted service could
    point that at ``127.0.0.1`` or at ``file:///etc/passwd`` and have the
    contents come back as table cells.

    This is settled with a parser rather than a pattern: an attribute value may
    contain ``>`` (``<STREAM rights=">" href="…">`` is valid XML), so any regex
    written in terms of "up to the closing bracket" can be stepped around. The
    pattern survives only as the fallback for a document that does not parse —
    which astropy will then refuse for its own reasons.
    """
    import re  # noqa: PLC0415
    import xml.etree.ElementTree as ET  # noqa: PLC0415, N817

    candidates = (payload, payload.replace(b"\x00", b""))

    # A DTD is not needed by any VOTable, and entity expansion is a denial of
    # service waiting to happen. Refuse the whole class rather than bound it.
    for candidate in candidates:
        if re.search(rb"<!\s*(DOCTYPE|ENTITY)\b", candidate, re.IGNORECASE):
            raise ConeSearchError(
                "The service returned a document with a DTD, which is not accepted.")

    refusal = ConeSearchError(
        "The service returned a table whose data is stored elsewhere "
        "(a STREAM href). This is not fetched.")

    try:
        for _event, element in ET.iterparse(io.BytesIO(payload), events=("start",)):
            # The tag carries its namespace as "{uri}STREAM"; the prefix used in
            # the document is irrelevant by then.
            tag = str(element.tag).rsplit("}", 1)[-1].upper()
            if tag == "STREAM" and any(
                    str(name).rsplit("}", 1)[-1].lower() == "href" for name in element.attrib):
                raise refusal
    except ConeSearchError:
        raise
    except ET.ParseError:
        # Not well-formed XML by ElementTree's reckoning: fall back to the
        # pattern, and let astropy have the last word on the document.
        stream_href = re.compile(
            rb"<\s*(?:[A-Za-z_][\w.-]*\s*:\s*)?STREAM\b[^>]*\bhref\s*=", re.IGNORECASE)
        for candidate in candidates:
            if stream_href.search(candidate):
                raise refusal from None


def parse_votable(payload: bytes) -> tuple[list[str], list[list[Any]]]:
    """Columns and rows out of a VOTable, or raise with the service's own message.

    A VO service reports failure *inside* a valid VOTable — an INFO element with
    ``value="ERROR"`` — so an empty table is not necessarily an empty sky.
    """
    from astropy.io.votable import parse as parse_vo  # noqa: PLC0415
    from astropy.io.votable.exceptions import VOWarning  # noqa: PLC0415
    import re  # noqa: PLC0415
    import warnings  # noqa: PLC0415

    # A VOTable may keep its data somewhere else, in a STREAM with an href —
    # and the parser will go and fetch it, with no guard of ours in the way.
    # A public service could point that at 127.0.0.1 or at file:///etc/passwd
    # and have the contents come back as table cells, so the document is
    # refused before it is parsed.
    #
    # The scan runs on the bytes with NULs removed as well as on the bytes
    # themselves: an XML declared as UTF-16 is the same ASCII with a zero
    # between every character, and a pattern written for UTF-8 slides straight
    # past it.
    _refuse_external_streams(payload)

    try:
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", VOWarning)
            votable = parse_vo(io.BytesIO(payload))
    except Exception as exc:  # noqa: BLE001 — the response is input, not a crash
        raise ConeSearchError(f"The service did not return a readable VOTable: {exc}") from exc

    for info in getattr(votable, "infos", []):
        if str(getattr(info, "value", "")).upper() == "ERROR":
            raise ConeSearchError(str(getattr(info, "content", "") or info.name
                                      or "The service reported an error."))

    table = None
    for candidate in votable.iter_tables():
        table = candidate
        break
    if table is None:
        raise ConeSearchError("The service returned no table.")

    columns = [str(field.name) for field in table.fields]
    rows: list[list[Any]] = []
    array = table.array
    for record in array:
        row: list[Any] = []
        for name in columns:
            value = record[name]
            if hasattr(value, "item"):
                try:
                    value = value.item()
                except (ValueError, AttributeError):
                    value = str(value)
            if isinstance(value, bytes):
                value = value.decode("utf-8", errors="replace")
            if isinstance(value, float) and not math.isfinite(value):
                value = None
            row.append(value)
        rows.append(row)
    return columns, rows


def write_csv(columns: list[str], rows: list[list[Any]], output_path: str) -> str:
    """Save the whole result, so it can be opened as a catalogue afterwards."""
    destination = Path(output_path)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with open(destination, "w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(columns)
        for row in rows:
            writer.writerow(["" if value is None else value for value in row])
    return str(destination)


def position_columns(columns: list[str]) -> dict[str, str]:
    """Which columns hold the position, by the usual UCD-ish names.

    Returned so the client can offer "overlay this" without asking the user to
    pick columns for a catalogue it has just downloaded.
    """
    ra_names = ("ra", "_raj2000", "raj2000", "ra_icrs", "raj2000d", "alpha", "_ra")
    dec_names = ("dec", "de", "_dej2000", "dej2000", "de_icrs", "dej2000d", "delta", "_de")
    found: dict[str, str] = {}
    lowered = {name.lower().strip(): name for name in columns}
    for key in ra_names:
        if key in lowered:
            found["ra"] = lowered[key]
            break
    for key in dec_names:
        if key in lowered:
            found["dec"] = lowered[key]
            break
    return found


def run_cone_search(service_url: str, ra: float, dec: float, radius: float, verbosity: int,
                    output_path: str, max_rows: int = DEFAULT_MAX_ROWS,
                    timeout: float = 120.0) -> dict[str, Any]:
    """Query the service, save every row, return the first ``max_rows``."""
    import urllib.error  # noqa: PLC0415

    from .safe_fetch import safe_urlopen  # noqa: PLC0415

    url = build_url(service_url, ra, dec, radius, verbosity)
    # The URL comes from the user, so it is a fetch target like any other — and
    # so is every redirect it leads to.
    try:
        response = safe_urlopen(
            url, allowlist_env="VISIVO_CONE_SEARCH_ALLOWED_HOSTS", timeout=timeout,
            headers={"User-Agent": "VisIVO-Backend/1.0 (cone search)"})
    except ValueError as exc:
        raise ConeSearchError(str(exc)) from exc
    except urllib.error.HTTPError as exc:
        raise ConeSearchError(f"The service answered {exc.code} {exc.reason}.") from exc
    except urllib.error.URLError as exc:
        raise ConeSearchError(f"The service could not be reached: {exc.reason}") from exc
    try:
        with response:
            payload = response.read()
    except urllib.error.URLError as exc:
        raise ConeSearchError(f"The service could not be reached: {exc.reason}") from exc

    columns, rows = parse_votable(payload)
    saved = write_csv(columns, rows, output_path) if rows else ""
    truncated = len(rows) > max_rows
    return {
        "url": url,
        "columns": columns,
        "rows": rows[:max_rows],
        "total_rows": len(rows),
        "truncated": truncated,
        "output_path": saved,
        "position_columns": position_columns(columns),
    }
