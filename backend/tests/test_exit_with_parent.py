"""Children of the backend end when it does, even when it is killed (issue #176,
app/exit_with_parent.py)."""
from __future__ import annotations

import json
import os
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

import pytest

from app import exit_with_parent
from app.parent_watch import process_alive

_BACKEND = Path(__file__).resolve().parents[1]


# ── The watcher ──────────────────────────────────────────────────────────────


@pytest.mark.skipif(sys.platform == "win32",
                    reason="a pipe descriptor is no waitable handle on Windows; see the process test")
def test_the_parent_pipe_closing_is_the_parent_gone():
    r, w = os.pipe()
    try:
        assert not exit_with_parent.parent_gone(r, os.getppid(), 0.05)
        os.close(w)
        w = None
        started = time.monotonic()
        assert exit_with_parent.parent_gone(r, os.getppid(), 5.0)
        assert time.monotonic() - started < 1.0       # at once, not at the timeout
    finally:
        os.close(r)
        if w is not None:
            os.close(w)


@pytest.mark.skipif(sys.platform == "win32", reason="getppid is not checked on Windows")
def test_a_new_parent_pid_is_the_parent_gone():
    # Re-parented to init or to a subreaper: compared with the original PID,
    # not with 1.
    assert exit_with_parent.parent_gone(None, os.getppid() + 1, 0.01)
    assert not exit_with_parent.parent_gone(None, os.getppid(), 0.01)


def test_with_nothing_to_watch_it_only_waits():
    started = time.monotonic()
    assert not exit_with_parent.parent_gone(None, None, 0.05)
    assert time.monotonic() - started >= 0.04


def test_a_process_sentinel_ending_is_the_parent_gone():
    # The form the sentinel takes on every platform, Windows included (a
    # process handle there, a pipe end on POSIX).
    import multiprocessing  # noqa: PLC0415

    ctx = multiprocessing.get_context("spawn")
    ev = ctx.Event()
    proc = ctx.Process(target=ev.wait, args=(30,))
    proc.start()
    try:
        assert not exit_with_parent.parent_gone(proc.sentinel, None, 0.05)
        ev.set()
        started = time.monotonic()
        assert exit_with_parent.parent_gone(proc.sentinel, None, 10.0)
        assert time.monotonic() - started < 5.0
    finally:
        ev.set()
        proc.join(10)


@pytest.mark.skipif(sys.platform == "win32", reason="a pipe descriptor is no waitable handle on Windows")
def test_an_unusable_sentinel_falls_back_on_the_pid():
    r, w = os.pipe()
    os.close(r)
    os.close(w)                                       # r is now a closed descriptor
    assert not exit_with_parent.parent_gone(r, os.getppid(), 0.01)


class _Exited(Exception):
    pass


def test_a_full_stderr_pipe_does_not_hold_the_exit(monkeypatch):
    # stderr inherited from the dead backend may be a full pipe: a write to it
    # blocks for ever. The exit must happen regardless.
    release = threading.Event()

    class Stuck:
        def write(self, _text):
            release.wait(30)

        def flush(self):
            release.wait(30)

    def fake_exit(code):
        raise _Exited(code)

    monkeypatch.setattr(sys, "stderr", Stuck())
    monkeypatch.setattr(exit_with_parent.os, "_exit", fake_exit)
    seen = []

    def run():
        try:
            exit_with_parent._leave("child", 0.0)
        except _Exited as e:
            seen.append(e.args[0])

    t = threading.Thread(target=run, daemon=True)
    started = time.monotonic()
    t.start()
    t.join(5)
    try:
        assert seen == [exit_with_parent.EXIT_CODE]
        assert time.monotonic() - started < 2.0
    finally:
        release.set()


def test_a_process_that_is_no_child_installs_nothing():
    assert exit_with_parent.install() is None


def test_only_the_main_thread_may_ask_for_the_parent_death_signal():
    assert exit_with_parent.spawned_from_main_thread()
    seen = []
    t = threading.Thread(target=lambda: seen.append(exit_with_parent.spawned_from_main_thread()))
    t.start()
    t.join()
    assert seen == [False]


@pytest.mark.skipif(not sys.platform.startswith("linux"), reason="prctl is Linux only")
def test_the_parent_death_signal_is_set_on_linux():
    code = ("import ctypes, sys; sys.path.insert(0, %r)\n"
            "from app import exit_with_parent as e\n"
            "assert e._set_pdeathsig(None)\n"
            "libc = ctypes.CDLL(None); v = ctypes.c_int()\n"
            "libc.prctl(2, ctypes.byref(v), 0, 0, 0)\n"     # PR_GET_PDEATHSIG
            "print(v.value)\n" % str(_BACKEND))
    done = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True, timeout=30)
    assert done.returncode == 0, done.stderr
    assert int(done.stdout) == signal.SIGKILL


