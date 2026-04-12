"""
test_memmap_cleanup.py – unit tests for np.memmap lifecycle management.

Covers:
  1. _safe_close_memmap() — basic close, idempotence, non-memmap no-op
  2. Session.close() — releases VBT memmap, skips non-VBT datasets
  3. SessionRegistry eviction — close() is called before session is popped
  4. Stale session with no VBT datasets — close() is a safe no-op
  5. Multiple VBT datasets in one session — all memmaps closed

Run with:
    cd backend
    pytest tests/test_memmap_cleanup.py -v
"""

from __future__ import annotations

import os
import tempfile

import numpy as np
import pytest

from app.sessions import Session, SessionRegistry, _safe_close_memmap


# ── Helpers ───────────────────────────────────────────────────────────────────

def _make_memmap(tmp_path, shape=(10,), dtype="float32") -> np.memmap:
    """Create a temporary writable memmap backed by a real file."""
    p = tmp_path / "test.bin"
    arr = np.memmap(str(p), dtype=dtype, mode="w+", shape=shape)
    arr[:] = np.arange(shape[0], dtype=dtype)
    return arr


def _mmap_is_closed(arr: np.memmap) -> bool:
    """Return True if the underlying mmap file descriptor has been closed."""
    mmap_obj = getattr(arr, "_mmap", None)
    if mmap_obj is None:
        return True  # nothing to check
    return mmap_obj.closed


# ═══════════════════════════════════════════════════════════════════════════════
# 1. _safe_close_memmap()
# ═══════════════════════════════════════════════════════════════════════════════


def test_safe_close_memmap_closes_fd(tmp_path):
    arr = _make_memmap(tmp_path)
    assert not _mmap_is_closed(arr)
    _safe_close_memmap(arr)
    assert _mmap_is_closed(arr)


def test_safe_close_memmap_idempotent(tmp_path):
    """Calling _safe_close_memmap twice must not raise."""
    arr = _make_memmap(tmp_path)
    _safe_close_memmap(arr)
    _safe_close_memmap(arr)  # second call — must be silent no-op


def test_safe_close_memmap_on_plain_ndarray():
    """Plain ndarray has no _mmap attribute — must be a silent no-op."""
    arr = np.zeros(10, dtype=np.float32)
    _safe_close_memmap(arr)  # must not raise


def test_safe_close_memmap_on_none():
    """None input must be a silent no-op."""
    _safe_close_memmap(None)


def test_safe_close_memmap_on_arbitrary_object():
    """Any object without _mmap must not raise."""
    _safe_close_memmap(42)
    _safe_close_memmap("string")
    _safe_close_memmap({"key": "value"})


def test_safe_close_memmap_releases_fd_so_file_can_be_deleted(tmp_path):
    """After explicit close, the backing file can be removed on all OSes."""
    p = tmp_path / "deleteme.bin"
    arr = np.memmap(str(p), dtype="float32", mode="w+", shape=(5,))
    _safe_close_memmap(arr)
    # On Windows this would fail if the handle were still open;
    # on macOS/Linux it is a useful regression guard.
    p.unlink()
    assert not p.exists()


# ═══════════════════════════════════════════════════════════════════════════════
# 2. Session.close()
# ═══════════════════════════════════════════════════════════════════════════════


def test_session_close_releases_vbt_memmap(tmp_path):
    session = Session("test-session")
    arr = _make_memmap(tmp_path)
    session.datasets["ds1"] = {"kind": "vbt", "memmap": arr}

    session.close()

    assert _mmap_is_closed(arr)


def test_session_close_skips_fits_datasets(tmp_path):
    """FITS datasets have no memmap entry — close() must not crash."""
    session = Session("test-session")
    session.datasets["ds1"] = {"kind": "fits_cube", "path": "/fake/path.fits"}

    session.close()  # must not raise


def test_session_close_skips_catalogue_datasets():
    """Catalogue datasets have no memmap — close() must not crash."""
    import pandas as pd

    session = Session("test-session")
    session.datasets["ds1"] = {
        "kind": "catalogue",
        "dataframe": pd.DataFrame({"ra": [1.0], "dec": [2.0]}),
    }
    session.close()  # must not raise


def test_session_close_skips_hips_datasets():
    session = Session("test-session")
    session.datasets["ds1"] = {"kind": "hips", "base_url": "http://example.com"}
    session.close()  # must not raise


