"""A render session in a process of its own.

VTK's ``Render()`` and Pillow's JPEG encoder hold the GIL. With the session in
the backend's process, every frame froze the event loop that receives the
client's commands and sends the frames, so a drag ran at the speed of the
render, the encode and the network one after the other (about 10 FPS on a V100
through an SSH tunnel, with the GPU idle half the time). In a child process the
render holds only the child's GIL: the backend keeps receiving commands and
sending the previous frame while the next one is made. A crash inside VTK or
the driver also ends that session, not the backend.

:class:`RenderSessionProcess` is the main-process side: the API the router and
the manager use (``initialize``, ``apply_command``, ``render_frame``,
``render_frame_if_changed``, ``render_rgb_frame``, ``consume_idr_flag``,
``close``, ``changed``, ``stats``, ``view``), forwarded over a pipe to
:func:`_serve`, which runs the real session in the child. Commands are sent
without waiting for a reply: the pipe is ordered, so the child has applied
them before it renders the next frame asked for.
"""
from __future__ import annotations

import asyncio
import atexit
import collections
import logging
import multiprocessing
# Imported before the exit hook below is registered: it registers its own hook
# (which joins non-daemon children) on import, and hooks run in reverse order,
# so ours, which ends the children, runs first.
import multiprocessing.util  # noqa: F401
import queue
import threading
import time
import weakref
from dataclasses import asdict
from typing import Any

from ._common import SessionStats

logger = logging.getLogger("visivo.render")

# The one message that is not answered.
_COMMAND = "cmd"

# Children still running, ended at interpreter exit: multiprocessing joins
# non-daemon children there, and one waiting on its pipe would never return.
_LIVE: weakref.WeakSet = weakref.WeakSet()


@atexit.register
def _end_children() -> None:
    for proxy in list(_LIVE):
        proxy._release()


class _View:
    """The part of the session's view state the main process reads."""

    def __init__(self) -> None:
        self.width = 0
        self.height = 0


