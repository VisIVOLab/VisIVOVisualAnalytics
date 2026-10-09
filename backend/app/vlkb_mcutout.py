"""VLKB multi-cutout — the archive's asynchronous batch API (UWS).

A single SODA `sync` request cuts one dataset. Asking for twenty means twenty
requests, twenty connections and twenty chances to lose one; the archive's own
answer to that is a UWS job: submit the whole list, poll a phase, collect one
archive of results. Ported from the legacy client's ``MCutoutSummary``
(ViaLactea Visual Analytics 1.7.4), which drove the same endpoints directly from
the desktop.

The protocol, as the service implements it:

1. ``POST <vlkb>/soda/async/uws?PHASE=RUN`` — multipart form-data with one part
   named ``mcutout`` carrying a JSON array of
   ``{id, possys, pos:{circle|range}}``. Answers **303** with the job URL in
   ``Location``; the job id is its last segment.
2. ``GET  <endpoint>/<job>/phase`` — a bare word: ``QUEUED``, ``EXECUTING``,
   ``COMPLETED``, ``HELD``, ``ABORTED``, ``ERROR``. ``HELD`` means the job was
   accepted but not started, and has to be run explicitly.
3. ``POST <endpoint>/<job>/phase`` with ``PHASE=RUN`` — start a held job.
4. ``GET  <endpoint>/<job>/results/Report`` — a JSON array, one entry per
   requested cutout, in the order they were submitted.
5. ``GET  <endpoint>/<job>/results/mcutout.tar.gz`` — the cutouts.

What this module does that the legacy did not: it **unpacks** the archive. The
legacy handed the user a `.tar.gz` and stopped there, which left the actual
cutouts one manual step away from the viewer that had just asked for them.
"""
from __future__ import annotations

import json
import logging
import os
import re
import shutil
import tarfile
import tempfile
import urllib.error as _urllib_error
import urllib.request as _urllib_request
import uuid
from pathlib import Path
from urllib.parse import urljoin, urlparse

logger = logging.getLogger("visivo.backend")

UWS_PATH = "/soda/async/uws"
_TIMEOUT_S = float(os.environ.get("VISIVO_VLKB_MCUTOUT_TIMEOUT", "300"))
_MAX_ARCHIVE_BYTES = int(os.environ.get("VISIVO_VLKB_MCUTOUT_MAX_BYTES", str(16 * 1024**3)))
_MAX_ITEMS = int(os.environ.get("VISIVO_VLKB_MCUTOUT_MAX_ITEMS", "500"))
_MAX_REPORT_BYTES = int(os.environ.get("VISIVO_VLKB_MCUTOUT_MAX_REPORT", str(8 * 1024**2)))
_MAX_EXTRACTED_BYTES = int(
    os.environ.get("VISIVO_VLKB_MCUTOUT_MAX_EXTRACTED", str(32 * 1024**3)))
_MAX_MEMBERS = int(os.environ.get("VISIVO_VLKB_MCUTOUT_MAX_MEMBERS", "2000"))
# Phases the service reports. Anything else is passed through untouched rather
# than guessed at.
TERMINAL_PHASES = {"COMPLETED", "ABORTED", "ERROR"}


class McutoutError(ValueError):
    """The batch request or the job is not usable."""


# ── the request body ──────────────────────────────────────────────────────────

def parse_pos(pos: str) -> dict:
    """``CIRCLE l b r`` / ``RANGE l1 l2 b1 b2`` → the service's JSON shape.

    One parser, here, rather than in the client: the POS string is what the user
    was shown and what the single-cutout path already sends, so the batch has to
    mean exactly the same region.
    """
    tokens = (pos or "").split()
    if not tokens:
        raise McutoutError("empty POS string")
    shape = tokens[0].upper()

    def _numbers(count: int) -> list[float]:
        if len(tokens) - 1 != count:
            raise McutoutError(
                f"{shape} needs {count} numbers, got {len(tokens) - 1}: {pos!r}")
        try:
            values = [float(t) for t in tokens[1:]]
        except ValueError as exc:
            raise McutoutError(f"{shape} has a non-numeric coordinate: {pos!r}") from exc
        if not all(v == v and abs(v) != float("inf") for v in values):
            raise McutoutError(f"{shape} has a non-finite coordinate: {pos!r}")
        return values

    if shape == "CIRCLE":
        lon, lat, radius = _numbers(3)
        if radius <= 0:
            raise McutoutError("a CIRCLE needs a radius greater than zero")
        return {"circle": {"lon": lon, "lat": lat, "radius": radius}}
    if shape == "RANGE":
        lon1, lon2, lat1, lat2 = _numbers(4)
        return {"range": {"lon1": lon1, "lon2": lon2, "lat1": lat1, "lat2": lat2}}
    raise McutoutError(f"unsupported POS shape {shape!r} (expected CIRCLE or RANGE)")


