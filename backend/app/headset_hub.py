"""headset_hub.py: the headset pages of one session, what the desktop asks of
them and what they report back (issue #236, epic #234).

The selection hub (``selection_hub.py``, #188) carries one thing, a selection
per cube. This hub carries the rest of the conversation between the desktop
and the headset page of the same backend session (the pairing link, the adb
tab or the LAN page all use the desktop's session, #172):

* presence: each page says hello (at start and after any reconnect), states
  what it shows, and is seen whenever it polls for commands; the desktop lists
  the pages seen in the last ``PRESENT_S`` seconds;
* commands, desktop to page: queued per page and handed to its long poll; kept
  until the page acknowledges them by polling with a later ``after``; a
  ``show`` replaces an older ``show`` not yet acknowledged (only the latest
  cube matters);
* activity, page to desktop: a ring of events the desktop long-polls; a
  ``look`` or a ``clip`` replaces the previous one of the same cube (only the
  latest state matters), the rest (products, cube switches) are kept in order.

Order: one counter (``seq``) over commands and events of every session, so a
cursor never goes back. It restarts with the backend: every answer carries
``INSTANCE``, and a cursor above the last seq issued (one from a backend that
was restarted) counts as 0.

Bounds: ``MAX_PAGES_PER_SESSION`` pages (the least recently seen goes);
``MAX_COMMANDS_PER_PAGE`` commands queued (the oldest goes); ``MAX_EVENTS``
events and ``MAX_EVENT_BYTES`` of their payloads per session (the oldest go,
and a desktop whose cursor is behind them is told ``lost``);
``MAX_PAYLOAD_BYTES`` per event; ``MAX_WAITERS_PER_SESSION`` long polls of
each kind at once, and one command poll per page (a newer one ends the older);
``MAX_WAIT_SECONDS`` per long poll. A page silent for ``FORGET_S`` is
forgotten with its commands. The hubs are held by a weak reference to their
session, so a session evicted after its idle time takes its hub along.

Shutdown: uvicorn waits for open requests before it runs the lifespan's
shutdown, with no limit, and a long poll is an open request. Every wait wakes
at least once a second and ends when the server has begun to exit (uvicorn's
signal handler has set ``should_exit``) or the lifespan's shutdown called
``shutdown()``, so a quit is never held by a headset or a desktop waiting.
"""
from __future__ import annotations

import asyncio
import collections
import itertools
import json
import signal
import time
import uuid
import weakref
from dataclasses import dataclass, field
from typing import Any, Awaitable, Callable

#: A page is listed (and takes commands) while seen in the last PRESENT_S
#: seconds; it is forgotten, with its queued commands, after FORGET_S.
PRESENT_S = 45.0
FORGET_S = 300.0
MAX_PAGES_PER_SESSION = 8
MAX_COMMANDS_PER_PAGE = 16
MAX_EVENTS = 256
#: One event's payload (compact JSON), and all the payloads of a session.
MAX_PAYLOAD_BYTES = 16 * 1024
MAX_EVENT_BYTES = 1024 * 1024
MAX_WAITERS_PER_SESSION = 8
#: The longest a long poll waits (seconds).
MAX_WAIT_SECONDS = 25.0
#: Event kinds of which only the latest per cube is kept.
COALESCED_KINDS = ("look", "clip")

#: This backend process (seqs restart at 1 when it restarts).
INSTANCE = uuid.uuid4().hex[:16]

_seq = itertools.count(1)
_last_seq = 0
_closing = False


def next_seq() -> int:
    global _last_seq
    _last_seq = next(_seq)
    return _last_seq


def cursor(after: int) -> int:
    """``after`` as this backend reads it: a cursor above every seq issued
    comes from a backend that has since restarted, and counts as 0."""
    return 0 if after > _last_seq else after


def _server_exiting() -> bool:
    """Whether uvicorn has begun to exit: its handler of SIGTERM/SIGINT is a
    bound method of its ``Server``, whose ``should_exit`` the signal sets.
    Best effort (any other server, or none: False)."""
    try:
        handler = signal.getsignal(signal.SIGTERM)
    except (ValueError, OSError, AttributeError):
        return False
    server = getattr(handler, "__self__", None)
    if server is None or not type(server).__module__.startswith("uvicorn"):
        return False
    return bool(getattr(server, "should_exit", False))


def closing() -> bool:
    return _closing or _server_exiting()


