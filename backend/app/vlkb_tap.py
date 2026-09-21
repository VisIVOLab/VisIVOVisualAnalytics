"""VLKB TAP — the query composer the legacy called "VLKB simple query".

``VLKBSimpleQueryComposer`` in the legacy client built one of four ADQL queries
over a Galactic longitude/latitude box and ran it through a bundled
``pyvo_tap.py`` script, launched with whichever Python the user had configured
in *setting.ini*. The result was written to a ``.dat`` file and imported.

The queries themselves are worth keeping — they encode where each catalogue
lives in the VLKB schema — so they are here, as functions with the box as
arguments. What is not kept:

* **the subprocess.** A TAP sync request is one HTTP GET; routing it through a
  user-configured interpreter meant the feature stopped working whenever that
  path went stale, and reported the interpreter's error when it did;
* **the bubble query that never existed.** The legacy had a "bubbles" mode with
  a button, a selector and an importer — and ``generateQuery()`` has no branch
  for it, so picking bubbles ran the *compact sources* query. That is why the
  mode appeared to do nothing useful. The query is written here;
* **silent truncation.** A TAP service applies its own row limit; the reply says
  what was asked for and how much came back.

Every result is saved as CSV in the workspace, which is what lets a query become
a catalogue the viewers can open — the legacy's own import path, without the
intermediate format.
"""
from __future__ import annotations

import csv
import io
import math
import os
from pathlib import Path
from typing import Any
from urllib.parse import urlencode

#: Where the VLKB TAP service lives when nothing says otherwise.
DEFAULT_TAP_BASE = "https://vlkb.ia2.inaf.it/vlkb"

#: The named queries, each over a Galactic box. `label` is what the user picks.
PRESETS: tuple[dict[str, str], ...] = (
    {"id": "bandmerged", "label": "Compact sources — band-merged (SED view)",
     "table": "compactsources.sed_view_final"},
    {"id": "band", "label": "Compact sources — one Hi-GAL band",
     "table": "compactsources.higal<band>"},
    {"id": "filaments", "label": "Filaments (with branches)",
     "table": "filaments.filaments"},
    {"id": "bubbles", "label": "Bubbles", "table": "bubbles.bubbles"},
    {"id": "distances", "label": "3-D selection — sources with distances",
     "table": "compactsources.distances"},
)

#: Hi-GAL bands, in μm, for the per-band tables.
BANDS = ("70", "160", "250", "350", "500")


class VlkbQueryError(ValueError):
    """Something the user can fix."""


def tap_base(explicit: str = "") -> str:
    return (explicit or "").strip() or os.environ.get("VISIVO_VLKB_URL", "").strip() \
        or DEFAULT_TAP_BASE


def _box(glon_min: float, glon_max: float, glat_min: float, glat_max: float
         ) -> tuple[float, float, float, float]:
    """Order and check the box, so a query is never built from nonsense."""
    values = [glon_min, glon_max, glat_min, glat_max]
    if any(v is None or not math.isfinite(float(v)) for v in values):
        raise VlkbQueryError("The box must be four finite numbers.")
    lon_min, lon_max = sorted((float(glon_min), float(glon_max)))
    lat_min, lat_max = sorted((float(glat_min), float(glat_max)))
    if lon_min == lon_max or lat_min == lat_max:
        raise VlkbQueryError("The box has zero width or height.")
    if not -90.0 <= lat_min <= 90.0 or not -90.0 <= lat_max <= 90.0:
        raise VlkbQueryError("Galactic latitude must be between −90 and +90 degrees.")
    return lon_min, lon_max, lat_min, lat_max


def build_query(preset: str, glon_min: float, glon_max: float, glat_min: float,
                glat_max: float, band: str = "") -> str:
    """The ADQL for one preset over one box."""
    lon_min, lon_max, lat_min, lat_max = _box(glon_min, glon_max, glat_min, glat_max)
    box = (f"(glon >= {lon_min:.6f} AND glon <= {lon_max:.6f}) AND "
           f"(glat >= {lat_min:.6f} AND glat <= {lat_max:.6f})")

    if preset == "filaments":
        # The join the legacy used: a filament plus the branches that belong to
        # it, which is what the viewer draws.
        return (
            "SELECT f.idfil_mos AS idfil_mos, f.contour AS contour, f.glon AS glon, "
            "f.glat AS glat, b.contour AS branches_contour, "
            "b.contour1d AS branches_contour1d, b.contour_new AS branches_contour_new, "
            "b.flagspine AS flagspine_branches "
            "FROM filaments.filaments AS f JOIN filaments.branches AS b "
            "ON f.idfil_mos = b.idfil_mos "
            f"WHERE (f.glon >= {lon_min:.6f} AND f.glon <= {lon_max:.6f}) AND "
            f"(f.glat >= {lat_min:.6f} AND f.glat <= {lat_max:.6f})")

    if preset == "bubbles":
        # The query the legacy never had: its bubble mode fell through to the
        # compact-source branch. A bubble is a closed contour around a centre.
        return (
            "SELECT idbubble, glon_cen, glat_cen, contour FROM bubbles.bubbles "
            f"WHERE (glon_cen >= {lon_min:.6f} AND glon_cen <= {lon_max:.6f}) AND "
            f"(glat_cen >= {lat_min:.6f} AND glat_cen <= {lat_max:.6f})")

    if preset == "distances":
        # Galactocentric Cartesian coordinates from the heliocentric distance,
        # exactly as the legacy computed them — this is what makes the result a
        # 3-D point cloud rather than a list.
        return (
            "SELECT dist * cos(radians(glon) + 3.14159265359 / 2) AS x, "
            "dist * sin(radians(glon) + 3.14159265359 / 2) AS y, "
            "dist * tan(radians(glat)) AS z, * "
            f"FROM compactsources.distances WHERE ({box})")

    if preset == "bandmerged":
        # The band-merged view uses its own position column names.
        return (
            "SELECT DISTINCT * FROM compactsources.sed_view_final "
            f"WHERE (glonft >= {lon_min:.6f} AND glonft <= {lon_max:.6f}) AND "
            f"(glatft >= {lat_min:.6f} AND glatft <= {lat_max:.6f})")

    if preset == "band":
        code = str(band or "").strip().replace("um", "").replace("µm", "")
        if code not in BANDS:
            raise VlkbQueryError(
                f"Unknown Hi-GAL band '{band}'. Use one of: {', '.join(BANDS)}.")
        return f"SELECT * FROM compactsources.higal{code} WHERE {box}"

    raise VlkbQueryError(f"Unknown query '{preset}'.")


