"""A backend started by the desktop client ends with it (app/parent_watch.py)."""
from __future__ import annotations

import os
import subprocess
import sys
import threading

from app import parent_watch


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