class RenderSessionProcess:
    """A :class:`RenderSession` running in a child process; see the module doc."""

    def __init__(self, session_id: str, dataset_path: str, *,
                 factory: str = "app.render_session:RenderSession") -> None:
        self.session_id = session_id
        self.dataset_path = dataset_path
        self.owner_token: str | None = None
        self.stats = SessionStats()
        self.view = _View()
        self.changed = asyncio.Event()
        self._factory = factory
        # Two states, not one: a session stops taking requests when it is
        # closed or its child is gone, but its process, pipe and threads are
        # released once, whichever way it ended.
        self._refusing = False
        self._released = False
        # Release runs once; a second caller waits for the first to finish.
        self._release_lock = threading.Lock()
        self._release_started = False
        self._release_done = threading.Event()
        self._process: multiprocessing.process.BaseProcess | None = None
        self._conn = None
        self._pending: collections.deque[asyncio.Future] = collections.deque()
        # Outgoing messages, written by a thread of their own: a blocking pipe
        # write (a child busy rendering, a full buffer) must never stall the
        # event loop. The lock keeps the pending answers in the order of the
        # requests in the queue.
        self._outbox: queue.Queue = queue.Queue()
        self._order_lock = threading.Lock()
        self._loop: asyncio.AbstractEventLoop | None = None
        # The child's key-frame flag, returned with each RGB frame.
        self._idr = False

    # ── Lifecycle ────────────────────────────────────────────────────────

    async def initialize(self) -> None:
        """Start the child and initialise the session in it."""
        self._loop = asyncio.get_running_loop()
        ctx = multiprocessing.get_context("spawn")
        parent, child = ctx.Pipe(duplex=True)
        # Not a daemon: the session starts a reader process of its own for
        # large cubes, and daemonic processes may not have children. close()
        # and the exit hook end it instead.
        self._process = ctx.Process(
            target=_serve, args=(child, self.session_id, self.dataset_path, self._factory),
            name=f"visivo-render-{self.session_id[:8]}", daemon=False,
        )
        self._process.start()
        _LIVE.add(self)
        child.close()
        self._conn = parent
        for target, role in ((self._write_messages, "send"), (self._read_replies, "reply")):
            threading.Thread(target=target, name=f"render-{role}-{self.session_id[:8]}",
                             daemon=True).start()
        await self._request("init")

    async def close(self) -> None:
        if self._released:
            return
        try:
            if not self._refusing and self._process is not None and self._process.is_alive():
                try:
                    await asyncio.wait_for(self._request("close"), timeout=10)
                except Exception:
                    logger.warning("[render-process %s] close did not answer",
                                   self.session_id[:8])
        finally:
            # Released even if this close is cancelled: a stalled child must
            # not outlive its session.
            self._refusing = True
            await asyncio.shield(asyncio.get_running_loop().run_in_executor(None, self._release))

    def _release(self) -> None:
        """End the child (it exits after close, else it is terminated) and free
        the pipe and the writer. Idempotent; never on the event loop thread
        while the child may still be running."""
        with self._release_lock:
            first = not self._release_started
            self._release_started = True
        if not first:
            self._release_done.wait(timeout=15)
            return
        self._refusing = True
        try:
            proc = self._process
            if proc is not None:
                proc.join(timeout=5)
                if proc.is_alive():
                    proc.terminate()
                    proc.join(timeout=5)
            self._outbox.put(None)   # ends the writer
            try:
                if self._conn is not None:
                    self._conn.close()
            except Exception:
                pass
            _LIVE.discard(self)
        finally:
            self._released = True
            self._release_done.set()

    # ── The session API ──────────────────────────────────────────────────

    async def apply_command(self, command: dict[str, Any]) -> None:
        """Send a command; it is applied before any frame asked for later."""
        self._check_open()
        self.stats.commands_received += 1
        self.stats.last_command_at = time.time()
        self._outbox.put((_COMMAND, command))
        self.changed.set()

    def mark_dirty(self) -> None:
        # The child marks its scene on every command; a wake-up is all the
        # push stream needs from here.
        self.changed.set()

    async def render_frame(self) -> bytes:
        return await self._request("frame")

    async def render_frame_if_changed(self, since: int) -> tuple[bytes | None, int]:
        return await self._request("frame_if_changed", since)

    async def render_rgb_frame(self):
        rgba, render_ms, idr = await self._request("rgb")
        self._idr = self._idr or idr
        return rgba, render_ms

    def consume_idr_flag(self) -> bool:
        """The child's key-frame flag as of the last RGB frame (the WebRTC
        track asks right after receiving it)."""
        flag, self._idr = self._idr, False
        return flag

    # ── Plumbing ─────────────────────────────────────────────────────────

    def _check_open(self) -> None:
        if self._refusing or self._conn is None:
            raise RuntimeError(f"Session {self.session_id} is closed.")

    async def _request(self, kind: str, *args: Any) -> Any:
        """Send a request that is answered and wait for its answer."""
        self._check_open()
        future = self._loop.create_future()
        with self._order_lock:
            self._pending.append(future)
            self._outbox.put((kind, *args))
        return await future

    def _write_messages(self) -> None:
        """Writer thread: send queued messages in order until released."""
        while True:
            message = self._outbox.get()
            if message is None:
                return
            try:
                self._conn.send(message)
            except (OSError, ValueError):
                return   # the child is gone; the reader reports it

    def _read_replies(self) -> None:
        """Reader thread: hand each answer to the request waiting for it."""
        while True:
            try:
                reply = self._conn.recv()
            except (EOFError, OSError):
                try:
                    self._loop.call_soon_threadsafe(self._child_gone)
                except RuntimeError:
                    pass   # the loop is closed: the backend is shutting down
                return
            try:
                self._loop.call_soon_threadsafe(self._deliver, reply)
            except RuntimeError:
                return

    def _deliver(self, reply: tuple) -> None:
        status, value, stats, size = reply
        for key, val in stats.items():
            if key not in ("commands_received", "last_command_at", "created_at"):
                setattr(self.stats, key, val)
        self.view.width, self.view.height = size
        if not self._pending:
            return
        future = self._pending.popleft()
        if future.done():
            return
        if status == "ok":
            future.set_result(value)
        else:
            future.set_exception(RuntimeError(value))

    def _child_gone(self) -> None:
        """The child exited or crashed: fail what is waiting, refuse the rest,
        and release its resources."""
        if not self._refusing:
            logger.error("[render-process %s] render process ended unexpectedly",
                         self.session_id[:8])
        self._refusing = True
        while self._pending:
            future = self._pending.popleft()
            if not future.done():
                code = self._process.exitcode if self._process is not None else None
                future.set_exception(RuntimeError(
                    f"Render process for session {self.session_id} ended (exit code {code})."))
        if not self._released:
            self._loop.run_in_executor(None, self._release)


# ── The child ────────────────────────────────────────────────────────────────


def _load(factory: str):
    module, _, name = factory.partition(":")
    mod = __import__(module, fromlist=[name])
    return getattr(mod, name)


def _serve(conn, session_id: str, dataset_path: str, factory: str) -> None:
    """Child process: run the session, answering requests in the order received."""
    # A spawned child starts with logging unconfigured: without this the
    # session's messages (render timings, failures) would be lost.
    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s %(name)s %(levelname)s %(message)s")
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    session = _load(factory)(session_id, dataset_path)

    def reply(status: str, value: Any) -> None:
        view = getattr(session, "view", None)
        size = (getattr(view, "width", 0), getattr(view, "height", 0))
        conn.send((status, value, asdict(session.stats), size))

    while True:
        try:
            message = conn.recv()
        except (EOFError, OSError):
            break   # the backend went away
        kind, args = message[0], message[1:]
        try:
            if kind == _COMMAND:
                loop.run_until_complete(session.apply_command(args[0]))
                continue
            if kind == "init":
                loop.run_until_complete(session.initialize())
                reply("ok", None)
            elif kind == "frame":
                reply("ok", loop.run_until_complete(session.render_frame()))
            elif kind == "frame_if_changed":
                reply("ok", loop.run_until_complete(session.render_frame_if_changed(args[0])))
            elif kind == "rgb":
                rgba, render_ms = loop.run_until_complete(session.render_rgb_frame())
                reply("ok", (rgba, render_ms, session.consume_idr_flag()))
            elif kind == "close":
                loop.run_until_complete(session.close())
                reply("ok", None)
                break
            else:
                reply("error", f"unknown request {kind!r}")
        except Exception as exc:   # noqa: BLE001 - reported to the waiting request
            logger.exception("[render-process %s] %s failed", session_id[:8], kind)
            if kind != _COMMAND:
                reply("error", f"{type(exc).__name__}: {exc}")
    loop.close()
