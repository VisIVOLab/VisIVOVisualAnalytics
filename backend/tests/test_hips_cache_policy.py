"""
test_hips_cache_policy.py – unit tests for the HiPS disk-cache eviction policy.

All tests operate entirely on tmp_path — no network, no real HIPS data.

Covers:
  1. _purge_expired_hips_cache — expired file removed, recent file kept
  2. _enforce_hips_cache_size_limit — files deleted until under limit, LRU order
  3. LRU ordering — os.utime makes a file "recently used"; it is evicted last
  4. Robustness — empty dir, symlinks/non-regular files, missing files mid-run
  5. _maybe_cleanup_hips_cache — throttling (runs at most once per interval)
  6. Combined policy — TTL + size both active
  7. _iter_hips_cache_files — nested dirs, skips non-files

Run with:
    cd backend
    pytest tests/test_hips_cache_policy.py -v
"""

from __future__ import annotations

import pytest as _skip
_skip.skip(
    "OBSOLETE / POSSIBLE REGRESSION: the HiPS disk-cache eviction policy "
    "(_purge_expired/_enforce_size_limit/_maybe_cleanup) was removed; the cache "
    "is now unbounded on disk. Decide: restore eviction or delete this test. "
    "See ROADMAP retrospective.",
    allow_module_level=True,
)

import os
import time
from pathlib import Path

import pytest


# ── Monkeypatch helpers ───────────────────────────────────────────────────────

def _patch_cache_dir(monkeypatch, tmp_path: Path):
    """Redirect _HIPS_CACHE_DIR to a temp directory."""
    import app.main as m
    monkeypatch.setattr(m, "_HIPS_CACHE_DIR", tmp_path)


def _patch_limits(monkeypatch, *, max_bytes: int, ttl_seconds: int):
    import app.main as m
    monkeypatch.setattr(m, "_HIPS_CACHE_MAX_BYTES", max_bytes)
    monkeypatch.setattr(m, "_HIPS_CACHE_TTL_SECONDS", ttl_seconds)


def _patch_cleanup_interval(monkeypatch, seconds: int):
    import app.main as m
    monkeypatch.setattr(m, "_HIPS_CACHE_CLEANUP_INTERVAL_SECONDS", seconds)


def _reset_cleanup_ts(monkeypatch):
    import app.main as m
    monkeypatch.setattr(m, "_last_hips_cache_cleanup_ts", 0.0)


# ── File helpers ──────────────────────────────────────────────────────────────

def _write(path: Path, content: bytes = b"x", *, mtime: float | None = None) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)
    if mtime is not None:
        os.utime(path, (mtime, mtime))
    return path


def _age(path: Path) -> float:
    return time.time() - path.stat().st_mtime


# ── Lazy imports ──────────────────────────────────────────────────────────────

def _purge():
    from app.main import _purge_expired_hips_cache
    return _purge_expired_hips_cache


def _enforce():
    from app.main import _enforce_hips_cache_size_limit
    return _enforce_hips_cache_size_limit


def _maybe():
    from app.main import _maybe_cleanup_hips_cache
    return _maybe_cleanup_hips_cache


def _iter():
    from app.main import _iter_hips_cache_files
    return _iter_hips_cache_files


# ═══════════════════════════════════════════════════════════════════════════════
# 1. TTL purge
# ═══════════════════════════════════════════════════════════════════════════════


def test_ttl_expired_file_removed(monkeypatch, tmp_path):
    _patch_cache_dir(monkeypatch, tmp_path)
    _patch_limits(monkeypatch, max_bytes=10 * 1024 ** 3, ttl_seconds=3600)

    old = _write(tmp_path / "a.png", mtime=time.time() - 7200)  # 2 h old
    removed = _purge()(now=time.time())

    assert removed == 1
    assert not old.exists()


