"""VLKB SODA cutout fetch.

``POST /v1/vlkb/fetch_cutout`` downloads a cutout from a *remote* SODA service
(the VLKB / IA2 SODA endpoint) and stores it on the backend filesystem, then
returns the local path. This is distinct from ``soda_cutout.make_cutout``,
which slices a FITS file the backend already has locally.

Because the URL is supplied by the client, the server would otherwise be a
Server-Side Request Forgery (SSRF) vector — an attacker could make the backend
fetch internal services (cloud metadata endpoints, localhost admin ports, …).
``_assert_safe_fetch_url`` guards against that: it requires http/https and, when
no explicit host allowlist is configured, refuses to connect to private,
loopback, link-local, or otherwise non-public addresses.

Configure a deployment allowlist with ``VISIVO_VLKB_ALLOWED_HOSTS`` (comma-
separated hostnames); allowlisted hosts skip the private-address check.
"""
import hashlib
import logging
import os
import urllib.error as _urllib_error
import urllib.request as _urllib_request
from pathlib import Path
from urllib.parse import urlparse

from fastapi import APIRouter, Depends, HTTPException
from pydantic import BaseModel, Field

from ..auth import verify_token
from ..dependencies import (_run, assert_safe_fetch_url, get_exports_dir,
                            make_workspace_path)

router = APIRouter(tags=["vlkb"])
logger = logging.getLogger("visivo.backend")
_auth = Depends(verify_token)

# Cap a single cutout download so a hostile/oversized URL can't fill the disk.
_MAX_CUTOUT_BYTES = int(os.environ.get("VISIVO_VLKB_MAX_BYTES", str(8 * 1024**3)))  # 8 GiB


class VlkbFetchRequest(BaseModel):
    soda_url: str
    vlkb_bearer: str = ""


class McutoutItem(BaseModel):
    """One dataset of a batch: what to cut, and where."""

    id: str
    #: "CIRCLE l b r" or "RANGE l1 l2 b1 b2" — the same string the single
    #: cutout path sends, so a batch means exactly the same region.
    pos: str
    possys: str = "GALACTIC"


class McutoutSubmitRequest(BaseModel):
    items: list[McutoutItem]
    service_url: str = ""
    vlkb_bearer: str = ""


class McutoutJobRequest(BaseModel):
    #: A job id is interpolated into a URL *and* into the directory the results
    #: are unpacked into, so it is constrained here as well as in the service
    #: module — "x/../../escaped" is a plain string until something refuses it.
    job_id: str = Field(pattern=r"^[A-Za-z0-9._:-]{1,128}$")
    service_url: str = ""
    vlkb_bearer: str = ""


@router.post("/v1/vlkb/fetch_cutout", tags=["vlkb"])
async def vlkb_fetch_cutout(request: VlkbFetchRequest, _: None = _auth):
    """Download a SODA cutout to the backend workspace; return its local path.

    The SODA URL comes from the client, so it is validated against SSRF: VLKB
    fetches external services, so private/loopback targets are blocked unless
    ``VISIVO_VLKB_ALLOWED_HOSTS`` explicitly trusts the host.
    """
    raw_url = (request.soda_url or "").strip()
    try:
        assert_safe_fetch_url(raw_url, allowlist_env="VISIVO_VLKB_ALLOWED_HOSTS", block_private=True)
    except ValueError as exc:
        return {"valid": False, "error": str(exc), "path": ""}

    out_dir = get_exports_dir() / "vlkb_cutouts"
    out_dir.mkdir(parents=True, exist_ok=True)
    url_hash = hashlib.sha256(raw_url.encode("utf-8")).hexdigest()[:16]
    stem = Path(urlparse(raw_url).path).name or "cutout"
    if not stem.lower().endswith((".fits", ".fits.gz", ".fit")):
        stem = f"{stem}.fits"
    out_path = out_dir / f"{url_hash}__{stem}"

    if out_path.exists():
        # Validate the cache too: a bad response saved before this check would
        # otherwise be handed back for ever, and re-requesting the same URL is
        # exactly what a user does after a failure.
        stale = _describe_if_not_fits(out_path, "")
        if stale:
            logger.info("[vlkb] discarding a cached non-FITS response for %s", raw_url)
            _unlink_quiet(out_path)
        else:
            logger.info("[vlkb] cache hit url=%s -> %s", raw_url, out_path)
            return {"valid": True, "error": "", "path": str(out_path)}

    logger.info("[vlkb] downloading url=%s -> %s", raw_url, out_path)
    try:
        req = _urllib_request.Request(raw_url, method="GET")
        if request.vlkb_bearer:
            req.add_header("Authorization", f"Bearer {request.vlkb_bearer}")
        total = 0
        content_type = ""
        with _urllib_request.urlopen(req, timeout=600) as resp, open(out_path, "wb") as out:
            content_type = (resp.headers.get("Content-Type") or "").split(";")[0].strip().lower()
            while True:
                chunk = resp.read(8 * 1024 * 1024)  # 8 MiB
                if not chunk:
                    break
                total += len(chunk)
                if total > _MAX_CUTOUT_BYTES:
                    raise ValueError(
                        f"Cutout exceeds the {_MAX_CUTOUT_BYTES} byte download limit."
                    )
                out.write(chunk)
    except _urllib_error.HTTPError as exc:
        _unlink_quiet(out_path)
        return {"valid": False, "error": f"Remote returned HTTP {exc.code}: {exc.reason}", "path": ""}
    except Exception as exc:  # noqa: BLE001
        _unlink_quiet(out_path)
        logger.warning("[vlkb] download failed url=%s error=%s", raw_url, exc)
        return {"valid": False, "error": f"Download failed: {exc}", "path": ""}

    # A SODA service answers an error with HTTP 200 and a VOTable or an HTML
    # page. Saving that as `.fits` and handing the path back moved the failure
    # to the FITS reader, which could only say "No SIMPLE card found" about a
    # file whose real content was the explanation the user needed.
    problem = _describe_if_not_fits(out_path, content_type)
    if problem:
        _unlink_quiet(out_path)
        logger.warning("[vlkb] not a FITS response url=%s: %s", raw_url, problem)
        return {"valid": False, "error": problem, "path": ""}

    return {"valid": True, "error": "", "path": str(out_path)}


