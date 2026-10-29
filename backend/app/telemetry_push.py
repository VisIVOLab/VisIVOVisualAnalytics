"""The node reports its own state to its SKAVA catalogue.

A catalogue ranks replicas on what it measures at each node, and by default it
measures by probing the node's ``/v1/ping`` and ``/v1/node/status``. A node the
catalogue cannot reach (behind a firewall, or a NAT) would then stay
"unmeasured" for ever. Such a node pushes instead: every few seconds it posts
its status document to the catalogue's ``POST /internal/telemetry/report``,
with its own node token, and the catalogue stops probing it while the
reports are fresh. The token speaks for this node only; the catalogue's
operator prints it with ``python -m app.security.node_token CODE``.

Off unless configured::

    VISIVO_SKAVA_TELEMETRY_URL       the catalogue's base URL (http or https)
    VISIVO_SKAVA_NODE_TOKEN          this node's telemetry token from that catalogue
    VISIVO_NODE_CODE                 this node's code in that catalogue
    VISIVO_SKAVA_TELEMETRY_INTERVAL  seconds between reports (default 10)

The round-trip time sent with each report is the one measured on the previous
report's POST, so the catalogue's network figure for the node is a real
round trip, not a guess.
"""
from __future__ import annotations

import asyncio
import json
import logging
import os
import re
import ssl
import time
from dataclasses import dataclass
from typing import Any, Awaitable, Callable
from urllib.parse import urlparse

logger = logging.getLogger("visivo.telemetry_push")

DEFAULT_INTERVAL = 10.0
MIN_INTERVAL = 1.0
TIMEOUT = 5.0            # the whole exchange: connect, send, status line
_TOKEN = re.compile(r"[A-Za-z0-9._~+/=-]{16,256}")


@dataclass(frozen=True)
class PushConfig:
    report_url: str
    key: str
    node_code: str
    interval: float


def config_from_env(env: dict[str, str] | None = None) -> PushConfig | None:
    """The push configuration, or None when the node is not set up to push."""
    env = os.environ if env is None else env
    base = (env.get("VISIVO_SKAVA_TELEMETRY_URL") or "").strip().rstrip("/")
    key = (env.get("VISIVO_SKAVA_NODE_TOKEN") or "").strip()
    code = (env.get("VISIVO_NODE_CODE") or "").strip()
    if not base:
        return None
    if not key or not code:
        logger.warning("[telemetry push] VISIVO_SKAVA_TELEMETRY_URL is set but "
                       "VISIVO_SKAVA_NODE_TOKEN or VISIVO_NODE_CODE is not: not pushing")
        return None
    if not _TOKEN.fullmatch(key):
        # Never echo it: a malformed token is still a secret.
        logger.warning("[telemetry push] VISIVO_SKAVA_NODE_TOKEN is malformed: not pushing")
        return None
    parsed = urlparse(base)
    if (parsed.scheme not in ("http", "https") or not parsed.hostname or "@" in parsed.netloc
            or any(c.isspace() or ord(c) < 32 or ord(c) > 126 for c in base)):
        logger.warning("[telemetry push] VISIVO_SKAVA_TELEMETRY_URL must be a plain http(s) URL: not pushing")
        return None
    try:
        interval = float(env.get("VISIVO_SKAVA_TELEMETRY_INTERVAL") or DEFAULT_INTERVAL)
    except ValueError:
        interval = DEFAULT_INTERVAL
    if not interval == interval or interval in (float("inf"), float("-inf")):   # NaN, inf
        interval = DEFAULT_INTERVAL
    return PushConfig(report_url=f"{base}/internal/telemetry/report", key=key,
                      node_code=code, interval=max(MIN_INTERVAL, interval))


class PushError(RuntimeError):
    """The catalogue did not take the report; the message never quotes the request."""