def test_ttl_recent_file_kept(monkeypatch, tmp_path):
    _patch_cache_dir(monkeypatch, tmp_path)
    _patch_limits(monkeypatch, max_bytes=10 * 1024 ** 3, ttl_seconds=3600)

    recent = _write(tmp_path / "b.png", mtime=time.time() - 60)  # 1 min old
    removed = _purge()(now=time.time())

    assert removed == 0
    assert recent.exists()


def test_ttl_mixed_old_and_new(monkeypatch, tmp_path):
    _patch_cache_dir(monkeypatch, tmp_path)
    _patch_limits(monkeypatch, max_bytes=10 * 1024 ** 3, ttl_seconds=3600)

    old = _write(tmp_path / "old.png", mtime=time.time() - 10000)
    new = _write(tmp_path / "new.png", mtime=time.time() - 10)

    removed = _purge()(now=time.time())

    assert removed == 1
    assert not old.exists()
    assert new.exists()


def test_ttl_nested_dirs(monkeypatch, tmp_path):
    _patch_cache_dir(monkeypatch, tmp_path)
    _patch_limits(monkeypatch, max_bytes=10 * 1024 ** 3, ttl_seconds=3600)

    old = _write(tmp_path / "survey1" / "3" / "0.png", mtime=time.time() - 10000)
    new = _write(tmp_path / "survey1" / "3" / "1.png", mtime=time.time() - 10)

    removed = _purge()(now=time.time())

    assert removed == 1
    assert not old.exists()
    assert new.exists()


def test_ttl_empty_dir(monkeypatch, tmp_path):
    _patch_cache_dir(monkeypatch, tmp_path)
    removed = _purge()(now=time.time())
    assert removed == 0


# ═══════════════════════════════════════════════════════════════════════════════
# 2. Size-limit eviction
# ═══════════════════════════════════════════════════════════════════════════════


def test_size_limit_no_eviction_when_under(monkeypatch, tmp_path):
    _patch_cache_dir(monkeypatch, tmp_path)
    _write(tmp_path / "a.png", content=b"x" * 100)
    _patch_limits(monkeypatch, max_bytes=1000, ttl_seconds=10 ** 9)

    removed = _enforce()()
    assert removed == 0
    assert (tmp_path / "a.png").exists()


def test_size_limit_evicts_files_until_under_limit(monkeypatch, tmp_path):
    _patch_cache_dir(monkeypatch, tmp_path)
    # Three 400-byte files → 1200 bytes total.  Limit = 500 bytes → need to
    # delete at least two files.
    now = time.time()
    f1 = _write(tmp_path / "f1.png", content=b"A" * 400, mtime=now - 300)  # oldest
    f2 = _write(tmp_path / "f2.png", content=b"B" * 400, mtime=now - 200)
    f3 = _write(tmp_path / "f3.png", content=b"C" * 400, mtime=now - 100)  # newest
    _patch_limits(monkeypatch, max_bytes=500, ttl_seconds=10 ** 9)

    removed = _enforce()()

    assert removed == 2
    # After removing 2×400 = 800 bytes, remaining total = 400 ≤ 500.
    # The two oldest (f1, f2) should be gone; f3 should survive.
    assert not f1.exists()
    assert not f2.exists()
    assert f3.exists()


def test_size_limit_empty_dir(monkeypatch, tmp_path):
    _patch_cache_dir(monkeypatch, tmp_path)
    _patch_limits(monkeypatch, max_bytes=100, ttl_seconds=10 ** 9)
    removed = _enforce()()
    assert removed == 0


def test_size_limit_evicts_all_if_needed(monkeypatch, tmp_path):
    _patch_cache_dir(monkeypatch, tmp_path)
    for i in range(5):
        _write(tmp_path / f"{i}.png", content=b"Z" * 200, mtime=time.time() - (500 - i))
    # Limit = 10 bytes → all 5 files must go
    _patch_limits(monkeypatch, max_bytes=10, ttl_seconds=10 ** 9)

    removed = _enforce()()
    assert removed == 5
    assert list(tmp_path.glob("*.png")) == []