def build_request_body(items: list[dict]) -> list[dict]:
    """The JSON array the ``mcutout`` part carries.

    ``items`` is ``[{id, pos}]`` — the identifier of the dataset and the POS
    string of the region, exactly as the inventory and the single-cutout path
    use them.
    """
    if not items:
        raise McutoutError("a batch needs at least one dataset")
    if len(items) > _MAX_ITEMS:
        raise McutoutError(f"{len(items)} datasets is beyond the batch limit of {_MAX_ITEMS}")

    body = []
    for index, item in enumerate(items):
        identifier = str(item.get("id", "")).strip()
        if not identifier:
            raise McutoutError(f"dataset {index + 1} has no identifier")
        body.append({
            "id": identifier,
            "possys": (item.get("possys") or "GALACTIC").strip().upper(),
            "pos": parse_pos(item.get("pos", "")),
        })
    return body


# ── the job ───────────────────────────────────────────────────────────────────

class _RefuseRedirect(_urllib_request.HTTPRedirectHandler):
    """Do not follow a redirect.

    urllib copies the ``Authorization`` header into the redirected request, so
    a service that answers 302 with a host of its choosing is handed the user's
    VLKB bearer token.
    """

    def redirect_request(self, req, fp, code, msg, headers, newurl):  # noqa: D102, ANN001
        raise McutoutError(
            f"the archive redirected to {newurl!r}; redirects are not followed because "
            "they would carry the access token to another host")


class _CaptureRedirect(_urllib_request.HTTPRedirectHandler):
    """Do not follow a redirect either — but let the caller read it.

    Returning None makes urllib raise ``HTTPError``, whose headers still carry
    the ``Location``. That is the answer for the two calls whose whole point is
    a 3xx: the submit (the job URL) and the RUN command.
    """

    def redirect_request(self, req, fp, code, msg, headers, newurl):  # noqa: D102, ANN001
        return None


def _open(url: str, *, method: str = "GET", data: bytes | None = None,
          headers: dict | None = None, bearer: str = "", timeout: float | None = None,
          capture_redirect: bool = False):
    """Every request this module makes goes through here.

    One place decides that a bearer token never travels over plain HTTP and that
    a redirect is never followed with one attached — ``submit_job`` built its own
    request and so observed neither.
    """
    if bearer and not url.lower().startswith("https://"):
        raise McutoutError("refusing to send the VLKB token over an unencrypted connection")
    request = _urllib_request.Request(url, data=data, method=method)
    for key, value in (headers or {}).items():
        request.add_header(key, value)
    if bearer:
        request.add_header("Authorization", f"Bearer {bearer}")
    opener = _urllib_request.build_opener(
        _CaptureRedirect if capture_redirect else _RefuseRedirect)
    return opener.open(request, timeout=timeout or _TIMEOUT_S)


def _request(url: str, **kwargs):
    """Backwards-compatible alias for :func:`_open` (tests patch this name)."""
    return _open(url, **kwargs)


