"""RenderSessionProcess: a render session in a child process.

GPU-free: the child runs tests/_fake_render_session.FakeRenderSession, which
records the commands it applied and renders them as the frame's bytes.
"""
from __future__ import annotations

import subprocess
import sys
import time
from pathlib import Path

import pytest

from app.render_session._process import _LIVE, RenderSessionProcess

_FAKE = "tests._fake_render_session:FakeRenderSession"


@pytest.fixture
async def session():
    s = RenderSessionProcess("feedc0de00", "/data/cube.fits", factory=_FAKE)
    await s.initialize()
    yield s
    await s.close()


async def test_commands_are_applied_before_the_next_frame(session):
    for kind in ("rotate", "zoom", "rotate"):
        await session.apply_command({"type": kind})       # not waited for
    assert await session.render_frame() == b"rotate,zoom,rotate"
    assert session.changed.is_set()


async def test_the_childs_state_comes_back_with_each_answer(session):
    assert (session.view.width, session.view.height) == (640, 480)
    await session.apply_command({"type": "rotate"})
    jpeg, version = await session.render_frame_if_changed(-1)
    assert (jpeg, version) == (b"rotate", 1)
    assert await session.render_frame_if_changed(version) == (None, 1)
    assert session.stats.frames_rendered == 1             # counted in the child
    assert session.stats.commands_received == 1           # counted here


async def test_the_starting_values_come_back_with_each_answer(session):
    assert session.levels() == {"threshold": 0.5, "data_min": -1.0, "data_max": 9.0,
                                "pending": False}
    await session.apply_command({"type": "set_threshold"})
    await session.render_frame()
    assert session.levels()["threshold"] == 1.5


async def test_a_keepalive_keeps_the_session_without_a_new_frame(session):
    jpeg, version = await session.render_frame_if_changed(-1)
    session.changed.clear()
    before = session.stats.last_command_at
    time.sleep(0.01)
    await session.apply_command({"type": "keepalive"})
    assert session.stats.last_command_at > before          # the idle timer restarts
    assert not session.changed.is_set()                    # the push loop is not woken
    assert await session.render_frame_if_changed(version) == (None, version)


async def test_a_failure_in_the_child_fails_that_request_only(session):
    await session.apply_command({"type": "fail"})          # logged in the child
    await session.apply_command({"type": "break_next_frame"})
    with pytest.raises(RuntimeError, match="readback failed"):
        await session.render_frame()
    assert await session.render_frame() == b"break_next_frame,broken"
    assert await session.render_rgb_frame() == ([[1, 2], [3, 4]], 1.5)


async def test_a_crashed_child_fails_the_waiting_request_and_the_session():
    s = RenderSessionProcess("feedc0de01", "/data/cube.fits", factory=_FAKE)
    await s.initialize()
    await s.apply_command({"type": "crash"})
    with pytest.raises(RuntimeError, match="exit code"):
        await s.render_frame()
    with pytest.raises(RuntimeError, match="closed"):
        await s.apply_command({"type": "rotate"})
    await s.close()                                        # still safe, and releases
    assert s._released and s._conn.closed and s not in _LIVE
    assert not s._process.is_alive()


async def test_close_ends_the_child():
    s = RenderSessionProcess("feedc0de02", "/data/cube.fits", factory=_FAKE)
    await s.initialize()
    proc = s._process
    await s.close()
    assert not proc.is_alive()
    with pytest.raises(RuntimeError, match="closed"):
        await s.render_frame()


async def test_commands_never_wait_on_a_busy_child(session):
    # The child is "rendering" for a second; the commands queued behind it,
    # 12 MB in all, must not hold the caller (the backend's event loop).
    await session.apply_command({"type": "busy"})
    start = time.perf_counter()
    for _ in range(200):
        await session.apply_command({"type": "rotate", "pad": "x" * 65536})
    assert time.perf_counter() - start < 0.2
    assert (await session.render_frame()).count(b"rotate") == 200


async def test_the_key_frame_flag_follows_the_frame_that_carries_it(session):
    await session.render_rgb_frame()
    assert session.consume_idr_flag() is False
    await session.apply_command({"type": "lut"})
    await session.render_rgb_frame()
    assert session.consume_idr_flag() is True
    assert session.consume_idr_flag() is False


def test_an_unclosed_session_does_not_hang_interpreter_exit():
    backend = Path(__file__).resolve().parents[1]
    code = (
        "import asyncio, sys; sys.path.insert(0, %r)\n"
        "from app.render_session._process import RenderSessionProcess\n"
        "async def main():\n"
        "    s = RenderSessionProcess('exit0000', 'x', factory=%r)\n"
        "    await s.initialize()\n"
        "asyncio.run(main())\n" % (str(backend), _FAKE)
    )
    done = subprocess.run([sys.executable, "-c", code], cwd=backend, timeout=30,
                          capture_output=True, text=True)
    assert done.returncode == 0, done.stderr


async def test_a_cancelled_close_still_ends_the_child():
    import asyncio
    s = RenderSessionProcess("feedc0de03", "/data/cube.fits", factory=_FAKE)
    await s.initialize()
    await s.apply_command({"type": "busy"})              # close will wait behind it
    task = asyncio.create_task(s.close())
    await asyncio.sleep(0.1)
    task.cancel()
    await asyncio.gather(task, return_exceptions=True)
    for _ in range(100):
        if s._released:
            break
        await asyncio.sleep(0.1)
    assert s._released and not s._process.is_alive()


