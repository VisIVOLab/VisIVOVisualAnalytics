"""Whether a VBT's values look like the byte order its ``.head`` declares
(issue #165).

A VBT's header names its byte order, and the readers obey it
(``volume_source.parse_vbt_header``). Some converters write the data in one
order and the header in the other: ChaNGa/Tipsy snapshots are big-endian, and
tables converted from them were found declaring ``little``. Read as declared,
such a table opens with every value absurd (masses of -1e24, positions from
1e-26 to 1e30, softenings of 4.6e-41) and nothing says why.

The check here reads a few thousand values per field, in chunks spread over
the file, and scores the same bytes in both orders. A float read in the wrong
order takes its exponent from a mantissa byte, which is close to random: the
magnitudes scatter over the whole range of the type, with NaNs, infinities and
subnormals among them. Real data, whatever its units, sit within some tens of
decades of their own median. The score of a field is the share of its
non-NaN sampled values that are infinite, subnormal, or more than
:data:`FAR_DECADES` from the field's median magnitude; no absolute magnitude
counts, so cgs masses of 1e33 g and densities of 1e-24 g/cm^3 are as
plausible as Mpc positions.

The verdict compares the two orders with each other (:func:`verdict`): the
declared one is flagged only when it scores clearly worse than the other. A
file whose values look the same either way (constant fields) gets no warning.

Nothing is rewritten: the order a user chooses is kept in a store every
backend process reads (``app/vbt_byte_order_store.py``), and every reader of
that file honours it.
"""
from __future__ import annotations

import logging
import math
import os
import threading
from pathlib import Path

import numpy as np

logger = logging.getLogger("visivo.backend")

#: How far (decades) from a field's median magnitude a value counts as absurd.
#: Byte-swapped floats scatter over ~83 decades (float32, subnormals
#: included) or ~630 (float64); a real field may span thirty (a density from
#: 1e-30 to 1e3), which must stay plausible.
FAR_DECADES = 20.0
#: Chunks read per field and values per chunk: a few thousand values, from
#: places spread over the field (positions are often sorted in space).
CHUNKS_PER_FIELD = 16
VALUES_PER_CHUNK = 128
#: At most this many reads for the whole table: a table of many fields reads
#: fewer chunks of each, never fewer than two.
MAX_READS = 256
#: At most this many fields are sampled (evenly chosen) in a very wide table.
MAX_FIELDS = 64
#: Fewer sampled values than this per field: no verdict (too little to judge).
MIN_VALUES = 64

#: The verdict compares the two orders with each other: the declared one is
#: flagged when its mean score over the informative fields exceeds the other's
#: by at least MARGIN and the other's is at most RATIO of it. A table read in
#: the wrong order scores about 0.45-1.0, one read in the right order 0.0, or
#: a little more for a field spanning very many decades
#: (tests/test_vbt_byte_order.py).
MARGIN = 0.2
RATIO = 0.25
#: "Overwhelming" evidence (the client then pre-selects the suggested order):
#: at least OVERWHELMING_DECLARED against at most OVERWHELMING_OTHER, from at
#: least two informative fields.
OVERWHELMING_DECLARED = 0.4
OVERWHELMING_OTHER = 0.01


def _other(order: str) -> str:
    return "big" if order == "little" else "little"


