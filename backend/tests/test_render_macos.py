"""Render sessions on macOS (issue #168): a hidden Cocoa window instead of EGL.

The executor and the platform choice are tested everywhere; drawing a real
cube needs a Mac with a logged-in user (a window server session) and is
skipped elsewhere. The drawing runs in a spawned render child, on its main
thread, as in the backend: a Cocoa window made on any other thread aborts the
process.
"""
from __future__ import annotations

import sys
import threading
from concurrent.futures import ThreadPoolExecutor

import numpy as np
import pytest

from app.render_session import _window


def test_the_caller_thread_executor_runs_calls_at_once_on_the_main_thread():
    ex = _window.CallerThreadExecutor()
    assert ex.submit(lambda a, b: a + b, 2, b=3).result() == 5
    with pytest.raises(ZeroDivisionError):
        ex.submit(lambda: 1 / 0).result()
    ex.shutdown()
    with pytest.raises(RuntimeError):
        ex.submit(lambda: None)


def test_the_caller_thread_executor_refuses_other_threads_without_running():
    ex = _window.CallerThreadExecutor()
    ran, futures = [], []
    t = threading.Thread(target=lambda: futures.append(ex.submit(ran.append, 1)))
    t.start()
    t.join()
    with pytest.raises(RuntimeError, match="main thread"):
        futures[0].result()
    assert ran == []


def test_a_cocoa_session_runs_on_the_callers_thread_an_egl_one_on_its_own():
    assert isinstance(_window.render_executor("cocoa", "t"), _window.CallerThreadExecutor)
    for kind in ("egl", None):
        ex = _window.render_executor(kind, "t")
        assert isinstance(ex, ThreadPoolExecutor)
        ex.shutdown()


def test_egl_first_then_cocoa_only_with_a_window_server(monkeypatch):
    monkeypatch.setattr(_window, "egl_window_class", lambda: object)
    monkeypatch.setattr(_window, "cocoa_window_class", lambda: object)
    monkeypatch.setattr(_window, "macos_window_server_session", lambda: True)
    assert _window.offscreen_kind() == "egl"
    monkeypatch.setattr(_window, "egl_window_class", lambda: None)
    assert _window.offscreen_kind() == "cocoa"
    monkeypatch.setattr(_window, "macos_window_server_session", lambda: False)
    assert _window.offscreen_kind() is None
    monkeypatch.setattr(_window, "cocoa_window_class", lambda: None)
    monkeypatch.setattr(_window, "macos_window_server_session", lambda: True)
    assert _window.offscreen_kind() is None


def test_the_backend_reports_rendering_where_a_window_can_be_made(monkeypatch):
    import app.render_session as rs
    monkeypatch.setattr(rs, "_EGL_AVAILABLE", None)
    monkeypatch.setattr(rs, "offscreen_kind", lambda: "cocoa")
    assert rs.server_side_rendering_available() is True
    monkeypatch.setattr(rs, "_EGL_AVAILABLE", None)
    monkeypatch.setattr(rs, "offscreen_kind", lambda: None)
    assert rs.server_side_rendering_available() is False


def test_a_cocoa_window_is_a_context_only_and_out_of_the_dock(monkeypatch):
    calls = []

    class FakeCocoa:
        def __init__(self):
            calls.append(("new",))

        def SetConnectContextToNSView(self, value):
            calls.append(("nsview", value))

        def SetShowWindow(self, value):
            calls.append(("show", value))

        def SetOffScreenRendering(self, value):
            calls.append(("offscreen", value))

    monkeypatch.setattr(_window, "cocoa_window_class", lambda: FakeCocoa)
    monkeypatch.setattr(_window, "hide_from_dock", lambda: calls.append(("hide",)))
    _window.new_offscreen_window("cocoa")
    # Declared background-only before VTK can make the NSApplication; no
    # NSWindow (no NSView), never shown, offscreen.
    assert calls[0] == ("hide",) and calls[1] == ("new",)
    assert {("nsview", False), ("show", False), ("offscreen", 1)} <= set(calls)


def _in_a_thread(coro_factory):
    """Run a coroutine on a thread of its own (with its own loop); its
    exception, if any, is returned."""
    import asyncio
    result = []

    def run():
        try:
            asyncio.new_event_loop().run_until_complete(coro_factory())
            result.append(None)
        except BaseException as exc:   # noqa: BLE001 - handed back to the test
            result.append(exc)

    t = threading.Thread(target=run)
    t.start()
    t.join()
    return result[0]