def _describe_if_not_fits(path: Path, content_type: str) -> str:
    """Empty when the file really is FITS; otherwise why it is not.

    The check is on the bytes, not on the Content-Type: services mislabel FITS
    as octet-stream routinely, and mislabel their error pages as FITS just as
    readily.
    """
    try:
        size = path.stat().st_size
    except OSError as exc:
        return f"The download produced no file ({exc})."
    if size == 0:
        return "The service returned an empty response."

    with open(path, "rb") as handle:
        head = handle.read(4096)

    if head.startswith(b"SIMPLE"):
        return ""
    if head[:2] == b"\x1f\x8b":
        return ("The service returned a gzip-compressed file. Request it uncompressed, or "
                "save it and decompress it before opening.")
    if head[257:262] == b"ustar":
        return ("The service returned a tar archive rather than a single cutout — this ID "
                "probably needs the multi-cutout (async) endpoint.")

    # Whatever it is, the service usually explains itself in the body: a
    # VOTable <INFO>, an HTML error, a plain-text message. Show that.
    text = head.decode("utf-8", errors="replace").strip()
    detail = _first_message(text)
    label = content_type or "an unknown type"
    if detail:
        return f"The service returned {label} instead of FITS: {detail}"
    return (f"The service returned {label} instead of FITS "
            f"({size} bytes, starting with {text[:120]!r}).")


def _first_message(text: str) -> str:
    """The human-readable part of a VOTable / XML / HTML error body, as PLAIN text.

    Whatever comes back is written by a remote service and ends up in a dialog,
    so any markup inside it is stripped rather than passed along: a message that
    still contained ``<b>`` would be rendered as markup by a Qt message box that
    auto-detects rich text.
    """
    import html  # noqa: PLC0415
    import re  # noqa: PLC0415

    def _plain(raw: str) -> str:
        without_tags = re.sub(r"<[^>]*>", " ", raw)
        return " ".join(html.unescape(without_tags).split())[:300]

    # An explicit error INFO first: a VOTable can carry several, and the one
    # that says ERROR is the one worth showing.
    for pattern in (r"<INFO[^>]*value\s*=\s*[\"\']ERROR[\"\'][^>]*>(.*?)</INFO>",
                    r"<INFO[^>]*>(.*?)</INFO>",
                    r"<(?:title|h1|p)[^>]*>(.*?)</(?:title|h1|p)>"):
        match = re.search(pattern, text, re.IGNORECASE | re.DOTALL)
        if match and _plain(match.group(1)):
            return _plain(match.group(1))
    if text and not text.lstrip().startswith("<"):
        return _plain(text)
    return ""


def _unlink_quiet(path: Path) -> None:
    try:
        path.unlink()
    except FileNotFoundError:
        pass


# ── multi-cutout (UWS batch) ──────────────────────────────────────────────────
#
# One dataset at a time is what /fetch_cutout does. Twenty datasets is what the
# archive's asynchronous job API is for, and what the legacy client's
# MCutoutSummary drove: submit the list, poll a phase, collect one archive.
# See app/vlkb_mcutout.py for the protocol.


