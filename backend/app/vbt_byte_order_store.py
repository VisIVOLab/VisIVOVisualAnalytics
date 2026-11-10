"""Where a VBT's chosen byte order is kept (issue #165): a small JSON file
that every process of the backend reads.

The backend reads a VBT in many processes, not one: render sessions
(``render_session/_process.py``, spawned), the worker pool of the cube tools
(``dependencies._POOL``, spawned, and Dask workers), the cube reader's pool,
pyramid builds and their workers. A choice kept in the memory of the process
that answered ``/v1/*/open`` is invisible to all of them. So the choice is a
file, read by ``volume_source.parse_vbt_header`` wherever it runs.

The store decides how a file is read when it is OPENED. An open dataset
carries the order it was opened in (``volume_source.with_order``: its path
ends in ``::vbt-order=…``), so a later change never reaches a running task, a
cached product, or a dataset another backend sharing the store holds open.

The byte order is a property of the header and the data together, so the
choice is per backend installation (and per store: backends sharing a
workspace share it), not per session. It is keyed by the real path of the
``.head`` the header parser reads (the one beside the path given, a symlink
followed), and valid while that ``.head`` and its data are the ones it was
made for: size, modification time and inode. Correcting the ``.head`` or
rewriting the data retires it; a rewrite that keeps size, mtime and inode
(``touch -r`` after an in-place edit) does not, and the file must then be
opened again with the order wanted. An entry whose files cannot be stat'ed
for the moment (an offline mount) is kept.

The store is ``$VISIVO_VBT_BYTE_ORDERS`` when set, else
``.vbt-byte-orders.json`` in the workspace (``$VISIVO_EXPORTS_DIR``, as
``dependencies`` reads it): from the environment alone, so a child process
finds the same file as its parent without being told. It is written whole to
a temporary file (fsync'ed) and renamed over the old one, under an exclusive
lock on a sidecar ``.lock`` file: ``fcntl.flock``, ``msvcrt.locking`` on
Windows, and, where the file system refuses both (Lustre without flock, some
SMB/NFS mounts), a lock directory with a stale timeout; failing all of them
the write goes ahead unlocked, and says so in the log. A reader re-reads the
store only when its stat changes.
"""
from __future__ import annotations

import contextlib
import json
import logging
import os
import tempfile
import threading
import time
from pathlib import Path

logger = logging.getLogger("visivo.backend")

BYTE_ORDERS = ("little", "big")
_LOCK = threading.RLock()   # re-entered: set_override resets the cache under it
_cache: tuple[tuple, dict] | None = None   # (stat of the store, its entries)
#: A lock directory older than this is an abandoned one (its writer died).
_STALE_LOCK_S = 30.0
#: How long a writer waits for the lock directory before going ahead unlocked.
_LOCK_WAIT_S = 10.0


class ByteOrderNotStored(ValueError):
    """An order was asked for a file the store cannot describe."""


def store_path() -> Path:
    explicit = os.environ.get("VISIVO_VBT_BYTE_ORDERS")
    if explicit:
        return Path(explicit)
    # As dependencies._EXPORTS_DIR reads it (an empty value is the cwd there).
    root = os.environ.get("VISIVO_EXPORTS_DIR", str(Path.home() / ".visivo" / "exports"))
    return Path(root) / ".vbt-byte-orders.json"


def head_of(path: str | os.PathLike) -> str:
    """The ``.head`` the header parser reads for ``path`` (data, ``.head``,
    field reference or a path with its order): beside the path as given."""
    text = str(path)
    for sep in ("::vbt-order=", "::vbt-field="):
        base, found, tail = text.rpartition(sep)
        if found and (tail in BYTE_ORDERS or tail.isdigit()):
            text = base
    return text if text.endswith(".head") else text + ".head"


def data_key(path: str | os.PathLike) -> str:
    """A VBT's key: the real path of its ``.head`` (:func:`head_of`)."""
    return os.path.realpath(head_of(path))


def _identity(path: str | os.PathLike) -> dict | None:
    """What a choice is valid for: the ``.head`` and its data, as they are
    (symlinks followed); None when either cannot be stat'ed."""
    head = head_of(path)
    try:
        h = os.stat(head)
        d = os.stat(head[: -len(".head")])
    except OSError:
        return None
    return {"data": [d.st_size, d.st_mtime_ns, d.st_ino], "head": [h.st_size, h.st_mtime_ns]}


def _load() -> dict:
    """The store's entries, re-read only when the file has changed."""
    global _cache
    path = store_path()
    try:
        st = os.stat(path)
    except OSError:
        return {}
    key = (str(path), st.st_size, st.st_mtime_ns, st.st_ino)
    with _LOCK:
        if _cache is not None and _cache[0] == key:
            return _cache[1]
    try:
        entries = json.loads(path.read_text("utf-8"))
        if not isinstance(entries, dict):
            raise ValueError("not an object")
    except (OSError, ValueError) as exc:
        logger.warning("[vbt-byte-order] ignoring unreadable store %s: %s", path, exc)
        entries = {}
    with _LOCK:
        _cache = (key, entries)
    return entries


def override(path: str | os.PathLike) -> str:
    """The byte order the VBT ``path`` is read in against its header, or ""
    (none chosen, or chosen for a version of the file that is gone)."""
    entries = _load()
    if not entries:           # the usual case: no stat, no realpath
        return ""
    entry = entries.get(data_key(path))
    if not isinstance(entry, dict) or entry.get("order") not in BYTE_ORDERS:
        return ""
    if entry.get("identity") != _identity(path):
        return ""
    return str(entry["order"])