def test_session_close_multiple_vbt_datasets(tmp_path):
    session = Session("test-session")
    arr1 = np.memmap(str(tmp_path / "a.bin"), dtype="float32", mode="w+", shape=(10,))
    arr2 = np.memmap(str(tmp_path / "b.bin"), dtype="float32", mode="w+", shape=(20,))

    session.datasets["ds1"] = {"kind": "vbt", "memmap": arr1}
    session.datasets["ds2"] = {"kind": "vbt", "memmap": arr2}

    session.close()

    assert _mmap_is_closed(arr1)
    assert _mmap_is_closed(arr2)


def test_session_close_mixed_dataset_kinds(tmp_path):
    """Session with both VBT and non-VBT datasets: only the VBT one is closed."""
    session = Session("test-session")
    arr = np.memmap(str(tmp_path / "data.bin"), dtype="float32", mode="w+", shape=(5,))
    session.datasets["vbt1"] = {"kind": "vbt", "memmap": arr}
    session.datasets["fits1"] = {"kind": "fits_cube", "path": "/fake.fits"}

    session.close()

    assert _mmap_is_closed(arr)  # VBT memmap was closed
    # fits1 entry is untouched — no assertion needed, just no crash


def test_session_close_idempotent(tmp_path):
    """Calling Session.close() twice must not raise."""
    session = Session("test-session")
    arr = np.memmap(str(tmp_path / "data.bin"), dtype="float32", mode="w+", shape=(5,))
    session.datasets["ds1"] = {"kind": "vbt", "memmap": arr}

    session.close()
    session.close()  # second call — must be silent


def test_session_close_empty_session():
    """Session with no datasets: close() must be a silent no-op."""
    session = Session("test-session")
    session.close()  # must not raise


def test_session_close_vbt_entry_missing_memmap_key():
    """VBT entry without 'memmap' key must not crash."""
    session = Session("test-session")
    session.datasets["ds1"] = {"kind": "vbt"}  # memmap key absent
    session.close()  # must not raise


# ═══════════════════════════════════════════════════════════════════════════════
# 3. SessionRegistry eviction — close() called before pop
# ═══════════════════════════════════════════════════════════════════════════════


def test_registry_eviction_closes_memmap(tmp_path):
    """Session.close() is called by the eviction path and releases memmaps.

    Rather than relying on the daemon thread (which races with the test),
    we exercise the eviction logic directly: build a standalone session with a
    VBT memmap, register it in a fresh registry, then manually reproduce what
    _evict_stale does — pop from _sessions, call close().
    """
    registry = SessionRegistry(ttl_seconds=3600)  # long TTL so daemon never fires

    session = registry.create()
    arr = np.memmap(str(tmp_path / "data.bin"), dtype="float32", mode="w+", shape=(5,))
    session.datasets["ds1"] = {"kind": "vbt", "memmap": arr}

    # Simulate what _evict_stale does for one session.
    with registry._lock:
        evicted = registry._sessions.pop(session.session_id, None)
    if evicted is not None:
        evicted.close()

    assert _mmap_is_closed(arr), "memmap must be closed after eviction"
    assert session.session_id not in registry._sessions


def test_registry_eviction_no_crash_without_vbt(tmp_path):
    """Eviction of a session with only FITS datasets must not raise."""
    registry = SessionRegistry(ttl_seconds=3600)
    session = registry.create()
    session.datasets["ds1"] = {"kind": "fits_cube", "path": "/fake.fits"}

    with registry._lock:
        evicted = registry._sessions.pop(session.session_id, None)
    if evicted is not None:
        evicted.close()  # must not raise


# ═══════════════════════════════════════════════════════════════════════════════
# 4. File descriptor not leaked after close
# ═══════════════════════════════════════════════════════════════════════════════


def test_fd_count_does_not_grow_after_close(tmp_path):
    """Open and close N memmaps; OS FD count must not accumulate."""
    import resource

    def _open_fds() -> int:
        soft, _ = resource.getrlimit(resource.RLIMIT_NOFILE)
        # Count actual open fds via /proc (Linux) or fall back to a best-effort check.
        try:
            return len(os.listdir("/proc/self/fd"))
        except FileNotFoundError:
            # macOS: use lsof approximation
            import subprocess
            out = subprocess.check_output(["lsof", "-p", str(os.getpid()), "-Fn"], text=True)
            return out.count("\nn")

    before = _open_fds()
    for i in range(20):
        p = tmp_path / f"fd_{i}.bin"
        arr = np.memmap(str(p), dtype="float32", mode="w+", shape=(100,))
        _safe_close_memmap(arr)
    after = _open_fds()

    # Allow a small margin for test runner internals, but not 20 leaked fds.
    assert after - before < 5, f"FD leak detected: before={before} after={after}"