def _mcutout_service(explicit: str) -> str:
    """The archive base URL, checked against SSRF before anything is sent."""
    from ..vlkb_mcutout import McutoutError, resolve_service_url  # noqa: PLC0415

    try:
        base = resolve_service_url(explicit)
    except McutoutError as exc:
        raise HTTPException(503, str(exc)) from exc
    try:
        assert_safe_fetch_url(base, allowlist_env="VISIVO_VLKB_ALLOWED_HOSTS",
                              block_private=True)
    except ValueError as exc:
        raise HTTPException(400, str(exc)) from exc
    return base


def _mcutout_failure(exc: Exception) -> HTTPException:
    """The archive's own words, or ours, with the status that fits."""
    import urllib.error as _err  # noqa: PLC0415

    from ..vlkb_mcutout import McutoutError  # noqa: PLC0415

    if isinstance(exc, McutoutError):
        return HTTPException(422, str(exc))
    if isinstance(exc, _err.HTTPError):
        return HTTPException(502, f"The archive returned HTTP {exc.code}: {exc.reason}")
    return HTTPException(502, f"The archive could not be reached: {exc}")


@router.post("/v1/vlkb/mcutout/submit", tags=["vlkb"])
async def vlkb_mcutout_submit(request: McutoutSubmitRequest, _: None = _auth):
    """Submit a batch of cutouts as one asynchronous job.

    Body: ``{items: [{id, pos, possys}], service_url?, vlkb_bearer?}`` — ``pos``
    is the POS string the single-cutout path uses, so a batch covers exactly the
    region the user was shown.

    Returns ``{valid, error, job_id, phase, count}``. The job outlives the
    request (and the client): poll ``/phase``, then collect with ``/fetch``.
    """
    import asyncio  # noqa: PLC0415

    from ..vlkb_mcutout import job_phase, submit_job  # noqa: PLC0415

    base = _mcutout_service(request.service_url)
    items = [{"id": item.id, "pos": item.pos, "possys": item.possys} for item in request.items]
    try:
        job = await asyncio.to_thread(submit_job, base, items, request.vlkb_bearer)
    except Exception as exc:  # noqa: BLE001
        logger.warning("[mcutout] submit failed: %s", exc)
        raise _mcutout_failure(exc) from exc

    # The job exists from here on. A phase read that fails must not lose it:
    # reporting an error without the id would leave the batch running on the
    # archive with nobody able to collect it, and a retry would submit a second.
    try:
        phase = await asyncio.to_thread(job_phase, base, job, request.vlkb_bearer)
    except Exception as exc:  # noqa: BLE001
        logger.warning("[mcutout] submitted job=%s but its phase is unknown: %s", job, exc)
        phase = "UNKNOWN"
    logger.info("[mcutout] submitted job=%s items=%d phase=%s", job, len(items), phase)
    return {"valid": True, "error": "", "job_id": job, "phase": phase, "count": len(items)}


@router.post("/v1/vlkb/mcutout/phase", tags=["vlkb"])
async def vlkb_mcutout_phase(request: McutoutJobRequest, _: None = _auth):
    """Where the job has got to.

    Returns ``{valid, error, phase, terminal}``. A job the archive left ``HELD``
    is started here rather than reported as stuck — that is what HELD means, and
    the client should not have to know it.
    """
    import asyncio  # noqa: PLC0415

    from ..vlkb_mcutout import TERMINAL_PHASES, job_phase, run_job  # noqa: PLC0415

    base = _mcutout_service(request.service_url)
    try:
        phase = await asyncio.to_thread(job_phase, base, request.job_id, request.vlkb_bearer)
        if phase == "HELD":
            phase = await asyncio.to_thread(run_job, base, request.job_id, request.vlkb_bearer)
    except Exception as exc:  # noqa: BLE001
        raise _mcutout_failure(exc) from exc
    return {"valid": True, "error": "", "phase": phase, "terminal": phase in TERMINAL_PHASES}


@router.post("/v1/vlkb/mcutout/report", tags=["vlkb"])
async def vlkb_mcutout_report(request: McutoutJobRequest, _: None = _auth):
    """Per-dataset outcome of a finished job, in submission order.

    Returns ``{valid, error, results: [{index, ok, filename, detail}]}``.
    ``detail`` carries whatever the archive said about a failure — the only
    clue to why one dataset of twenty came back empty.
    """
    import asyncio  # noqa: PLC0415

    from ..vlkb_mcutout import job_report  # noqa: PLC0415

    base = _mcutout_service(request.service_url)
    try:
        results = await asyncio.to_thread(job_report, base, request.job_id, request.vlkb_bearer)
    except Exception as exc:  # noqa: BLE001
        raise _mcutout_failure(exc) from exc
    return {"valid": True, "error": "", "results": results}