class _Changed:
    """A wake-up for the waiters of one hub: an asyncio.Event made for the
    running loop, replaced at each notify (a hub outlives an event loop in
    the tests, and an Event is bound to the loop it first waited on)."""

    def __init__(self) -> None:
        self._event: asyncio.Event | None = None
        self._loop: asyncio.AbstractEventLoop | None = None

    def event(self) -> asyncio.Event:
        loop = asyncio.get_running_loop()
        if self._event is None or self._loop is not loop:
            self._event, self._loop = asyncio.Event(), loop
        return self._event

    def notify(self) -> None:
        ev, self._event = self._event, None
        if ev is not None:
            ev.set()


@dataclass
class Command:
    seq: int
    type: str
    body: dict[str, Any]

    def info(self) -> dict[str, Any]:
        return {"seq": self.seq, "type": self.type, **self.body}


@dataclass
class Page:
    client: str
    transport: str
    label: str
    since: float                      # wall clock (Unix s) of its first hello
    hello_at: float                   # monotonic, of its latest hello
    last_seen: float                  # monotonic
    last_seen_wall: float             # wall clock (Unix s)
    in_vr: bool = False
    cube: dict[str, Any] | None = None
    staged_cube: dict[str, Any] | None = None
    commands: list[Command] = field(default_factory=list)
    poll_gen: int = 0                 # the latest command poll's number
    polling: int = 0                  # command polls waiting now

    def seen(self) -> None:
        self.last_seen = time.monotonic()
        self.last_seen_wall = time.time()

    def present(self, now: float) -> bool:
        return self.polling > 0 or now - self.last_seen <= PRESENT_S

    def info(self) -> dict[str, Any]:
        return {"client": self.client, "transport": self.transport, "label": self.label,
                "since": self.since, "last_seen": self.last_seen_wall, "in_vr": self.in_vr,
                "cube": self.cube, "staged_cube": self.staged_cube}


@dataclass
class Event:
    seq: int
    client: str
    time: float                       # wall clock (Unix s)
    kind: str
    cube_path: str | None
    summary: str
    payload: dict[str, Any]
    nbytes: int

    def info(self) -> dict[str, Any]:
        return {"seq": self.seq, "client": self.client, "time": self.time, "kind": self.kind,
                "cube_path": self.cube_path, "summary": self.summary, "payload": self.payload}


Disconnected = Callable[[], Awaitable[bool]] | None