def submit_job(base_url: str, items: list[dict], bearer: str = "") -> str:
    """Submit the batch; return the job id.

    The service answers 303 and puts the job URL in ``Location``, so redirects
    are deliberately not followed — following one would fetch the job page and
    throw away the only place the id appears.
    """
    body = build_request_body(items)
    endpoint = base_url.rstrip("/") + UWS_PATH

    boundary = f"----visivo{uuid.uuid4().hex}"
    payload = (
        f"--{boundary}\r\n"
        'Content-Disposition: form-data; name="mcutout"; filename="mcutout"\r\n'
        "Content-Type: application/json\r\n\r\n"
    ).encode() + json.dumps(body).encode() + f"\r\n--{boundary}--\r\n".encode()

    try:
        with _open(f"{endpoint}?PHASE=RUN", method="POST", data=payload, bearer=bearer,
                   headers={"Content-Type": f"multipart/form-data; boundary={boundary}"},
                   capture_redirect=True) as response:
            location = response.headers.get("Location", "")
            status = response.status
    except _urllib_error.HTTPError as exc:
        # urllib raises on the 303 when redirects are captured; the headers are
        # still the answer.
        if exc.code in (302, 303, 307):
            location = exc.headers.get("Location", "")
            status = exc.code
        else:
            raise
    if not location:
        raise McutoutError(
            f"the archive accepted the batch with HTTP {status} but named no job "
            "(no Location header)")
    return job_id_from_location(location)


#: A job id goes into a URL *and* into a filesystem path, so it has to be a
#: plain identifier — and "." / ".." are identifiers by that pattern, which is
#: exactly the pair that walks out of a directory.
_JOB_ID = re.compile(r"[A-Za-z0-9._:-]{1,128}")


def validate_job_id(job: str) -> str:
    """The job id, or an error. Call this on anything that came from outside.

    Not only on what the archive returns: the client sends a job id back on
    every later call, and it is interpolated into both the request URL and the
    directory the results are unpacked into.
    """
    job = (job or "").strip()
    if not _JOB_ID.fullmatch(job) or job in {".", ".."}:
        raise McutoutError(f"{job!r} is not a usable job id")
    return job


def job_id_from_location(location: str) -> str:
    """The job id is the last non-empty segment of the job URL."""
    path = urlparse(location).path or location
    segments = [segment for segment in path.split("/") if segment]
    if not segments:
        raise McutoutError(f"cannot read a job id out of {location!r}")
    return validate_job_id(segments[-1])


def job_phase(base_url: str, job: str, bearer: str = "") -> str:
    """The job's current phase, as the service words it."""
    url = f"{base_url.rstrip('/')}{UWS_PATH}/{validate_job_id(job)}/phase"
    with _request(url, bearer=bearer) as response:
        return response.read(256).decode("utf-8", errors="replace").strip().upper()


def run_job(base_url: str, job: str, bearer: str = "") -> str:
    """Start a job the service left HELD, and report the phase afterwards."""
    url = f"{base_url.rstrip('/')}{UWS_PATH}/{validate_job_id(job)}/phase"
    try:
        # The service answers RUN with a 303 back to the job; that is success,
        # not a redirect to follow — capture it rather than refusing it.
        with _open(url, method="POST", data=b"PHASE=RUN", bearer=bearer,
                   headers={"Content-Type": "application/x-www-form-urlencoded"},
                   capture_redirect=True):
            pass
    except _urllib_error.HTTPError as exc:
        if exc.code not in (302, 303, 307):
            raise
    return job_phase(base_url, job, bearer)


def job_report(base_url: str, job: str, bearer: str = "") -> list[dict]:
    """Per-dataset outcome, in the order the batch was submitted.

    The service reports each entry as ``{type, …}``; ``type == "filename"`` is
    the success case. Everything else is a failure whose text is worth keeping —
    the legacy reduced it to the word "Error".
    """
    url = f"{base_url.rstrip('/')}{UWS_PATH}/{validate_job_id(job)}/results/Report"
    with _request(url, bearer=bearer) as response:
        # A report is a short JSON array; reading it with the ARCHIVE's cap
        # would let a hostile or broken service hand us gigabytes to parse.
        raw = response.read(_MAX_REPORT_BYTES + 1)
    if len(raw) > _MAX_REPORT_BYTES:
        raise McutoutError(f"the job report exceeds {_MAX_REPORT_BYTES} bytes")
    try:
        entries = json.loads(raw.decode("utf-8", errors="replace"))
    except json.JSONDecodeError as exc:
        raise McutoutError(f"the job report is not JSON: {raw[:200]!r}") from exc
    if not isinstance(entries, list):
        raise McutoutError("the job report is not a list of results")

    out = []
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict):
            out.append({"index": index, "ok": False, "detail": str(entry)[:300]})
            continue
        kind = str(entry.get("type", "")).lower()
        ok = kind == "filename"
        out.append({
            "index": index,
            "ok": ok,
            "filename": str(entry.get("content", entry.get("filename", ""))) if ok else "",
            # Keep whatever the service said; it is the only clue to WHY one
            # dataset of twenty came back empty.
            "detail": "" if ok else json.dumps(entry)[:300],
        })
    return out