def _request_bytes(cfg: PushConfig, body: bytes) -> tuple[str, int, bool, bytes]:
    url = urlparse(cfg.report_url)
    secure = url.scheme == "https"
    port = url.port or (443 if secure else 80)
    path = url.path + (f"?{url.query}" if url.query else "")
    head = (f"POST {path} HTTP/1.1\r\n"
            f"Host: {url.netloc}\r\n"
            "Content-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\n"
            f"X-Skava-Node-Token: {cfg.key}\r\n"
            "Connection: close\r\n\r\n")
    return url.hostname or "", port, secure, head.encode("ascii") + body


def _status_code(line: bytes) -> int:
    parts = line.split(b" ", 2)
    if len(parts) < 2 or not parts[0].startswith(b"HTTP/") or not parts[1].strip().isdigit():
        raise PushError("not an HTTP response")
    return int(parts[1])


async def post_report(cfg: PushConfig, status: dict[str, Any], rtt_ms: float | None) -> float:
    """POST one report; returns the round-trip time in ms. Raises on any failure.

    One deadline (``TIMEOUT``) covers the whole exchange, and cancelling the
    caller ends it at once: no thread outlives the push loop. Only the status
    line of the answer is read, and a redirect is a failure, never followed,
    since the request carries the node's token.
    """
    payload = {"node_code": cfg.node_code, "status": status}
    if rtt_ms is not None:
        payload["rtt_ms"] = round(rtt_ms, 3)
    host, port, secure, request = _request_bytes(cfg, json.dumps(payload).encode())

    async def exchange() -> int:
        reader, writer = await asyncio.open_connection(
            host, port, ssl=ssl.create_default_context() if secure else None)
        try:
            writer.write(request)
            await writer.drain()
            while True:
                code = _status_code(await reader.readline())
                if not 100 <= code < 200:
                    return code
                # An interim answer (100 Continue, 103 Early Hints): skip its
                # header block and read on for the final one.
                while (await reader.readline()).strip():
                    pass
        finally:
            writer.close()

    started = time.perf_counter()
    try:
        code = await asyncio.wait_for(exchange(), timeout=TIMEOUT)
    except TimeoutError:
        raise PushError(f"no answer within {TIMEOUT:g} s") from None
    if not 200 <= code < 300:
        raise PushError(f"HTTP {code}")
    return (time.perf_counter() - started) * 1000.0


async def run(cfg: PushConfig, collect: Callable[[], Awaitable[dict[str, Any]]],
              *, post: Callable[[PushConfig, dict[str, Any], float | None], Awaitable[float]] = post_report,
              max_reports: int | None = None) -> None:
    """Report every ``cfg.interval`` seconds until cancelled (or ``max_reports``)."""
    rtt_ms: float | None = None
    failing = False
    sent = 0
    while max_reports is None or sent < max_reports:
        try:
            status = await collect()
            rtt_ms = await post(cfg, status, rtt_ms)
            if failing:
                logger.info("[telemetry push] reports to %s accepted again", cfg.report_url)
            failing = False
        except asyncio.CancelledError:
            raise
        except Exception as exc:  # noqa: BLE001 - a failed report must not stop the loop
            rtt_ms = None
            if not failing:   # once per outage, not every interval
                logger.warning("[telemetry push] report to %s failed: %s", cfg.report_url, _describe(exc))
            failing = True
        sent += 1
        if max_reports is None or sent < max_reports:
            await asyncio.sleep(cfg.interval)


def _describe(exc: Exception) -> str:
    """What went wrong, without text that could quote the request's headers."""
    if isinstance(exc, PushError):
        return str(exc)
    return type(exc).__name__


def start(app_version: str) -> asyncio.Task | None:
    """Start the push loop if configured; the caller cancels the task on shutdown."""
    cfg = config_from_env()
    if cfg is None:
        return None
    from .routers.node import collect_status  # noqa: PLC0415 - avoids an import cycle

    logger.info("[telemetry push] reporting node %s to %s every %.0f s",
                cfg.node_code, cfg.report_url, cfg.interval)
    return asyncio.create_task(run(cfg, lambda: collect_status(app_version)),
                               name="visivo-telemetry-push")
