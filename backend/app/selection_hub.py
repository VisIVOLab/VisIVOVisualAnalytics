"""selection_hub.py: the selection shared by the clients of one session (issue #188).

The VR headset's page and the desktop's cube viewer work in the same backend
session (the desktop's pairing link hands its session to the headset, #172).
A selection made on one side (a Kinematic Lasso, a box region) is published
here and the other side, waiting on a long poll, shows it within a request's
round trip.

What is kept, per session and per cube (the dataset's file: the headset may
have opened the desktop's cube again under another dataset id), is the LATEST
selection only: a new one replaces it, whichever side made it. A lasso's mask
is stored bit-packed over its bounding box (one bit per voxel: a 16 Mi voxel
mask is 2 MiB), a box as its corners. With the mask: the statistics measured
on the full cube when it was published (so both sides show the same numbers),
which side and which client made it, and the creator's record of how it was
made (the grow request, under the API's names), so the side that receives it
can attach that record instead of recording a step of its own.

Bounds: masks of at most ``MAX_SHARE_VOXELS``; at most ``MAX_ENTRIES_PER_SESSION``
cubes per session (the oldest is dropped); ``MAX_TOTAL_BYTES`` of masks over
all sessions (the oldest selections go first); at most ``MAX_WAITERS_PER_SESSION``
long polls waiting at once; at most ``MAX_MASK_READS`` masks unpacked at once,
off the event loop, the latest one kept unpacked. A selection lives as long as
its session and the cube stays open in it: the hubs are held by a weak
reference to their session, so a session evicted after its idle time
(VISIVO_SESSION_TTL) takes its selections along, and a selection of a cube no
longer open in the session is dropped.

Order: a selection's ``seq`` is taken when its publish ARRIVES, before the
backend measures it, and a cube never takes a selection below the highest seq
it has taken (a high-water mark per cube that a clear or a prune leaves in
place), so the latest publish wins even when an earlier one takes longer to
measure, and an earlier one cannot land after a clear.

The one mask kept unpacked (at most 64 MiB of float32) is not counted in
``MAX_TOTAL_BYTES``; it is dropped as soon as its selection leaves its hub
(replaced, cleared, pruned, evicted by a cap, or with its session).
"""
from __future__ import annotations

import asyncio
import itertools
import uuid
import time
import weakref
from dataclasses import dataclass, field
from typing import Any

import numpy as np

#: The largest selection shared (voxels of its bounding box): 16 Mi, a 64 MiB
#: float32 body on the wire, 2 MiB stored. Larger is a cube, not a source.
MAX_SHARE_VOXELS = 16 * 1024 * 1024
MAX_ENTRIES_PER_SESSION = 4
MAX_TOTAL_BYTES = 64 * 1024 * 1024
MAX_WAITERS_PER_SESSION = 8
#: The longest a long poll waits (seconds).
MAX_WAIT_SECONDS = 25.0
#: Masks unpacked to float32 at once (each up to 64 MiB).
MAX_MASK_READS = 2
#: A client's meta record (JSON text), at most.
MAX_META_CHARS = 6000


_seq = itertools.count(1)   # global, so a seq never repeats across hubs
_last_seq = 0               # the last seq issued (next_seq)
#: This backend process: seqs restart at 1 when it restarts, so a client that
#: sees another instance resets its cursor and floor (they belong to the old one).
INSTANCE = uuid.uuid4().hex[:16]