def sync_url(base: str, adql: str, max_rows: int = 0) -> str:
    """The TAP sync GET for one query."""
    statement = str(adql or "").strip()
    if not statement:
        raise VlkbQueryError("The query is empty.")
    lowered = statement.lower()
    if not lowered.startswith("select"):
        # TAP sync is a read interface; anything else is either a mistake or an
        # attempt at something this has no business forwarding.
        raise VlkbQueryError("Only SELECT queries can be run.")
    params = [("REQUEST", "doQuery"), ("LANG", "ADQL"), ("FORMAT", "csv"),
              ("QUERY", statement)]
    if max_rows > 0:
        params.append(("MAXREC", str(int(max_rows))))
    return f"{tap_base(base).rstrip('/')}/tap/sync?{urlencode(params)}"


def parse_csv(payload: bytes) -> tuple[list[str], list[list[str]]]:
    """Columns and rows out of the service's CSV, or its error message.

    A TAP service answers a bad query with a VOTable carrying the message even
    when CSV was asked for, so what comes back is not always a table.
    """
    text = payload.decode("utf-8", errors="replace")
    stripped = text.lstrip()
    if stripped.startswith("<"):
        # A VOTable (or an HTML error page): dig the message out rather than
        # showing the user angle brackets.
        import re  # noqa: PLC0415
        match = re.search(r"<INFO[^>]*value=\"ERROR\"[^>]*>(.*?)</INFO>", text,
                          re.IGNORECASE | re.DOTALL)
        if match:
            raise VlkbQueryError(re.sub(r"<[^>]+>", " ", match.group(1)).strip())
        raise VlkbQueryError("The service did not return a table.")

    reader = csv.reader(io.StringIO(text))
    rows = list(reader)
    if not rows:
        raise VlkbQueryError("The service returned an empty response.")
    header = [str(name).strip() for name in rows[0]]
    return header, [row for row in rows[1:] if any(cell.strip() for cell in row)]


def run_query(base: str, adql: str, output_path: str, max_rows: int = 0,
              preview_rows: int = 200, timeout: float = 180.0) -> dict[str, Any]:
    """Run the query, save every row as CSV, return a preview of it."""
    import urllib.error  # noqa: PLC0415

    from .safe_fetch import safe_urlopen  # noqa: PLC0415

    url = sync_url(base, adql, max_rows)
    try:
        # The service URL can come from the client, and a redirect can lead
        # anywhere: both are checked. The open and the read are in ONE try: a
        # TAP service reports a bad query as a 400 whose body carries the
        # explanation, and that is raised by the open, not by the read.
        with safe_urlopen(
                url, allowlist_env="VISIVO_VLKB_ALLOWED_HOSTS", timeout=timeout,
                headers={"User-Agent": "VisIVO-Backend/1.0 (VLKB TAP)"}) as response:
            payload = response.read()
    except VlkbQueryError:
        raise
    except ValueError as exc:
        raise VlkbQueryError(str(exc)) from exc
    except urllib.error.HTTPError as exc:
        body = b""
        try:
            body = exc.read()
        except Exception:  # noqa: BLE001 — the body is a courtesy, not a requirement
            body = b""
        try:
            parse_csv(body)
        except VlkbQueryError as parsed:
            raise VlkbQueryError(str(parsed)) from exc
        raise VlkbQueryError(f"The VLKB answered {exc.code} {exc.reason}.") from exc
    except urllib.error.URLError as exc:
        raise VlkbQueryError(f"The VLKB could not be reached: {exc.reason}") from exc

    columns, rows = parse_csv(payload)
    saved = ""
    if rows:
        destination = Path(output_path)
        destination.parent.mkdir(parents=True, exist_ok=True)
        with open(destination, "w", newline="", encoding="utf-8") as handle:
            writer = csv.writer(handle)
            writer.writerow(columns)
            writer.writerows(rows)
        saved = str(destination)

    return {
        "url": url,
        "query": adql,
        "columns": columns,
        "rows": rows[:max(0, int(preview_rows))],
        "total_rows": len(rows),
        "truncated": len(rows) > preview_rows,
        "output_path": saved,
        # A service that stops at exactly MAXREC probably had more to give.
        "hit_row_limit": bool(max_rows > 0 and len(rows) >= max_rows),
    }