@router.post("/v1/vlkb/mcutout/fetch", tags=["vlkb"])
async def vlkb_mcutout_fetch(request: McutoutJobRequest, _: None = _auth):
    """Download the job's archive and unpack it on the backend.

    Returns ``{valid, error, archive, files, bytes}`` — ``files`` are the
    extracted cutouts, ready for the ordinary dataset-open route. The legacy
    stopped at the `.tar.gz`, which left the cutouts one manual step away from
    the viewer that had just asked for them.

    Archive members that would write outside the job's own directory are
    refused, symlinks and devices skipped.
    """
    import asyncio  # noqa: PLC0415

    from ..vlkb_mcutout import fetch_results  # noqa: PLC0415

    base = _mcutout_service(request.service_url)
    out_dir = get_exports_dir() / "vlkb_cutouts"
    try:
        result = await asyncio.to_thread(fetch_results, base, request.job_id,
                                         str(out_dir), request.vlkb_bearer)
    except Exception as exc:  # noqa: BLE001
        logger.warning("[mcutout] fetch failed job=%s: %s", request.job_id, exc)
        raise _mcutout_failure(exc) from exc
    logger.info("[mcutout] fetched job=%s files=%d", request.job_id, len(result["files"]))
    return {"valid": True, "error": "", **result}


# ── TAP: the catalogue query composer ────────────────────────────────────────

class VlkbTapRequest(BaseModel):
    """Either a preset over a Galactic box, or an ADQL statement of your own."""
    #: "" = use `query`; otherwise one of app.vlkb_tap.PRESETS.
    preset: str = ""
    query: str = ""
    glon_min: float = 0.0
    glon_max: float = 0.0
    glat_min: float = 0.0
    glat_max: float = 0.0
    band: str = ""
    #: "" = the backend's configured VLKB.
    service_url: str = ""
    max_rows: int = 0
    output_basename: str = ""


class VlkbTapResponse(BaseModel):
    valid: bool
    error: str = ""
    url: str = ""
    query: str = ""
    columns: list[str] = []
    rows: list[list[str]] = []
    total_rows: int = 0
    truncated: bool = False
    output_path: str = ""
    workspace_filename: str = ""
    hit_row_limit: bool = False


class VlkbTapPresetsResponse(BaseModel):
    presets: list[dict[str, str]]
    bands: list[str]
    service_url: str


@router.get("/v1/vlkb/tap/presets", response_model=VlkbTapPresetsResponse, tags=["vlkb"])
async def vlkb_tap_presets(_: None = _auth) -> VlkbTapPresetsResponse:
    """The named queries, so the client does not carry the VLKB schema in it."""
    from .. import vlkb_tap  # noqa: PLC0415
    return VlkbTapPresetsResponse(
        presets=[dict(p) for p in vlkb_tap.PRESETS],
        bands=list(vlkb_tap.BANDS),
        service_url=vlkb_tap.tap_base(""),
    )


@router.post("/v1/vlkb/tap", response_model=VlkbTapResponse, tags=["vlkb"])
async def vlkb_tap_query(request: VlkbTapRequest, _: None = _auth) -> VlkbTapResponse:
    """Run an ADQL query against the VLKB and save the result as a catalogue."""
    from .. import vlkb_tap  # noqa: PLC0415

    try:
        adql = (vlkb_tap.build_query(request.preset, request.glon_min, request.glon_max,
                                     request.glat_min, request.glat_max, request.band)
                if request.preset else request.query)
    except vlkb_tap.VlkbQueryError as exc:
        return VlkbTapResponse(valid=False, error=str(exc))

    basename = request.output_basename.strip() or f"vlkb_{request.preset or 'query'}.csv"
    output_path = str(make_workspace_path(basename))
    try:
        result = await _run(vlkb_tap.run_query, request.service_url, adql, output_path,
                            int(request.max_rows))
    except vlkb_tap.VlkbQueryError as exc:
        return VlkbTapResponse(valid=False, error=str(exc))
    except Exception as exc:
        logger.exception("[vlkb-tap] failed preset=%s", request.preset)
        return VlkbTapResponse(
            valid=False, error="The query could not be completed. See the backend log.")

    workspace_filename = Path(result["output_path"]).name if result["output_path"] else ""
    return VlkbTapResponse(valid=True, workspace_filename=workspace_filename, **result)