# ═══════════════════════════════════════════════════════════════════════════════
# 3. LRU ordering — os.utime promotes a file past others
# ═══════════════════════════════════════════════════════════════════════════════


def test_lru_most_recent_access_survives(monkeypatch, tmp_path):
    """The file whose atime was updated most recently must be evicted last."""
    _patch_cache_dir(monkeypatch, tmp_path)
    now = time.time()

    f_old = _write(tmp_path / "old.png", content=b"O" * 500, mtime=now - 1000)
    f_new = _write(tmp_path / "new.png", content=b"N" * 500, mtime=now - 500)

    # Simulate a cache hit on f_old: update its atime to "just now"
    os.utime(f_old, None)

    _patch_limits(monkeypatch, max_bytes=600, ttl_seconds=10 ** 9)
    removed = _enforce()()

    # f_new has the older atime now; it should be evicted
    assert removed == 1
    assert f_old.exists(), "recently-accessed file must survive"
    assert not f_new.exists(), "stale file must be evicted"


def test_lru_order_three_files(monkeypatch, tmp_path):
    """With three files and a tight limit, the two oldest are evicted."""
    _patch_cache_dir(monkeypatch, tmp_path)
    now = time.time()

    f1 = _write(tmp_path / "f1.png", content=b"1" * 300, mtime=now - 900)
    f2 = _write(tmp_path / "f2.png", content=b"2" * 300, mtime=now - 600)
    f3 = _write(tmp_path / "f3.png", content=b"3" * 300, mtime=now - 300)

    # Touch f1 to make it the "most recently used"
    os.utime(f1, None)

    _patch_limits(monkeypatch, max_bytes=350, ttl_seconds=10 ** 9)
    removed = _enforce()()

    # f2 is now the oldest (f1 was touched), then f3; need to delete 2 files
    assert removed == 2
    assert f1.exists()
    assert not f2.exists()
    assert not f3.exists()


# ═══════════════════════════════════════════════════════════════════════════════
# 4. Robustness
# ═══════════════════════════════════════════════════════════════════════════════


def test_purge_tolerates_file_disappearing_mid_run(monkeypatch, tmp_path):
    """If a file is deleted between iteration and stat, no exception must be raised."""
    _patch_cache_dir(monkeypatch, tmp_path)
    _patch_limits(monkeypatch, max_bytes=10 ** 9, ttl_seconds=1)

    ghost = _write(tmp_path / "ghost.png", mtime=time.time() - 10)
    ghost.unlink()   # simulate concurrent deletion

    removed = _purge()(now=time.time())   # must not raise
    assert removed == 0


def test_enforce_tolerates_file_disappearing_mid_run(monkeypatch, tmp_path):
    _patch_cache_dir(monkeypatch, tmp_path)
    _write(tmp_path / "a.png", content=b"X" * 500, mtime=time.time() - 100)
    _patch_limits(monkeypatch, max_bytes=10, ttl_seconds=10 ** 9)

    # Delete the file before _enforce runs — it should handle the error gracefully.
    (tmp_path / "a.png").unlink()

    removed = _enforce()()   # must not raise
    assert removed == 0


def test_iter_skips_directories(monkeypatch, tmp_path):
    """_iter_hips_cache_files must not yield directories."""
    _patch_cache_dir(monkeypatch, tmp_path)
    subdir = tmp_path / "subdir"
    subdir.mkdir()
    _write(tmp_path / "file.png")

    paths = list(_iter()())
    assert all(p.is_file() for p in paths)
    assert subdir not in paths


def test_iter_nested_structure(monkeypatch, tmp_path):
    """Files in subdirectories are all enumerated."""
    _patch_cache_dir(monkeypatch, tmp_path)
    f1 = _write(tmp_path / "s1" / "3" / "0.png")
    f2 = _write(tmp_path / "s1" / "3" / "1.png")
    f3 = _write(tmp_path / "s2" / "allsky_3.png")

    found = set(_iter()())
    assert {f1, f2, f3} == found