def test_sessions_take_only_the_jobs_gpus_in_turn():
    from app.render_session._process import _pick_egl_device
    # All four GPUs visible: EGL device i is CUDA ordinal i.
    assert [_pick_egl_device([0, 1, 2, 3], slot) for slot in range(5)] == [0, 1, 2, 3, 0]
    # CUDA_VISIBLE_DEVICES=2,3 (as measured on PLEIADI-GPU): EGL devices 0 and 1
    # do not answer, and the sessions must never land on them.
    assert [_pick_egl_device([None, None, 0, 1], slot) for slot in range(4)] == [2, 3, 2, 3]
    # Ordered by CUDA ordinal, not by EGL index.
    assert _pick_egl_device([1, 0], 0) == 1
    # No mapping known: VTK keeps its choice.
    assert _pick_egl_device([], 0) is None
    assert _pick_egl_device([None, None], 1) is None


def test_the_device_is_bound_and_an_operators_choice_wins(monkeypatch):
    import os
    import app.render_session._process as proc
    monkeypatch.setattr(proc, "_egl_cuda_ordinals", lambda: [None, None, 0, 1])
    monkeypatch.setenv("VTK_EGL_DEVICE_INDEX", "3")
    proc._egl_device_for(0)
    assert os.environ["VTK_EGL_DEVICE_INDEX"] == "3"           # left alone
    monkeypatch.delenv("VTK_EGL_DEVICE_INDEX")
    proc._egl_device_for(1)
    assert os.environ["VTK_EGL_DEVICE_INDEX"] == "3"           # slot 1 → CUDA 1 → EGL 3
    monkeypatch.delenv("VTK_EGL_DEVICE_INDEX")


def test_no_allowed_gpu_refuses_and_no_mapping_leaves_vtk_to_choose(monkeypatch):
    import os
    import app.render_session._process as proc
    monkeypatch.delenv("VTK_EGL_DEVICE_INDEX", raising=False)
    monkeypatch.setattr(proc, "_egl_cuda_ordinals", lambda: [None, None])
    with pytest.raises(RuntimeError, match="visible to this job"):
        proc._egl_device_for(0)
    monkeypatch.setattr(proc, "_egl_cuda_ordinals", lambda: [])
    proc._egl_device_for(0)
    assert "VTK_EGL_DEVICE_INDEX" not in os.environ


async def test_a_session_that_cannot_be_built_fails_its_init():
    s = RenderSessionProcess("feedc0de09", "x", factory="tests._fake_render_session:NoSuchSession")
    with pytest.raises(RuntimeError, match="NoSuchSession"):
        await s.initialize()
    await s.close()
    assert not s._process.is_alive()


def _fake_egl(monkeypatch, devices):
    """Point _egl_cuda_ordinals at fake EGL entry points: ``devices`` is a list
    of (extensions, cuda_ordinal or None when the query fails)."""
    import ctypes as c
    from types import SimpleNamespace

    import app.render_session._process as proc

    @c.CFUNCTYPE(c.c_uint, c.c_int, c.POINTER(c.c_void_p), c.POINTER(c.c_int))
    def query_devices(limit, out, count):
        for i in range(len(devices)):
            out[i] = i + 1
        count[0] = len(devices)
        return 1

    @c.CFUNCTYPE(c.c_uint, c.c_void_p, c.c_int, c.POINTER(c.c_ssize_t))
    def query_attrib(device, key, out):
        ordinal = devices[device - 1][1]
        if ordinal is None:
            return 0
        out[0] = ordinal
        return 1

    strings = [c.c_char_p(ext) for ext, _ in devices]

    @c.CFUNCTYPE(c.c_void_p, c.c_void_p, c.c_int)
    def query_string(device, key):
        return c.cast(strings[device - 1], c.c_void_p).value

    keep = (query_devices, query_attrib, query_string, strings)
    table = {b"eglQueryDevicesEXT": query_devices, b"eglQueryDeviceAttribEXT": query_attrib,
             b"eglQueryDeviceStringEXT": query_string}
    lib = SimpleNamespace(eglGetProcAddress=lambda name: c.cast(table[name], c.c_void_p).value
                          if name in table else None)
    monkeypatch.setattr(proc.ctypes, "CDLL", lambda name: lib)
    return keep


def test_the_cuda_mapping_is_read_from_the_devices_that_support_it(monkeypatch):
    import app.render_session._process as proc
    nv = b"EGL_EXT_device_drm EGL_NV_device_cuda"
    keep = _fake_egl(monkeypatch, [(nv, None), (nv, None), (nv, 0), (nv, 1)])
    assert proc._egl_cuda_ordinals() == [None, None, 0, 1]     # outside the job: refused
    keep = _fake_egl(monkeypatch, [(b"EGL_EXT_device_drm", None)] * 2)
    assert proc._egl_cuda_ordinals() == []                     # no extension: no mapping
    keep = _fake_egl(monkeypatch, [(b"EGL_MESA_device_software", None), (nv, 0)])
    assert proc._egl_cuda_ordinals() == [None, 0]              # a software device is skipped
    del keep
