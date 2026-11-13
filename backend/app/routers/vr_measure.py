"""The VR page's measurement records (issue #224): ``POST /v1/vr/measurements``
appends a batch of the page's samples and events to one JSON-lines file per
record, in ``VISIVO_VR_MEASURE_DIR`` (default ``~/.visivo/vr-measurements``).

The record is for a paper's measurements, so it is kept as the page sent it,
with three guarantees:

- nothing secret is written: every string is scrubbed with the same rules as
  the page's log (``vr.scrub``: tokens, render-session ids, directory paths);
- sizes are bounded: the request body, the lines per request, a line's size
  and depth, a record file, and the directory (no new record once it holds
  ``MAX_DIR_BYTES``);
- a batch is written once: each carries its number (``seq``), written into
  its lines as ``q``; a batch resent after a dropped answer (the pairing's
  link resumed, a timeout) is acknowledged and not written again, also after
  a backend restart (the last ``q`` is read back from the file).

A failure to write (a full disk, a directory that cannot be written, a
record over its size) answers 507 and never raises into the page's session:
the page stops sending this record and carries on.
"""
from __future__ import annotations

import asyncio
import hashlib
import json
import logging
import math
import os
import re
import threading
import time
from collections import OrderedDict
from pathlib import Path

from fastapi import APIRouter, Depends, HTTPException, Request

from ..auth import accepted_tokens, verify_token
from .vr import scrub

router = APIRouter(tags=["vr"])
logger = logging.getLogger("visivo.vr")

#: Request body (bytes), lines per request, one line's serialized size.
MAX_BODY = 256 * 1024
MAX_LINES = 120
MAX_LINE_BYTES = 32 * 1024
#: Nesting depth, items per list, characters per string, characters per key.
MAX_DEPTH = 6
MAX_LIST = 512
MAX_STRING = 2000
_KEY = re.compile(r"[A-Za-z0-9_.,* -]{1,64}")
#: A record file, and the directory (checked when a record starts).
MAX_FILE_BYTES = 64 * 1024 * 1024
MAX_DIR_BYTES = 2 * 1024 * 1024 * 1024
#: Per token, per sliding minute.
RATE_REQUESTS = 40
RATE_LINES = 4000
_WINDOW_S = 60.0

RECORD_ID = re.compile(r"[0-9]{8}-[0-9]{6}-[0-9a-f]{8}")
KINDS = ("h", "s", "e")


def measure_dir() -> Path:
    """Where the records are written (read at each request, so a test or the
    desktop's settings can change it)."""
    d = os.environ.get("VISIVO_VR_MEASURE_DIR", "").strip()
    return Path(d).expanduser() if d else Path.home() / ".visivo" / "vr-measurements"


def record_file(record: str) -> str:
    return f"vr-measure-{record}.jsonl"


