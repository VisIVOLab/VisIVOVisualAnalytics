"""A backend started by the desktop client ends with it (app/parent_watch.py)."""
from __future__ import annotations

import os
import subprocess
import sys
import threading
import time
from pathlib import Path

from app import parent_watch


def test_a_shutdown_that_hangs_does_not_keep_the_backend():
    """The client gone, a backend whose graceful shutdown never finishes (here:
    SIGTERM ignored) exits anyway; its children then end by themselves (#176)."""
    client = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"])
    code = ("import signal, sys, time; sys.path.insert(0, %r)\n"
            "signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
            "from app import parent_watch as w\n"
            "w._INTERVAL_S, w._HARD_EXIT_S = 0.05, 0.5\n"
            "w.start(); print('watching', flush=True); time.sleep(30)\n"
            % str(Path(__file__).resolve().parents[1]))
    env = dict(os.environ, VISIVO_EXIT_WITH_PID=str(client.pid), VISIVO_TOKEN="x")
    backend = subprocess.Popen([sys.executable, "-c", code], env=env, stdout=subprocess.PIPE,
                               text=True)
    try:
        assert backend.stdout.readline().strip() == "watching"
        client.kill()
        client.wait()
        assert backend.wait(timeout=10) == 1
    finally:
        for p in (client, backend):
            if p.poll() is None:
                p.kill()


def test_the_exit_waits_for_a_lifespan_shutdown_under_way():
    """Past the limit, a lifespan shutdown already closing render sessions
    (their GPU contexts) is let finish before the exit."""
    client = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"])
    code = ("import signal, sys, threading, time; sys.path.insert(0, %r)\n"
            "signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
            "from app import parent_watch as w\n"
            "w._INTERVAL_S, w._HARD_EXIT_S = 0.05, 0.2\n"
            "w.SHUTDOWN_STARTED.set()\n"
            "w.start(); print('watching', flush=True)\n"
            "sys.stdin.readline(); t0 = time.monotonic()\n"
            "threading.Timer(1.0, w.SHUTDOWN_DONE.set).start()\n"
            "time.sleep(30)\n" % str(Path(__file__).resolve().parents[1]))
    env = dict(os.environ, VISIVO_EXIT_WITH_PID=str(client.pid), VISIVO_TOKEN="x")
    backend = subprocess.Popen([sys.executable, "-c", code], env=env, stdout=subprocess.PIPE,
                               stdin=subprocess.PIPE, text=True)
    try:
        assert backend.stdout.readline().strip() == "watching"
        backend.stdin.write("\n")
        backend.stdin.flush()
        started = time.monotonic()
        client.kill()
        client.wait()
        assert backend.wait(timeout=10) == 1
        # Not at the 0.2 s limit: when the shutdown said it was done, 1 s on.
        assert time.monotonic() - started >= 0.9
    finally:
        for p in (client, backend):
            if p.poll() is None:
                p.kill()


def test_a_live_process_and_a_gone_one():
    assert parent_watch.process_alive(os.getpid())
    child = subprocess.Popen([sys.executable, "-c", "pass"])
    child.wait()
    assert not parent_watch.process_alive(child.pid)
    assert not parent_watch.process_alive(0)


def test_without_the_variable_nothing_is_watched(monkeypatch):
    monkeypatch.delenv("VISIVO_EXIT_WITH_PID", raising=False)
    assert parent_watch.start() is None


def test_the_backend_stops_once_its_client_is_gone(monkeypatch):
    monkeypatch.setattr(parent_watch, "_INTERVAL_S", 0.05)
    client = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"])
    monkeypatch.setenv("VISIVO_EXIT_WITH_PID", str(client.pid))
    stopped = threading.Event()
    thread = parent_watch.start(stop=stopped.set)
    try:
        assert not stopped.wait(0.3)          # the client lives: the backend runs
        client.kill()
        client.wait()
        assert stopped.wait(5)                # gone: the backend is told to stop
    finally:
        thread.halt.set()
        if client.poll() is None:
            client.kill()
