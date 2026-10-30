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