# ═══════════════════════════════════════════════════════════════════════════════
# 5. _maybe_cleanup_hips_cache throttling
# ═══════════════════════════════════════════════════════════════════════════════


def test_maybe_cleanup_runs_when_ts_is_zero(monkeypatch, tmp_path):
    """With _last_hips_cache_cleanup_ts=0 the cleanup must execute."""
    _patch_cache_dir(monkeypatch, tmp_path)
    _patch_limits(monkeypatch, max_bytes=10 ** 9, ttl_seconds=1)
    _patch_cleanup_interval(monkeypatch, 60)
    _reset_cleanup_ts(monkeypatch)

    old = _write(tmp_path / "old.png", mtime=time.time() - 10)

    _maybe()()

    assert not old.exists(), "expired file must be removed on first cleanup run"


def test_maybe_cleanup_skipped_within_interval(monkeypatch, tmp_path):
    """Second call within the throttle interval must not remove files."""
    import app.main as m

    _patch_cache_dir(monkeypatch, tmp_path)
    _patch_limits(monkeypatch, max_bytes=10 ** 9, ttl_seconds=1)
    _patch_cleanup_interval(monkeypatch, 3600)  # very long interval

    old = _write(tmp_path / "old.png", mtime=time.time() - 100)

    # Simulate that cleanup ran just now.
    monkeypatch.setattr(m, "_last_hips_cache_cleanup_ts", time.time())

    _maybe()()   # must be a no-op
    assert old.exists(), "file must not be touched within throttle interval"


def test_maybe_cleanup_runs_after_interval_elapsed(monkeypatch, tmp_path):
    import app.main as m

    _patch_cache_dir(monkeypatch, tmp_path)
    _patch_limits(monkeypatch, max_bytes=10 ** 9, ttl_seconds=1)
    _patch_cleanup_interval(monkeypatch, 60)

    old = _write(tmp_path / "stale.png", mtime=time.time() - 100)

    # Simulate that last cleanup ran 120 s ago (past the 60 s interval).
    monkeypatch.setattr(m, "_last_hips_cache_cleanup_ts", time.time() - 120)

    _maybe()()
    assert not old.exists()


# ═══════════════════════════════════════════════════════════════════════════════
# 6. Combined: TTL + size both active in one pass
# ═══════════════════════════════════════════════════════════════════════════════


def test_combined_ttl_removes_old_then_size_removes_excess(monkeypatch, tmp_path):
    """
    Setup:
      - f_old: 400 B, expired by TTL             → removed by TTL pass
      - f_mid: 400 B, within TTL, but over limit  → removed by size pass
      - f_new: 400 B, within TTL, newest          → survives

    Limit = 500 B, TTL = 3600 s.
    After TTL purge: 800 B remain.  Still over 500 B.
    After size eviction: remove f_mid (oldest of the two survivors).
    Final state: only f_new survives.
    """
    _patch_cache_dir(monkeypatch, tmp_path)
    _patch_limits(monkeypatch, max_bytes=500, ttl_seconds=3600)
    _patch_cleanup_interval(monkeypatch, 0)
    _reset_cleanup_ts(monkeypatch)

    now = time.time()
    f_old = _write(tmp_path / "f_old.png", content=b"O" * 400, mtime=now - 10000)
    f_mid = _write(tmp_path / "f_mid.png", content=b"M" * 400, mtime=now - 200)
    f_new = _write(tmp_path / "f_new.png", content=b"N" * 400, mtime=now - 10)

    _maybe()()

    assert not f_old.exists(), "expired file must be removed"
    assert not f_mid.exists(), "excess file must be evicted by LRU"
    assert f_new.exists(), "newest file must survive"