def _chunk_starts(num_rows: int, chunks: int, length: int) -> list[int]:
    """Where the chunks of one field start (row indices), spread evenly from
    its first row to its last."""
    if num_rows <= length:
        return [0]
    last = num_rows - length
    count = min(chunks, max(1, num_rows // length))
    if count == 1:
        return [0]
    return sorted({round(i * last / (count - 1)) for i in range(count)})


def sample_fields(binary: str | os.PathLike, itemsize: int, field_count: int,
                  num_rows: int) -> list[tuple[int, bytes]]:
    """``(field index, raw bytes)`` for the sampled fields of a field-major
    VBT (each field ``num_rows`` values, one after the other; points and
    volumes alike). Seeks and reads only: a few hundred small reads whatever
    the file's size."""
    fields = list(range(field_count))
    if field_count > MAX_FIELDS:
        fields = sorted({round(i * (field_count - 1) / (MAX_FIELDS - 1))
                         for i in range(MAX_FIELDS)})
    chunks = max(2, min(CHUNKS_PER_FIELD, MAX_READS // max(1, len(fields))))
    length = min(VALUES_PER_CHUNK, num_rows)
    starts = _chunk_starts(num_rows, chunks, length)
    out = []
    with open(binary, "rb") as fh:
        for f in fields:
            parts = []
            for row in starts:
                fh.seek((f * num_rows + row) * itemsize)
                parts.append(fh.read(length * itemsize))
            out.append((f, b"".join(parts)))
    return out


def implausible_share(values: np.ndarray) -> float:
    """Share of the non-NaN ``values`` (a float array of the file's type)
    that no real field holds: infinite, subnormal, or more than
    :data:`FAR_DECADES` from the field's median magnitude. Zeros are
    plausible. NaN is left out: a masked field is legitimately full of it,
    and read the other way those NaNs are subnormals, which count there."""
    x = values.astype(np.float64)
    x = x[~np.isnan(x)]
    if x.size == 0:
        return 0.0
    tiny = np.finfo(values.dtype).tiny
    finite = np.isfinite(x)
    magnitude = np.abs(np.where(finite, x, 0.0))
    subnormal = finite & (magnitude > 0) & (magnitude < tiny)
    normal = finite & (magnitude >= tiny)
    far = 0
    if normal.any():
        logs = np.log10(magnitude[normal])
        far = int(np.count_nonzero(np.abs(logs - np.median(logs)) > FAR_DECADES))
    bad = int(np.count_nonzero(~finite)) + int(np.count_nonzero(subnormal)) + far
    return bad / x.size


def verdict(mean_declared: float, mean_other: float, informative: int) -> str | None:
    """"overwhelming" or "strong" when the declared order is the implausible
    one of the two, None when it is not (or when the sample cannot tell)."""
    if mean_declared - mean_other < MARGIN or mean_other > RATIO * mean_declared:
        return None
    if (mean_declared >= OVERWHELMING_DECLARED and mean_other <= OVERWHELMING_OTHER
            and informative >= 2):
        return "overwhelming"
    return "strong"


def _show(value: float) -> str:
    return f"{value:.4g}" if math.isfinite(value) else str(value)


def check_byte_order(binary: str | os.PathLike, header: dict) -> dict | None:
    """The warning for a VBT whose values look like the other byte order than
    its header declares (``header`` as ``parse_vbt_header`` gives it); None
    when they look like the declared order, or when the sample cannot tell
    (a table of fewer than :data:`MIN_VALUES` rows, a file of the wrong size).

    The warning is ``{declared, suggested, confidence, evidence}``:
    ``confidence`` is ``"overwhelming"`` or ``"strong"``; ``evidence`` holds
    the mean scores of both orders, the number of values sampled, and per
    field (at most twelve) its scores and first value read either way.
    """
    declared = header.get("declared_endian") or header["file_endian"]
    suggested = _other(declared)
    item = "f4" if header["scalar_type"] == "float32" else "f8"
    as_declared = np.dtype(("<" if declared == "little" else ">") + item)
    as_suggested = as_declared.newbyteorder()
    num_rows = int(header["num_rows"])
    if num_rows < MIN_VALUES:
        return None
    expected = int(header["field_count"]) * num_rows * as_declared.itemsize
    try:
        if Path(binary).stat().st_size != expected:
            return None   # the readers refuse it with their own message
        samples = sample_fields(binary, as_declared.itemsize, int(header["field_count"]),
                                num_rows)
    except OSError:
        return None
    names = header["field_names"]
    rows = []
    sampled = 0
    for f, raw in samples:
        a = np.frombuffer(raw, dtype=as_declared)
        b = np.frombuffer(raw, dtype=as_suggested)
        sampled += a.size
        with np.errstate(all="ignore"):
            rows.append({"name": str(names[f]) if f < len(names) else f"field {f}",
                         "implausible_declared": round(implausible_share(a), 4),
                         "implausible_suggested": round(implausible_share(b), 4),
                         "first_declared": _show(float(a[0])),
                         "first_suggested": _show(float(b[0]))})
    # A field that scores nothing either way (constant, all zero) says
    # nothing about the order; it is left out of the means.
    informative = [r for r in rows
                   if max(r["implausible_declared"], r["implausible_suggested"]) > 0]
    if not informative:
        return None
    mean_declared = sum(r["implausible_declared"] for r in informative) / len(informative)
    mean_suggested = sum(r["implausible_suggested"] for r in informative) / len(informative)
    confidence = verdict(mean_declared, mean_suggested, len(informative))
    if confidence is None:
        return None
    return {
        "declared": declared,
        "suggested": suggested,
        "confidence": confidence,
        "evidence": {
            "implausible_declared": round(mean_declared, 4),
            "implausible_suggested": round(mean_suggested, 4),
            "sampled_values": sampled,
            "informative_fields": len(informative),
            "fields": rows[:12],
        },
    }


#: One open at a time sets and reads a file's order, so that two concurrent
#: opens asking for different orders each report the order they read in.
_OPEN_LOCK = threading.Lock()


def open_with_byte_order(path: str | os.PathLike, requested: str | None, by: str = "") -> dict:
    """What an open route does with a VBT's byte order, BEFORE it reads any
    value: apply the order the user asked for (``requested``; None keeps the
    one this backend already reads the file in), then check the values when
    nothing was asked. ``path`` is the data, its ``.head`` or a field
    reference; ``by`` says who asked (the session), for the log.

    The check compares the values with the order the HEADER declares,
    whatever the file is read in: a fresh open of a file read against its
    header is warned again, so that the user can reconsider. When the order
    changes, the datasets already open on that file in another order are
    closed (:func:`forget_datasets_read_otherwise`): one dataset id must
    never serve two sets of values.

    Returns the open answer's fields ``byte_order`` (read in),
    ``byte_order_declared`` and ``byte_order_warning``, and ``changed``
    (the order was changed: the caller closes the render sessions reading it
    the old way, :func:`close_render_sessions_read_otherwise`)."""
    from .volume_source import parse_vbt_header, set_byte_order_override, split_ref  # noqa: PLC0415

    base, _field = split_ref(path)
    text = str(base)
    head = text if text.endswith(".head") else text + ".head"
    binary = head[: -len(".head")]
    with _OPEN_LOCK:
        changed = False
        if requested is not None:
            declared = parse_vbt_header(head)["declared_endian"]
            before, after = set_byte_order_override(binary, requested, declared, by)
            changed = before != after
        header = parse_vbt_header(head)
    if changed:
        forget_datasets_read_otherwise(binary, header["file_endian"])
    warning = check_byte_order(binary, header) if requested is None else None
    return {"byte_order": header["file_endian"],
            "byte_order_declared": header["declared_endian"],
            "byte_order_warning": warning, "changed": changed}


def forget_datasets_read_otherwise(binary: str | os.PathLike, order: str) -> list[str]:
    """Close every dataset, in every session, that reads the VBT ``binary``
    in another byte order than ``order`` (their values are the other order's:
    a catalogue's frame, a memory map). A request on one is then answered as
    for any closed dataset, and the client opens it again. Returns their ids."""
    from .sessions import REGISTRY  # noqa: PLC0415
    from .vbt_byte_order_store import data_key  # noqa: PLC0415

    key = data_key(binary)

    def stale(entry: dict) -> bool:
        if not isinstance(entry, dict) or "byte_order" not in entry:
            return False
        if entry.get("byte_order") == order:
            return False
        names = [entry.get("path"), *(entry.get("paths") or []),
                 entry.get("head_path"), entry.get("bin_path")]
        return any(name and data_key(str(name)) == key for name in names)

    dropped = REGISTRY.drop_datasets(stale)
    # Closing a dataset leaves its render sessions to the caller (async).
    for session_id, dataset_id in dropped:
        logger.info("[vbt-byte-order] closed %s (session %s…): %s is now read %s-endian",
                    dataset_id, session_id[:8], key, order)
    return [dataset_id for _session, dataset_id in dropped]


def sessions_read_otherwise(sessions, binary: str | os.PathLike, order: str) -> list[str]:
    """The ids of the render ``sessions`` (objects with ``session_id`` and
    ``dataset_path``) that draw the VBT ``binary`` in another byte order
    than ``order``."""
    from .volume_source import read_order  # noqa: PLC0415
    from .vbt_byte_order_store import data_key  # noqa: PLC0415

    key = data_key(binary)
    out = []
    for s in sessions:
        path = str(getattr(s, "dataset_path", "") or "")
        if not path or path.startswith("{"):   # a catalogue spec: not a VBT
            continue
        try:
            if data_key(path) == key and read_order(path) != order:
                out.append(s.session_id)
        except (OSError, ValueError):
            continue
    return out


async def close_render_sessions_read_otherwise(binary: str | os.PathLike, order: str) -> list[str]:
    """Close the render sessions drawing the VBT ``binary`` in another byte
    order than ``order`` (their data were loaded in it): the client finds its
    session gone, as after an expiry, and starts a new one, which reads the new
    order. Nothing when no render manager runs."""
    from . import render_manager as rm  # noqa: PLC0415

    manager = rm._MANAGER
    if manager is None:
        return []
    async with manager._lock:
        sessions = list(manager._sessions.values())
    closed = []
    for session_id in sessions_read_otherwise(sessions, binary, order):
        if await manager.close_session(session_id):
            logger.info("[vbt-byte-order] closed render session %s…: %s is now read %s-endian",
                        session_id[:8], binary, order)
            closed.append(session_id)
    return closed
