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

from fastapi import APIRouter, Depends
from pydantic import BaseModel

from ..auth import verify_token
from ..dependencies import assert_safe_fetch_url, get_exports_dir

router = APIRouter(tags=["vlkb"])
logger = logging.getLogger("visivo.backend")
_auth = Depends(verify_token)

# Cap a single cutout download so a hostile/oversized URL can't fill the disk.
_MAX_CUTOUT_BYTES = int(os.environ.get("VISIVO_VLKB_MAX_BYTES", str(8 * 1024**3)))  # 8 GiB


class VlkbFetchRequest(BaseModel):
    soda_url: str
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