def test_a_point_session_off_the_main_thread_is_refused_before_any_window(monkeypatch, tmp_path):
    pytest.importorskip("vtk")
    from app.render_session import points
    made = []
    monkeypatch.setattr(points, "offscreen_kind", lambda: "cocoa")
    monkeypatch.setattr(points, "new_offscreen_window", lambda kind: made.append(kind))
    session = points.PointRenderSession("off-main", str(tmp_path / "none.bin.head"))
    error = _in_a_thread(session.initialize)
    assert isinstance(error, _window.RenderThreadError)
    assert made == []
    assert _in_a_thread(session.close) is None   # refused VTK part, closed anyway


def test_a_cube_session_closed_off_the_main_thread_still_frees_its_reader(monkeypatch):
    pytest.importorskip("vtk")
    import app.render_session as rs
    monkeypatch.setattr(rs, "offscreen_kind", lambda: "cocoa")
    session = rs.RenderSession("off-main", "/nowhere/cube.fits")
    released = []

    class Reader:
        def close(self):
            released.append("reader")

    class Ooc:
        def release_files(self):
            released.append("pyramid")

        def close(self):
            released.append("ooc VTK")

    class Window:
        def Finalize(self):
            released.append("window")

    session._cube_reader, session._ooc, session._render_window = Reader(), Ooc(), Window()
    assert _in_a_thread(session.close) is None
    assert sorted(released) == ["pyramid", "reader"]   # no VTK touched off the main thread
    assert session._render_window is not None          # kept: freeing it here would destroy it


def _on_a_mac_with_a_display() -> bool:
    if sys.platform != "darwin" or _window.egl_window_class() is not None:
        return False
    return _window.cocoa_window_class() is not None and _window.macos_window_server_session()


@pytest.mark.skipif(not _on_a_mac_with_a_display(),
                    reason="needs macOS with a logged-in user (window server session)")
async def test_a_cube_session_renders_jpeg_frames_in_its_child_on_a_mac(tmp_path):
    from astropy.io import fits
    from app.render_session._process import RenderSessionProcess

    rng = np.random.default_rng(168)
    cube = rng.normal(size=(24, 32, 40)).astype(np.float32)
    cube[8:16, 10:20, 12:28] += 20.0
    path = tmp_path / "cube.fits"
    fits.writeto(path, cube)
    session = RenderSessionProcess("mac168test", str(path))
    await session.initialize()
    try:
        await session.apply_command({"type": "resize", "width": 320, "height": 240})
        rest = await session.render_frame()
        await session.apply_command({"type": "set_interactive", "value": True})
        await session.apply_command({"type": "rotate", "d_azimuth": 10.0})
        drag = await session.render_frame()
    finally:
        await session.close()
    for jpeg in (rest, drag):
        assert jpeg[:2] == b"\xff\xd8" and len(jpeg) > 500
    from io import BytesIO
    from PIL import Image
    image = np.asarray(Image.open(BytesIO(rest)).convert("L"))
    assert image.shape == (240, 320)
    assert image.std() > 1.0       # a drawn cube, not a blank frame


@pytest.mark.skipif(not _on_a_mac_with_a_display(),
                    reason="needs macOS with a logged-in user (window server session)")
async def test_a_point_session_renders_jpeg_frames_in_its_child_on_a_mac(tmp_path):
    from io import BytesIO
    from PIL import Image
    from app.render_session._process import RenderSessionProcess

    rng = np.random.default_rng(168)
    n = 3000
    data = np.stack([rng.normal(0, 1, n) for _ in range(3)]).astype("<f4")
    data.tofile(tmp_path / "pts.bin")
    (tmp_path / "pts.bin.head").write_text(f"float\n3\n{n}\nlittle\nX\nY\nZ\n")
    session = RenderSessionProcess("mac168pts", str(tmp_path / "pts.bin.head"),
                                   factory="app.render_session.points:PointRenderSession")
    await session.initialize()
    try:
        await session.apply_command({"type": "resize", "width": 320, "height": 240})
        frame = await session.render_frame()
    finally:
        await session.close()
    assert frame[:2] == b"\xff\xd8"
    image = np.asarray(Image.open(BytesIO(frame)).convert("L"))
    assert image.shape == (240, 320) and image.std() > 1.0
