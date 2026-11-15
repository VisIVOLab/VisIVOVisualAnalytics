"""headset_spectator.py: the headset's view on the desktop (issue #281, epic
#278), the "spectator view".

The desktop watches one headset page of its session: the page captures a
small copy of one eye (with the menu, the wrist cards and the ray) a few
times a second and posts it as a JPEG; the desktop reads the latest one. Only
the latest frame is kept: a slow reader skips frames, nothing queues.

* Watching is the desktop's choice, off by default. Starting it queues a
  ``watch`` command for the page (the hub's command queue, so it reaches the
  page through its usual long poll), stopping it queues ``watch`` off.
* A lease keeps it alive: every frame read renews it for ``LEASE_S``. A
  desktop that goes away without saying so stops being served after that:
  the watch ends (``lapse``: not watched, the frame dropped), a later read
  answers 409 instead of reviving it, and the page, whose next frame post is
  answered ``watched: false``, stops capturing.
* A page that says hello again (a reload, a reconnect) while watched gets
  its ``watch`` command again.

Bounds: a frame of at most ``MAX_FRAME_BYTES``; at most ``MAX_FPS`` frames a
second are stored (a faster one is refused, 429); the frame read waits at
most ``MAX_WAIT_S``; at most ``MAX_WAITERS_PER_SESSION`` reads wait at once
per session (the hub's bound). A feed lives on its hub page, so a page that is
forgotten or dropped takes its frame with it.
"""
from __future__ import annotations

import asyncio
import time
from dataclasses import dataclass, field
from typing import Any

from . import headset_hub as H

#: A frame (JPEG) at most, in bytes.
MAX_FRAME_BYTES = 512 * 1024
#: Frames stored per second at most (a page faster than this is refused).
MAX_FPS = 30
#: The shortest gap between two stored frames (seconds), with 20 % for jitter.
MIN_FRAME_GAP_S = 0.8 / MAX_FPS
#: What the desktop may ask for, and what it gets when it does not say.
DEFAULT_FPS = 15
MIN_WIDTH, DEFAULT_WIDTH, MAX_WIDTH = 160, 512, 960
#: A frame's declared size at most (a portrait eye: up to twice as tall as wide).
MAX_FRAME_SIDE = 2 * MAX_WIDTH
#: Watching ends this long after the desktop's last frame read or start.
LEASE_S = 10.0
#: The frame read's longest wait (seconds).
MAX_WAIT_S = 5.0
#: The eyes a frame may come from ('mono': the desktop browser's single view).
EYES = ("left", "right", "mono")


@dataclass
class Frame:
    seq: int
    jpeg: bytes
    width: int
    height: int
    eye: str
    cost_ms: float | None             # the headset's capture cost per frame, as it measured it
    time: float                       # wall clock (Unix s) when the backend got it
    received: float                   # monotonic


@dataclass
class Feed:
    """One page's spectator state."""
    watching: bool = False
    lease_until: float = 0.0          # monotonic
    fps: int = DEFAULT_FPS
    width: int = DEFAULT_WIDTH
    frame: Frame | None = None
    seq: int = 0                      # the last frame's number (per page; restarts with the backend)
    refused: int = 0                  # frames refused by the rate cap
    changed: H._Changed = field(default_factory=H._Changed)

    def live(self, now: float | None = None) -> bool:
        """Watched, and the desktop was heard from within the lease."""
        return self.watching and (now if now is not None else time.monotonic()) <= self.lease_until

    def renew(self) -> None:
        self.lease_until = time.monotonic() + LEASE_S

    def watch_body(self) -> dict[str, Any]:
        return {"on": True, "fps": self.fps, "width": self.width}


def lapse(feed: Feed) -> bool:
    """A watch whose lease ran out ends here: not watched, its frame dropped
    (so no snapshot saves a stale one, and no read revives it). True when it
    ended now."""
    if feed.watching and not feed.live():
        feed.watching, feed.lease_until, feed.frame = False, 0.0, None
        feed.changed.notify()
        return True
    return False


def feed_of(page: H.Page) -> Feed:
    if page.feed is None:
        page.feed = Feed()
    return page.feed


def cursor(feed: Feed, after: int) -> int:
    """``after`` as this feed reads it: above its last frame (a backend that
    has since restarted) counts as 0."""
    return 0 if after > feed.seq else after


def start(hub: H.HeadsetHub, page: H.Page, fps: int, width: int) -> Feed:
    """The desktop starts (or retunes) watching ``page``: the lease starts and
    the page gets a ``watch`` command."""
    feed = feed_of(page)
    feed.watching, feed.fps, feed.width = True, fps, width
    feed.renew()
    hub.queue_command(page.client, "watch", feed.watch_body())
    return feed