class HeadsetHub:
    """The pages, their commands and their activity, of ONE session."""

    def __init__(self) -> None:
        self.pages: dict[str, Page] = {}
        self.events: collections.deque[Event] = collections.deque()
        self.event_bytes = 0
        #: The highest seq dropped by a bound (not by coalescing): a cursor
        #: below it has missed events.
        self.lost_up_to = 0
        self.command_waiters = 0
        self.activity_waiters = 0
        self._changed = _Changed()

    def notify(self) -> None:
        self._changed.notify()

    # ── Presence ────────────────────────────────────────────────────────────

    def _forget_silent(self) -> None:
        now = time.monotonic()
        for c in [c for c, p in self.pages.items() if p.polling == 0 and now - p.last_seen > FORGET_S]:
            del self.pages[c]

    def page(self, client: str) -> Page | None:
        self._forget_silent()
        return self.pages.get(client)

    def hello(self, client: str, transport: str, label: str) -> Page:
        self._forget_silent()
        now, wall = time.monotonic(), time.time()
        p = self.pages.get(client)
        if p is None:
            p = Page(client=client, transport=transport, label=label, since=wall,
                     hello_at=now, last_seen=now, last_seen_wall=wall)
            self.pages[client] = p
            while len(self.pages) > MAX_PAGES_PER_SESSION:
                oldest = min((q for q in self.pages.values() if q is not p),
                             key=lambda q: (q.polling > 0, q.last_seen))
                self._drop_page(oldest.client)
        else:
            p.transport, p.label, p.hello_at = transport, label, now
            p.seen()
        self.notify()
        return p

    def _drop_page(self, client: str) -> bool:
        p = self.pages.pop(client, None)
        if p is None:
            return False
        p.poll_gen += 1               # its waiting poll ends
        self.notify()
        return True

    def remove(self, client: str) -> bool:
        return self._drop_page(client)

    def present_pages(self) -> list[Page]:
        """The pages seen in the last PRESENT_S seconds, newest hello first."""
        self._forget_silent()
        now = time.monotonic()
        return sorted((p for p in self.pages.values() if p.present(now)),
                      key=lambda p: p.hello_at, reverse=True)

    # ── Commands ────────────────────────────────────────────────────────────

    def queue_command(self, client: str | None, type_: str,
                      body: dict[str, Any]) -> tuple[Command, str] | None:
        """Queues a command for ``client`` (or the newest page present):
        (the command, the page's client); None when that page is not present."""
        present = self.present_pages()
        if client is None:
            target = present[0] if present else None
        else:
            target = next((p for p in present if p.client == client), None)
        if target is None:
            return None
        if type_ == "show":
            target.commands = [c for c in target.commands if c.type != "show"]
        cmd = Command(seq=next_seq(), type=type_, body=body)
        target.commands.append(cmd)
        del target.commands[:-MAX_COMMANDS_PER_PAGE]
        self.notify()
        return cmd, target.client

    @staticmethod
    def acknowledge(page: Page, after: int) -> list[Command]:
        """Drops the commands at or below ``after``; the rest, in order."""
        page.commands = [c for c in page.commands if c.seq > after]
        return list(page.commands)

    async def wait_commands(self, page: Page, after: int, timeout: float,
                            disconnected: Disconnected = None) -> list[Command]:
        """The page's commands above ``after`` once there are any, waiting up
        to ``timeout`` seconds. Ends early: empty, without acknowledging,
        when a newer poll of the same page started or the page was removed
        (the newer poll answers for it); empty when the caller left; with
        what there is when the server is exiting. The page counts as seen
        throughout."""
        page.poll_gen += 1
        gen = page.poll_gen
        self.notify()                 # an older poll of this page ends
        page.seen()
        page.polling += 1
        try:
            deadline = time.monotonic() + max(0.0, min(timeout, MAX_WAIT_SECONDS))
            while True:
                if page.poll_gen != gen or self.pages.get(page.client) is not page:
                    return []
                pending = self.acknowledge(page, after)
                remaining = deadline - time.monotonic()
                if pending or remaining <= 0 or closing():
                    return pending
                ev = self._changed.event()
                try:
                    await asyncio.wait_for(ev.wait(), min(remaining, 1.0))
                except asyncio.TimeoutError:
                    if disconnected is not None and await disconnected():
                        return []
        finally:
            page.polling -= 1
            page.seen()

    # ── Activity ────────────────────────────────────────────────────────────

    def add_event(self, client: str, kind: str, cube_path: str | None, summary: str,
                  payload: dict[str, Any], nbytes: int) -> Event:
        if kind in COALESCED_KINDS:
            for old in [e for e in self.events if e.kind == kind and e.cube_path == cube_path]:
                self.events.remove(old)
                self.event_bytes -= old.nbytes
        ev = Event(seq=next_seq(), client=client, time=time.time(), kind=kind,
                   cube_path=cube_path, summary=summary, payload=payload, nbytes=nbytes)
        self.events.append(ev)
        self.event_bytes += nbytes
        while len(self.events) > MAX_EVENTS or (self.event_bytes > MAX_EVENT_BYTES and len(self.events) > 1):
            gone = self.events.popleft()
            self.event_bytes -= gone.nbytes
            self.lost_up_to = max(self.lost_up_to, gone.seq)
        self.notify()
        return ev

    def events_after(self, after: int) -> list[Event]:
        return [e for e in self.events if e.seq > after]

    async def wait_events(self, after: int, timeout: float,
                          disconnected: Disconnected = None) -> list[Event]:
        """The events above ``after`` once there are any, waiting up to
        ``timeout`` seconds (ends early when the caller left or the server
        is exiting)."""
        deadline = time.monotonic() + max(0.0, min(timeout, MAX_WAIT_SECONDS))
        while True:
            found = self.events_after(after)
            remaining = deadline - time.monotonic()
            if found or remaining <= 0 or closing():
                return found
            ev = self._changed.event()
            try:
                await asyncio.wait_for(ev.wait(), min(remaining, 1.0))
            except asyncio.TimeoutError:
                if disconnected is not None and await disconnected():
                    return []


# Session -> hub, held weakly: a session evicted from the registry takes its
# pages, commands and activity with it.
_HUBS: "weakref.WeakKeyDictionary[Any, HeadsetHub]" = weakref.WeakKeyDictionary()


def hub_for(session: Any) -> HeadsetHub:
    hub = _HUBS.get(session)
    if hub is None:
        hub = HeadsetHub()
        _HUBS[session] = hub
    return hub


def shutdown() -> None:
    """The lifespan's shutdown: every long poll ends now (with what it has)."""
    global _closing
    _closing = True
    for hub in list(_HUBS.values()):
        hub.notify()


def reopen() -> None:
    """The lifespan's start (and a test after ``shutdown``)."""
    global _closing
    _closing = False


def compact_json(value: Any) -> str:
    """``value`` as stored and answered: compact JSON of finite numbers
    (ValueError otherwise, RecursionError when nested too deep)."""
    return json.dumps(value, separators=(",", ":"), allow_nan=False)