def fetch_results(base_url: str, job: str, out_dir: str, bearer: str = "") -> dict:
    """Download the job's archive and unpack it; return the extracted files.

    The legacy stopped at "here is a .tar.gz" — the cutouts were then one manual
    step away from the viewer that had just requested them.

    Each fetch gets a directory of its own. Keying it on the job id alone meant
    two fetches of the same job, or two archives that happen to share an id,
    writing the same files at the same time.
    """
    job = validate_job_id(job)
    url = f"{base_url.rstrip('/')}{UWS_PATH}/{job}/results/mcutout.tar.gz"
    root = Path(out_dir)
    root.mkdir(parents=True, exist_ok=True)
    target = Path(tempfile.mkdtemp(prefix=f"mcutout-{job}-", dir=root))
    archive = target / "mcutout.tar.gz"

    total = 0
    try:
        with _request(url, bearer=bearer) as response, open(archive, "wb") as out:
            while True:
                chunk = response.read(8 * 1024 * 1024)
                if not chunk:
                    break
                total += len(chunk)
                if total > _MAX_ARCHIVE_BYTES:
                    raise McutoutError(
                        f"the results archive exceeds {_MAX_ARCHIVE_BYTES} bytes")
                out.write(chunk)
        files = extract_archive(archive, target)
    except Exception:
        # A half-written archive or a half-unpacked job is not a result. Take
        # the staging directory with it rather than leaving the caller to
        # wonder which of those files are real.
        shutil.rmtree(target, ignore_errors=True)
        raise
    return {"archive": str(archive), "files": files, "bytes": total}


def extract_archive(archive: Path, target: Path) -> list[str]:
    """Unpack a results tarball, refusing anything that escapes ``target``.

    A tar member may be named ``../../etc/…`` or be a symlink pointing out of
    the directory; extracting one blindly writes wherever it says. And an
    archive that unpacks to a thousand times its size is a way to fill a disk
    with a small download, so the members are counted and the extracted bytes
    are capped — neither of which ``filter="data"`` does.
    """
    extracted: list[str] = []
    target = target.resolve()
    written = 0
    members = 0
    try:
        with tarfile.open(archive, "r:*") as tar:
            for member in tar:          # streamed, not getmembers()
                members += 1
                if members > _MAX_MEMBERS:
                    raise McutoutError(
                        f"the results archive holds more than {_MAX_MEMBERS} entries")
                if not (member.isfile() or member.isdir()):
                    logger.info("[mcutout] skipping %s (%s)", member.name, member.type)
                    continue
                if getattr(member, "sparse", None) is not None:
                    # tarfile does not check that a sparse map agrees with the
                    # declared size, so member.size stops being a bound on what
                    # gets written — which is the bound this loop relies on.
                    raise McutoutError(
                        f"the archive contains a sparse member ({member.name!r}); "
                        "results are expected to be plain files")
                destination = (target / member.name).resolve()
                if not destination.is_relative_to(target):
                    raise McutoutError(
                        f"the archive contains a path outside its directory: {member.name!r}")
                if member.isfile():
                    written += member.size
                    if written > _MAX_EXTRACTED_BYTES:
                        raise McutoutError(
                            f"the results unpack to more than {_MAX_EXTRACTED_BYTES} bytes")
                tar.extract(member, path=target, filter="data")
                if member.isfile():
                    extracted.append(str(destination))
    except tarfile.TarError as exc:
        raise McutoutError(f"the results archive could not be read: {exc}") from exc
    return sorted(extracted)


def resolve_service_url(explicit: str = "") -> str:
    """Where the archive lives: the caller's URL, or the configured default."""
    url = (explicit or os.environ.get("VISIVO_VLKB_URL", "")).strip()
    if not url:
        raise McutoutError(
            "No VLKB service configured: set VISIVO_VLKB_URL or pass service_url.")
    return urljoin(url + "/", ".").rstrip("/")