def stop(hub: H.HeadsetHub, page: H.Page) -> Feed:
    """The desktop stops watching: the frame goes and the page is told."""
    feed = feed_of(page)
    feed.watching, feed.lease_until, feed.frame = False, 0.0, None
    hub.queue_command(page.client, "watch", {"on": False})
    feed.changed.notify()
    return feed


def on_hello(hub: H.HeadsetHub, client: str) -> None:
    """A page said hello (again): watched, it is told to capture again (a
    reloaded page starts unwatched)."""
    page = hub.pages.get(client)
    if page is not None and page.feed is not None and page.feed.live():
        hub.queue_command(client, "watch", page.feed.watch_body())


def add_frame(feed: Feed, jpeg: bytes, width: int, height: int, eye: str,
              cost_ms: float | None) -> Frame | None:
    """Stores a frame as the latest; None when it comes sooner than the rate
    cap allows (it is dropped)."""
    now = time.monotonic()
    if feed.frame is not None and now - feed.frame.received < MIN_FRAME_GAP_S:
        feed.refused += 1
        return None
    feed.seq += 1
    feed.frame = Frame(seq=feed.seq, jpeg=jpeg, width=width, height=height, eye=eye,
                       cost_ms=cost_ms, time=time.time(), received=now)
    feed.changed.notify()
    return feed.frame


async def wait_frame(hub: H.HeadsetHub, page: H.Page, after: int, timeout: float,
                     disconnected: H.Disconnected = None) -> Frame | None:
    """The latest frame once it is above ``after``, waiting up to ``timeout``
    seconds; None when there is none in time, the watch ended, the page was
    dropped, the caller left or the server is exiting. Renews the lease."""
    feed = feed_of(page)
    deadline = time.monotonic() + max(0.0, min(timeout, MAX_WAIT_S))
    while True:
        if hub.pages.get(page.client) is not page or lapse(feed) or not feed.watching:
            return None
        feed.renew()
        frame = feed.frame
        if frame is not None and frame.seq > after:
            return frame
        remaining = deadline - time.monotonic()
        if remaining <= 0 or H.closing():
            return None
        ev = feed.changed.event()
        try:
            await asyncio.wait_for(ev.wait(), min(remaining, 1.0))
        except asyncio.TimeoutError:
            if disconnected is not None and await disconnected():
                return None


def jpeg_size(data: bytes) -> tuple[int, int] | None:
    """(width, height) from a JPEG's frame header (SOF), read up to its first
    scan; None when the bytes are not a JPEG with exactly one frame header
    before that scan. Two frame headers are refused: a reader that takes the
    later one (Pillow does) would see another size than the one checked here."""
    if len(data) < 4 or data[:3] != b"\xff\xd8\xff":
        return None
    i, n = 2, len(data)
    size: tuple[int, int] | None = None
    while i + 4 <= n:
        if data[i] != 0xFF:
            return None
        marker = data[i + 1]
        if marker == 0xFF:                # fill byte
            i += 1
            continue
        if marker in (0xD8, 0x01) or 0xD0 <= marker <= 0xD7:
            i += 2
            continue
        if marker == 0xD9:                # end of image before any scan
            return None
        length = int.from_bytes(data[i + 2:i + 4], "big")
        if length < 2 or i + 2 + length > n:
            return None
        # SOF0..SOF15 except DHT (C4), JPG (C8) and DAC (CC).
        if 0xC0 <= marker <= 0xCF and marker not in (0xC4, 0xC8, 0xCC):
            if size is not None or length < 7:
                return None               # a second frame header, or a short one
            h = int.from_bytes(data[i + 5:i + 7], "big")
            w = int.from_bytes(data[i + 7:i + 9], "big")
            if w <= 0 or h <= 0:
                return None
            size = (w, h)
        elif marker == 0xDA:              # the first scan: the header ends here
            return size
        i += 2 + length
    return None


def decoded_size(data: bytes) -> tuple[int, int] | None:
    """The size Pillow reads from a JPEG's header (nothing is decoded), or
    None when it does not read it as a JPEG."""
    import io  # noqa: PLC0415
    import warnings  # noqa: PLC0415

    from PIL import Image, UnidentifiedImageError  # noqa: PLC0415

    try:
        # Pillow's own bomb warning (or error, past twice its limit) says
        # nothing more than the size the caller compares with its bounds.
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", Image.DecompressionBombWarning)
            with Image.open(io.BytesIO(data), formats=["JPEG"]) as im:
                return im.size
    except (UnidentifiedImageError, Image.DecompressionBombError, OSError, ValueError, SyntaxError):
        return None