@dataclass
class SharedSelection:
    seq: int
    kind: str                      # "lasso" | "box"
    source: str                    # "headset" | "desktop"
    client: str                    # the publishing client's own id (it ignores its echo)
    dataset_key: str               # the cube's file
    dataset_id: str                # the publisher's dataset id
    origin: tuple[int, int, int]   # x, y, z (cube voxels)
    dims: tuple[int, int, int]     # width, height, depth
    full_shape: tuple[int, int, int]
    count: int
    packed: bytes | None           # bit-packed mask (x fastest), None for a box
    stats: dict[str, Any]
    meta: dict[str, Any]
    created: float = field(default_factory=time.monotonic)

    @property
    def nbytes(self) -> int:
        return len(self.packed) if self.packed else 0

    def info(self) -> dict[str, Any]:
        """The JSON a subscriber reads (the mask comes from its own route)."""
        return {
            "instance": INSTANCE,
            "seq": self.seq, "kind": self.kind, "source": self.source, "client": self.client,
            "dataset_id": self.dataset_id,
            "origin": list(self.origin), "dims": list(self.dims),
            "full_shape": list(self.full_shape), "count": self.count,
            "age_s": round(time.monotonic() - self.created, 3),
            "stats": self.stats, "meta": self.meta,
        }

    def mask_float32(self) -> bytes:
        """The mask as the select route answers it: little-endian float32 0/1
        over the box, x fastest (a box: all ones). CPU-bound: the route runs it
        off the event loop (``mask_bytes``)."""
        n = self.dims[0] * self.dims[1] * self.dims[2]
        if self.packed is None:
            return np.ones(n, dtype="<f4").tobytes()
        bits = np.unpackbits(np.frombuffer(self.packed, dtype=np.uint8), count=n)
        return bits.astype("<f4").tobytes()


def pack_mask(mask_bytes: bytes, n: int) -> tuple[bytes, int]:
    """A float32 mask to bits, a voxel selected when its value is > 0.5: the
    rule of mask_stats_bin and mask_apply_bin (and of the clients), so the
    count, the statistics and the mask handed on describe the same voxels
    (NaN is not selected). Returns (packed, count)."""
    if len(mask_bytes) != n * 4:
        raise ValueError(f"mask body is {len(mask_bytes)} bytes; expected {n * 4}")
    m = np.frombuffer(mask_bytes, dtype="<f4")
    sel = m > 0.5
    return np.packbits(sel).tobytes(), int(np.count_nonzero(sel))


class SelectionHub:
    """The latest selection per cube of ONE session, and its waiters."""

    def __init__(self) -> None:
        self.entries: dict[str, SharedSelection] = {}
        # The highest seq each cube has taken: kept through clear and prune.
        self.high_water: dict[str, int] = {}
        # The mark the last clear of each cube set (every seq issued by then).
        self.cleared_at: dict[str, int] = {}
        self.waiters = 0
        self._changed = asyncio.Event()

    def prune(self, open_keys: set[str]) -> None:
        """Drops the selections of cubes no longer open in the session."""
        for key in [k for k in self.entries if k not in open_keys]:
            _forget(self.entries.pop(key))

    def get(self, dataset_key: str) -> SharedSelection | None:
        return self.entries.get(dataset_key)

    def publish(self, sel: SharedSelection) -> str:
        """Keeps ``sel`` ('') unless a later publish was kept ('stale') or the
        cube's selection was cleared after ``sel`` arrived ('cleared')."""
        # At or below the mark: a later publish was kept, or a clear came after
        # this one arrived (a seq is issued once, so equal is never this one's own).
        if sel.seq <= self.high_water.get(sel.dataset_key, 0):
            current = self.entries.get(sel.dataset_key)
            if (current is None or current.seq < sel.seq) and sel.seq <= self.cleared_at.get(sel.dataset_key, 0):
                return "cleared"
            return "stale"
        self.high_water[sel.dataset_key] = sel.seq
        old = self.entries.pop(sel.dataset_key, None)
        if old is not None:
            _forget(old)
        self.entries[sel.dataset_key] = sel          # newest last
        while len(self.entries) > MAX_ENTRIES_PER_SESSION:
            _forget(self.entries.pop(next(iter(self.entries))))
        _enforce_total_bytes()
        self._notify()
        return ""

    def clear(self, dataset_key: str) -> bool:
        # Every publish already under way for this cube (its seq is issued when
        # it arrives) is older than the clear: none may bring a selection back.
        self.high_water[dataset_key] = max(self.high_water.get(dataset_key, 0), last_seq())
        self.cleared_at[dataset_key] = self.high_water[dataset_key]
        old = self.entries.pop(dataset_key, None)
        if old is None:
            return False
        _forget(old)
        self._notify()
        return True

    def _notify(self) -> None:
        old, self._changed = self._changed, asyncio.Event()
        old.set()

    async def wait_newer(self, dataset_key: str, after: int, timeout: float,
                         disconnected=None) -> SharedSelection | None:
        """The selection of the cube once its seq is above ``after``, waiting
        up to ``timeout`` seconds; None on timeout (or when the caller left:
        ``disconnected`` is an async callable checked every second)."""
        deadline = time.monotonic() + max(0.0, min(timeout, MAX_WAIT_SECONDS))
        while True:
            e = self.get(dataset_key)
            if e is not None and e.seq > after:
                return e
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            ev = self._changed
            try:
                await asyncio.wait_for(ev.wait(), min(remaining, 1.0))
            except asyncio.TimeoutError:
                if disconnected is not None and await disconnected():
                    return None