class _Rate:
    """A sliding minute of requests and lines per token (by hash)."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._seen: dict[str, list[tuple[float, int]]] = {}

    def allow(self, key: str, lines: int, now: float | None = None) -> tuple[bool, float]:
        now = time.monotonic() if now is None else now
        with self._lock:
            hist = [(t, n) for t, n in self._seen.get(key, []) if now - t < _WINDOW_S]
            if len(hist) >= RATE_REQUESTS or sum(n for _, n in hist) + lines > RATE_LINES:
                self._seen[key] = hist
                return False, max(1.0, _WINDOW_S - (now - hist[0][0]) if hist else _WINDOW_S)
            hist.append((now, lines))
            self._seen[key] = hist
            if len(self._seen) > 256:
                for k in [k for k, v in self._seen.items() if not v or now - v[-1][0] >= _WINDOW_S]:
                    self._seen.pop(k, None)
            return True, 0.0


RATE = _Rate()


class BadLine(ValueError):
    pass


def clean(value, secrets: tuple[str, ...], depth: int = 0):
    """A line's value made safe to write: strings scrubbed, numbers finite
    (else null), keys checked, depth and sizes bounded (BadLine beyond)."""
    if depth > MAX_DEPTH:
        raise BadLine("nested too deep")
    if value is None or isinstance(value, bool):
        return value
    if isinstance(value, int):
        return value if abs(value) < 2 ** 53 else None
    if isinstance(value, float):
        return value if math.isfinite(value) else None
    if isinstance(value, str):
        if len(value) > MAX_STRING:
            raise BadLine("a string is too long")
        return scrub(value, secrets) if value else value
    if isinstance(value, list):
        if len(value) > MAX_LIST:
            raise BadLine("a list is too long")
        # Lists of numbers (frame intervals) are the bulk: checked without recursion.
        if all(type(v) in (int, float) for v in value):
            return [v if (type(v) is int and abs(v) < 2 ** 53) or (type(v) is float and math.isfinite(v)) else None
                    for v in value]
        return [clean(v, secrets, depth + 1) for v in value]
    if isinstance(value, dict):
        if len(value) > MAX_LIST:
            raise BadLine("an object has too many keys")
        out = {}
        for k, v in value.items():
            if not isinstance(k, str) or not _KEY.fullmatch(k):
                raise BadLine("a key is not allowed")
            out[k] = clean(v, secrets, depth + 1)
        return out
    raise BadLine("a value of an unknown type")


def clean_lines(lines, secrets: tuple[str, ...]) -> list[dict]:
    out = []
    for line in lines:
        if not isinstance(line, dict) or line.get("k") not in KINDS:
            raise BadLine('every line is an object with "k" one of "h", "s", "e"')
        # As the page sent it (scrubbing may shorten it).
        if len(json.dumps(line, separators=(",", ":"), ensure_ascii=False)) > MAX_LINE_BYTES:
            raise BadLine(f"a line is over {MAX_LINE_BYTES} bytes")
        out.append(clean(line, secrets))
    return out


# ── Writing ─────────────────────────────────────────────────────────────────

_locks: dict[str, threading.Lock] = {}
_locks_guard = threading.Lock()
#: The last batch written per record (the least recently used are dropped;
#: the file is read again for one that is not here).
_last_seq: OrderedDict[str, int] = OrderedDict()
_MAX_TRACKED = 256
_warned: set[str] = set()


def _lock_for(record: str) -> threading.Lock:
    with _locks_guard:
        lock = _locks.get(record)
        if lock is None:
            if len(_locks) > 4 * _MAX_TRACKED:
                for k in [k for k, v in _locks.items() if not v.locked()][: len(_locks) // 2]:
                    _locks.pop(k, None)
            lock = _locks[record] = threading.Lock()
        return lock


#: How far back the last committed batch is looked for (a batch is at most
#: MAX_BODY bytes as sent, a little more as written).
_TAIL = 2 * MAX_BODY + 64 * 1024


def _recover(path: Path) -> int:
    """The number of the last batch written whole (its last line carries
    ``qn``), after cutting off a batch that was only partly written (a crash
    or a full disk in the middle of an append); -1 when there is none.
    StoreError when the file cannot be read or cut: nothing is appended then
    (it could write a batch already in the file a second time)."""
    try:
        with open(path, "r+b") as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            start = max(0, size - _TAIL)
            f.seek(start)
            tail = f.read()
            parts = tail.split(b"\n")          # the last part: a torn line, or b""
            pos = len(tail) - len(parts[-1])     # the end of the last complete line
            # From the end: the last line with "qn" closes the last whole batch.
            for raw in reversed(parts[:-1]):
                line_end = pos
                pos -= len(raw) + 1
                try:
                    line = json.loads(raw)
                except ValueError:
                    continue                     # the tail's first, cut line
                if isinstance(line, dict) and isinstance(line.get("qn"), int) and isinstance(line.get("q"), int):
                    if start + line_end < size:
                        f.truncate(start + line_end)
                    return line["q"]
            if start == 0 and size:
                f.truncate(0)          # nothing whole in the file
    except OSError as exc:
        raise StoreError(f"cannot read the record back ({exc.strerror or type(exc).__name__})") from None
    return -1


def _dir_bytes(d: Path) -> int:
    total = 0
    try:
        for e in os.scandir(d):
            if e.name.endswith(".jsonl") and e.is_file(follow_symlinks=False):
                total += e.stat().st_size
    except OSError:
        pass
    return total


class StoreError(Exception):
    pass


def write_batch(record: str, seq: int, lines: list[dict]) -> dict:
    """Appends the batch (once) to the record's file; StoreError when it
    cannot be stored."""
    d = measure_dir()
    path = d / record_file(record)
    with _lock_for(record):
        last = _last_seq.get(record)
        if last is None:
            last = _recover(path) if path.exists() else -1
        if seq <= last:
            _last_seq[record] = last
            _last_seq.move_to_end(record)
            return {"accepted": 0, "duplicate": True, "file": path.name}
        # Every line carries its batch number; the batch's last line also its
        # length (qn), which marks the batch as written whole.
        # The page's own q and qn, if any, are dropped: they are the server's.
        payload = "".join(json.dumps({**{k: v for k, v in line.items() if k not in ("q", "qn")}, "q": seq,
                                      **({"qn": len(lines)} if i == len(lines) - 1 else {})},
                                     separators=(",", ":"), ensure_ascii=False) + "\n"
                          for i, line in enumerate(lines)).encode("utf-8")
        size = None                               # known once the append may start
        try:
            d.mkdir(parents=True, exist_ok=True)
            exists = path.exists()
            current = path.stat().st_size if exists else 0
            if not exists and _dir_bytes(d) + len(payload) > MAX_DIR_BYTES:
                raise StoreError(f"the measurement directory holds more than {MAX_DIR_BYTES // 2**30} GiB")
            if current + len(payload) > MAX_FILE_BYTES:
                raise StoreError(f"the record is over {MAX_FILE_BYTES // 2**20} MiB")
            # Append-only, opened for this batch (a disk that fills is seen at once).
            size = current
            with open(path, "ab") as f:
                f.write(payload)
                f.flush()
            if not exists:
                logger.info("[vr] measurement record %s started in %s", path.name, d)
        except OSError as exc:
            # A part of the batch may be in the file: cut it off again, so the
            # batch sent again is written whole.
            # Only once the append started (a failure before it wrote nothing).
            if size is not None:
                try:
                    if path.stat().st_size > size:
                        os.truncate(path, size)
                except OSError:
                    _last_seq.pop(record, None)   # read back (and cut) at the next batch
            if record not in _warned:
                _warned.add(record)
                logger.warning("[vr] measurement record %s not stored: %s", path.name, exc.strerror or type(exc).__name__)
            raise StoreError(f"cannot write the record ({exc.strerror or type(exc).__name__})") from None
        _last_seq[record] = seq
        _last_seq.move_to_end(record)
        while len(_last_seq) > _MAX_TRACKED:
            _last_seq.popitem(last=False)
        return {"accepted": len(lines), "file": path.name, "bytes": size + len(payload)}


async def _read_capped(request: Request) -> bytes:
    declared = request.headers.get("content-length")
    if declared is not None:
        try:
            if int(declared) > MAX_BODY:
                raise HTTPException(status_code=413, detail=f"At most {MAX_BODY} bytes per request.")
        except ValueError:
            raise HTTPException(status_code=400, detail="Bad Content-Length.") from None
    body = bytearray()
    async for chunk in request.stream():
        body += chunk
        if len(body) > MAX_BODY:
            raise HTTPException(status_code=413, detail=f"At most {MAX_BODY} bytes per request.")
    return bytes(body)


@router.post("/v1/vr/measurements")
async def vr_measurements(request: Request, token: str = Depends(verify_token)) -> dict:
    """Appends a batch of a measurement record: ``{"record": "<YYYYMMDD-
    HHMMSS-8 hex>", "seq": <batch number>, "lines": [<header, sample or
    event objects>]}``. Answers ``{"accepted", "file", "bytes"}`` (the
    file's name, never its directory), or ``"duplicate": true`` for a batch
    already written. 413/422 for a body or a line out of bounds, 429 beyond
    the rate, 507 when the record cannot be stored."""
    raw = await _read_capped(request)
    try:
        body = json.loads(raw or b"{}")
    except (ValueError, UnicodeDecodeError):
        raise HTTPException(status_code=422, detail="The body is not JSON.") from None
    if not isinstance(body, dict):
        raise HTTPException(status_code=422, detail="The body is not an object.")
    record, seq, lines = body.get("record"), body.get("seq"), body.get("lines")
    if not isinstance(record, str) or not RECORD_ID.fullmatch(record):
        raise HTTPException(status_code=422, detail="record: YYYYMMDD-HHMMSS-<8 hex digits>.")
    if not isinstance(seq, int) or isinstance(seq, bool) or not 0 <= seq < 10_000_000:
        raise HTTPException(status_code=422, detail="seq: the batch number, an integer from 0.")
    if not isinstance(lines, list) or not 1 <= len(lines) <= MAX_LINES:
        raise HTTPException(status_code=422, detail=f"lines: 1 to {MAX_LINES} objects.")
    key = hashlib.sha256(token.encode()).hexdigest()[:16]
    ok, retry = RATE.allow(key, len(lines))
    if not ok:
        raise HTTPException(status_code=429, detail="Too many measurement lines: slow down.",
                            headers={"Retry-After": str(int(retry + 0.999))})
    secrets = tuple(accepted_tokens()) + (token,)

    def run() -> dict:
        return write_batch(record, seq, clean_lines(lines, secrets))

    # In a thread: scrubbing and the file write must never hold the event loop.
    try:
        return await asyncio.to_thread(run)
    except BadLine as exc:
        raise HTTPException(status_code=422, detail=f"Bad line: {exc}.") from None
    except StoreError as exc:
        raise HTTPException(status_code=507, detail=str(exc)) from None