def _child_that_reports_its_parent(marker: str) -> None:   # pragma: no cover - in a child
    def gone() -> None:
        Path(marker).write_text("gone")
        os._exit(0)
    exit_with_parent.install(interval_s=0.1, on_gone=gone)
    time.sleep(60)


def test_a_watched_child_is_told_when_its_parent_ends(tmp_path):
    """The real sentinel of a spawned child: a middle process starts the
    watched child and is killed."""
    marker = tmp_path / "gone.txt"
    code = (
        "import multiprocessing as mp, sys\n"
        "from tests.test_exit_with_parent import _child_that_reports_its_parent as f\n"
        "if __name__ == '__main__':\n"
        "    p = mp.get_context('spawn').Process(target=f, args=(sys.argv[1],)); p.start()\n"
        "    print(p.pid, flush=True); p.join()\n")
    env = dict(os.environ, VISIVO_TOKEN="x")
    middle = subprocess.Popen([sys.executable, "-c", code, str(marker)], cwd=_BACKEND, env=env,
                              stdout=subprocess.PIPE, text=True)
    pid = None
    try:
        pid = int(middle.stdout.readline())
        time.sleep(1.0)                                # the child installs its watcher
        assert process_alive(pid) and not marker.exists()
        middle.kill()
        middle.wait()
        deadline = time.monotonic() + 5
        while process_alive(pid) and time.monotonic() < deadline:
            time.sleep(0.05)
        assert not process_alive(pid)
        assert marker.read_text() == "gone"
    finally:
        if middle.poll() is None:
            middle.kill()
        if pid and process_alive(pid):
            os.kill(pid, signal.SIGKILL)


_FORK_ORPHAN = """
import multiprocessing as mp, os, sys, time
from app import exit_with_parent as e

def late(marker):
    ppid = os.getppid()
    while os.getppid() == ppid:          # the backend dies before install
        time.sleep(0.02)
    def gone():
        open(marker, "w").write("gone")
        os._exit(0)
    e.install(interval_s=0.1, on_gone=gone)
    time.sleep(30)

def sibling():
    time.sleep(30)

if __name__ == "__main__":
    ctx = mp.get_context("fork")
    a = ctx.Process(target=late, args=(sys.argv[1],)); a.start()
    b = ctx.Process(target=sibling); b.start()      # holds a's sentinel open
    print(a.pid, b.pid, flush=True)
    time.sleep(60)
"""


@pytest.mark.skipif("fork" not in __import__("multiprocessing").get_all_start_methods(),
                    reason="no fork start method here")
def test_a_backend_dead_before_install_is_seen_under_fork(tmp_path):
    """Under fork, a child whose backend died before install() starts with
    getppid already at init and a sentinel a later sibling keeps open:
    install() must see that the backend is gone from the start."""
    marker = tmp_path / "gone.txt"
    middle = subprocess.Popen([sys.executable, "-c", _FORK_ORPHAN, str(marker)], cwd=_BACKEND,
                              env=dict(os.environ, VISIVO_TOKEN="x"), stdout=subprocess.PIPE,
                              text=True)
    late = sib = None
    try:
        late, sib = map(int, middle.stdout.readline().split())
        middle.kill()
        middle.wait()
        deadline = time.monotonic() + 5
        while not marker.exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        assert marker.exists() and marker.read_text() == "gone"
        assert process_alive(sib)                       # the sibling did not let it see EOF
    finally:
        if middle.poll() is None:
            middle.kill()
        for pid in (late, sib):
            if pid and process_alive(pid):
                os.kill(pid, signal.SIGKILL)


def test_a_dead_backend_at_install_ends_the_child_at_once(monkeypatch):
    """The same in-process: the OS parent is not the backend, and the backend's
    PID no longer exists."""
    import multiprocessing
    import threading as _t
    done = subprocess.Popen([sys.executable, "-c", "pass"])
    done.wait()
    r, w = os.pipe()                                    # a sentinel that stays open
    fake = type("Parent", (), {"pid": done.pid, "sentinel": r})()
    monkeypatch.setattr(multiprocessing, "parent_process", lambda: fake)
    monkeypatch.setattr(exit_with_parent, "_installed", None)
    called = _t.Event()
    try:
        exit_with_parent.install(interval_s=0.05, on_gone=called.set)
        assert called.wait(2)
    finally:
        os.close(r)
        os.close(w)