def _lock_directory(lock: Path) -> bool:
    """Take the lock directory ``lock``; False when it cannot be had."""
    deadline = time.monotonic() + _LOCK_WAIT_S
    while True:
        try:
            os.mkdir(lock)
            return True
        except FileExistsError:
            with contextlib.suppress(OSError):
                if time.time() - os.stat(lock).st_mtime > _STALE_LOCK_S:
                    os.rmdir(lock)
                    continue
        except OSError:
            return False
        if time.monotonic() > deadline:
            return False
        time.sleep(0.05)


@contextlib.contextmanager
def _exclusive(store: Path):
    """An exclusive lock across processes, and across threads."""
    store.parent.mkdir(parents=True, exist_ok=True)
    with _LOCK, open(store.with_name(store.name + ".lock"), "a+") as fh:
        unlock = None
        try:
            import fcntl  # noqa: PLC0415
            fcntl.flock(fh.fileno(), fcntl.LOCK_EX)
            unlock = lambda: fcntl.flock(fh.fileno(), fcntl.LOCK_UN)  # noqa: E731
        except ImportError:
            try:
                import msvcrt  # noqa: PLC0415
            except ImportError:
                msvcrt = None
            if msvcrt is not None:
                fh.seek(0)
                deadline = time.monotonic() + _LOCK_WAIT_S
                while True:
                    try:
                        msvcrt.locking(fh.fileno(), msvcrt.LK_LOCK, 1)
                        break
                    except OSError:
                        if time.monotonic() > deadline:
                            raise
                unlock = lambda: (fh.seek(0), msvcrt.locking(fh.fileno(), msvcrt.LK_UNLCK, 1))  # noqa: E731
        except OSError as exc:
            # flock refused by the file system (Lustre without flock: ENOSYS;
            # some SMB/NFS mounts: ENOTSUP): a lock directory instead.
            lock_dir = store.with_name(store.name + ".lockdir")
            if _lock_directory(lock_dir):
                unlock = lambda: os.rmdir(lock_dir)  # noqa: E731
            else:
                logger.warning("[vbt-byte-order] %s: no lock (%s); writing unlocked", store, exc)
        try:
            yield
        finally:
            if unlock is not None:
                with contextlib.suppress(OSError):
                    unlock()


def _replace(tmp: str, store: Path) -> None:
    """``os.replace``, retried while Windows reports the store in use."""
    for attempt in range(40):
        try:
            os.replace(tmp, store)
            return
        except PermissionError:
            if os.name != "nt" or attempt == 39:
                raise
            time.sleep(0.05)


def _write(store: Path, entries: dict) -> None:
    fd, tmp = tempfile.mkstemp(prefix=store.name + ".", suffix=".tmp", dir=store.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            json.dump(entries, fh, indent=1, sort_keys=True)
            fh.flush()
            os.fsync(fh.fileno())
        _replace(tmp, store)
    except BaseException:
        with contextlib.suppress(OSError):
            os.unlink(tmp)
        raise
    if hasattr(os, "O_DIRECTORY"):   # the rename itself, where a directory can be synced
        with contextlib.suppress(OSError):
            dfd = os.open(store.parent, os.O_DIRECTORY)
            try:
                os.fsync(dfd)
            finally:
                os.close(dfd)


def set_override(path: str | os.PathLike, order: str | None, declared: str,
                 by: str = "") -> tuple[str, str]:
    """Read the VBT ``path`` in byte order ``order`` from now on (``order``
    equal to ``declared``, or None, drops the choice). ``by`` says who asked,
    for the log. Returns the order read in before and after; raises
    :class:`ByteOrderNotStored` when an order was asked for a file whose
    ``.head`` or data cannot be stat'ed."""
    global _cache
    if order is not None and order not in BYTE_ORDERS:
        raise ValueError(f"byte order must be 'little' or 'big', not {order!r}")
    key = data_key(path)
    identity = _identity(path)
    wanted = order is not None and order != declared
    if wanted and identity is None:
        raise ByteOrderNotStored(
            f"The byte order cannot be kept for {head_of(path)}: the header or its data "
            "cannot be read.")
    store = store_path()
    with _exclusive(store):
        try:
            entries = json.loads(store.read_text("utf-8"))
            if not isinstance(entries, dict):
                entries = {}
        except (OSError, ValueError):
            entries = {}
        old = entries.get(key)
        before = (old["order"] if isinstance(old, dict) and old.get("order") in BYTE_ORDERS
                  and old.get("identity") == identity else declared)

        def current(k: str, v) -> bool:
            # Retired only on a positive mismatch: an offline mount keeps its entry.
            if not isinstance(v, dict):
                return False
            now = _identity(k)
            return now is None or v.get("identity") == now

        entries = {k: v for k, v in entries.items() if k != key and current(k, v)}
        after = declared
        if wanted:
            entries[key] = {"order": order, "declared": declared, "identity": identity,
                            "set_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "set_by": by}
            after = order
        _write(store, entries)
        with _LOCK:
            _cache = None
    if before != after:
        logger.info("[vbt-byte-order] %s: read as %s-endian (header says %s), was %s; by %s",
                    key, after, declared, before, by or "unknown")
    return before, after


def clear() -> None:
    """Forget every choice (tests)."""
    global _cache
    with contextlib.suppress(OSError):
        os.unlink(store_path())
    with _LOCK:
        _cache = None