# Session -> hub, held weakly: a session evicted from the registry takes its
# selections with it.
_HUBS: "weakref.WeakKeyDictionary[Any, SelectionHub]" = weakref.WeakKeyDictionary()


def hub_for(session: Any) -> SelectionHub:
    hub = _HUBS.get(session)
    if hub is None:
        hub = SelectionHub()
        _HUBS[session] = hub
    return hub


def total_bytes() -> int:
    return sum(e.nbytes for h in list(_HUBS.values()) for e in h.entries.values())


def _enforce_total_bytes() -> None:
    """Drops the oldest selections, over every session, until the masks fit."""
    while True:
        entries = [(e.created, h, k, e) for h in list(_HUBS.values()) for k, e in h.entries.items()]
        if sum(e.nbytes for *_, e in entries) <= MAX_TOTAL_BYTES or len(entries) <= 1:
            return
        _, h, k, _e = min(entries, key=lambda t: t[0])
        _forget(h.entries.pop(k))


def next_seq() -> int:
    global _last_seq
    _last_seq = next(_seq)
    return _last_seq


def last_seq() -> int:
    """Every seq issued so far is at most this (publishes still in flight included)."""
    return _last_seq


# The mask reads: off the event loop, one unpack per selection however many
# clients ask at once (they share it), at most MAX_MASK_READS selections
# unpacked at once, the latest unpacked one kept.
_mask_reads = asyncio.Semaphore(MAX_MASK_READS)
_mask_cache: dict[str, Any] = {"seq": None, "raw": b"", "ref": None}
_mask_inflight: dict[int, "asyncio.Future[bytes]"] = {}


def _drop_cache(seq: int) -> None:
    if _mask_cache["seq"] == seq:
        _mask_cache.update(seq=None, raw=b"", ref=None)


def _forget(sel: SharedSelection) -> None:
    """A selection left its hub: its unpacked mask goes too."""
    _drop_cache(sel.seq)


async def _unpack(sel: SharedSelection) -> bytes:
    async with _mask_reads:
        raw = await asyncio.to_thread(sel.mask_float32)
    # Kept only while its selection is still in its hub (it may have been
    # replaced while it was unpacked), and dropped with it when its session is
    # evicted (the hub, held weakly, takes its selections along).
    if _HUBS and any(e is sel for h in list(_HUBS.values()) for e in h.entries.values()):
        _mask_cache.update(seq=sel.seq, raw=raw,
                           ref=weakref.ref(sel, lambda _r, k=sel.seq: _drop_cache(k)))
    return raw


async def mask_bytes(sel: SharedSelection) -> bytes:
    if _mask_cache["seq"] == sel.seq:
        return _mask_cache["raw"]
    fut = _mask_inflight.get(sel.seq)
    if fut is None:
        fut = asyncio.ensure_future(_unpack(sel))
        _mask_inflight[sel.seq] = fut
        fut.add_done_callback(lambda _f, k=sel.seq: _mask_inflight.pop(k, None))
    return await asyncio.shield(fut)


async def pack_mask_async(mask_bytes_: bytes, n: int) -> tuple[bytes, int]:
    return await asyncio.to_thread(pack_mask, mask_bytes_, n)