def test_a_live_backend_behind_a_fork_server_is_not_taken_for_dead(monkeypatch):
    import multiprocessing
    import threading as _t
    r, w = os.pipe()
    fake = type("Parent", (), {"pid": os.getpid(), "sentinel": r})()   # alive, not getppid
    monkeypatch.setattr(multiprocessing, "parent_process", lambda: fake)
    monkeypatch.setattr(exit_with_parent, "_installed", None)
    called = _t.Event()
    try:
        exit_with_parent.install(interval_s=0.05, on_gone=called.set)
        assert not called.wait(0.4)
        os.close(w)                                     # now the backend goes
        w = None
        assert called.wait(2)
    finally:
        os.close(r)
        if w is not None:
            os.close(w)


# ── The backend's children, the backend killed outright ──────────────────────


def _alive(pids: list[int]) -> list[int]:
    return [p for p in pids if process_alive(p)]


def _pool_start_methods() -> list:
    import multiprocessing as mp
    available = mp.get_all_start_methods()
    return [m if m in available else pytest.param(m, marks=pytest.mark.skip(
        reason=f"no {m} start method here")) for m in ("spawn", "forkserver", "fork")]


@pytest.mark.skipif(sys.platform == "win32", reason="needs os.mkfifo and SIGKILL")
@pytest.mark.parametrize("pool_start_method", _pool_start_methods())
def test_every_child_ends_when_the_backend_is_killed(tmp_path, pool_start_method):
    """The compute pool under each start method VISIVO_MP_START_METHOD allows
    (forkserver: the workers' OS parent is the fork server, and they must not
    take that for the backend's death); the other children always spawn."""
    env = dict(os.environ, VISIVO_TOKEN="x", VISIVO_WORKERS="2",
               VISIVO_MP_START_METHOD=pool_start_method)
    env.pop("VISIVO_EXIT_WITH_PID", None)
    helper = subprocess.Popen(
        [sys.executable, "-m", "tests._exit_with_parent_helpers", str(tmp_path)],
        cwd=_BACKEND, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    pids: dict = {}
    try:
        report = tmp_path / "pids.json"
        deadline = time.monotonic() + 120
        while not report.exists() and helper.poll() is None and time.monotonic() < deadline:
            time.sleep(0.05)
        assert report.exists(), "the stand-in backend did not start its children"
        pids = json.loads(report.read_text())
        children = pids["pool"] + [pids["render"], pids["reader"], pids["pyramid"]]
        helpers = [pids["tracker"]] + ([pids["forkserver"]] if pids["forkserver"] else [])
        assert len(pids["pool"]) == 2 and pids["tracker"] is not None
        if pool_start_method == "forkserver":
            assert pids["forkserver"] is not None
        # Past the watchers' first PID checks: a watcher that mistakes the
        # live backend for a dead one has ended its process by now.
        time.sleep(1.5)
        assert _alive(children + helpers) == children + helpers

        helper.send_signal(signal.SIGKILL)
        helper.wait()
        killed = time.monotonic()
        ended: dict[int, float] = {}
        watched = children + helpers
        # The pyramid build is given 5 s to unwind (its partial file goes too).
        while len(ended) < len(watched) and time.monotonic() - killed < 10:
            for pid in watched:
                if pid not in ended and not process_alive(pid):
                    ended[pid] = round(time.monotonic() - killed, 2)
            time.sleep(0.02)
        # One that ended after the last look gets its time now, so the
        # assertions below never meet a missing entry.
        for pid in watched:
            if pid not in ended and not process_alive(pid):
                ended[pid] = round(time.monotonic() - killed, 2)
        assert _alive(children) == [], (pids, ended)
        # With every holder of its pipe gone, the resource tracker ends too.
        assert _alive(helpers) == [], (pids, ended)
        # Everything but the pyramid build (5 s of grace) at once.
        assert max(ended[p] for p in children if p != pids["pyramid"]) < 3.0, (pids, ended)
    finally:
        if helper.poll() is None:
            helper.kill()
        for pid in (pids.get("pool", []) + [pids.get(k) for k in
                    ("render", "reader", "pyramid", "tracker", "forkserver")]):
            if pid and process_alive(pid):
                os.kill(pid, signal.SIGKILL)
