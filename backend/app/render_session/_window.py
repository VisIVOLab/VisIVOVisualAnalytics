"""The offscreen OpenGL window a render session draws into, per platform.

Linux (and any VTK built with EGL): ``vtkEGLRenderWindow``, headless, on the
GPU the render process picked; the session's VTK calls run on a worker thread
of their own.

macOS (issue #168): the VTK wheels have no EGL and their OSMesa window is a
stub ("VTK does not support OSMesa for your operating system"), so the window
is a ``vtkCocoaRenderWindow`` drawn offscreen, on the Mac's GPU through Apple's
OpenGL 4.1 (on top of Metal). It is not connected to an ``NSView``
(``SetConnectContextToNSView(False)``): VTK then makes an OpenGL context only,
no ``NSWindow``, and does not turn the process into a regular app. Cocoa still
imposes two rules:

* AppKit may only be used from the process's main thread (an ``NSWindow``
  made on a worker thread throws and aborts the process), so the session's VTK
  calls run on the thread that asks for them (:class:`CallerThreadExecutor`)
  — in a render child that is its main thread, which does nothing else;
* it needs the user's window server session: a backend started over ssh with
  nobody logged in at the Mac, or as a LaunchDaemon, has none, and reports no
  server-side rendering rather than crashing the first session.

A framework Python (Homebrew's, python.org's) runs as ``Python.app``, which
LaunchServices checks in as a regular app — a Dock tile per render process
for its whole life — as soon as the process has an ``NSApplication``, whatever
activation policy is set afterwards. The render child declares itself a
background-only app before that (:func:`hide_from_dock`).
"""
from __future__ import annotations

import ctypes
import logging
import sys
import threading
from concurrent.futures import Executor, Future, ThreadPoolExecutor

logger = logging.getLogger("visivo.render")

_APPKIT = "/System/Library/Frameworks/AppKit.framework/AppKit"
_COREGRAPHICS = "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics"
_COREFOUNDATION = "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation"
_OBJC = "/usr/lib/libobjc.A.dylib"
# NSApplicationActivationPolicyProhibited: no Dock icon, no menu bar.
_POLICY_PROHIBITED = 2


def egl_window_class():
    """vtkEGLRenderWindow, from wherever this VTK has it, or None.

    In VTK 9.3 and later it lives in ``vtkRenderingOpenGL2``; in 9.0/9.1 it
    was a standalone ``vtkRenderingEGL`` module."""
    for path in ("vtkmodules.vtkRenderingOpenGL2", "vtkmodules.vtkRenderingEGL"):
        try:
            cls = getattr(__import__(path, fromlist=["vtkEGLRenderWindow"]),
                          "vtkEGLRenderWindow", None)
        except ImportError:
            continue
        if cls is not None:
            return cls
    return None


def cocoa_window_class():
    """vtkCocoaRenderWindow on macOS, or None."""
    if sys.platform != "darwin":
        return None
    try:
        from vtkmodules import vtkRenderingOpenGL2
    except ImportError:
        return None
    return getattr(vtkRenderingOpenGL2, "vtkCocoaRenderWindow", None)


def macos_window_server_session() -> bool:
    """True iff this process can reach a window server session (a user logged
    in at the Mac). Asks CoreGraphics only: no window, no AppKit, so it is safe
    from any thread of the backend."""
    try:
        cg = ctypes.cdll.LoadLibrary(_COREGRAPHICS)
        cf = ctypes.cdll.LoadLibrary(_COREFOUNDATION)
    except OSError:
        return False
    cg.CGSessionCopyCurrentDictionary.restype = ctypes.c_void_p
    cg.CGSessionCopyCurrentDictionary.argtypes = []
    session = cg.CGSessionCopyCurrentDictionary()
    if not session:
        return False
    cf.CFRelease.argtypes = [ctypes.c_void_p]
    cf.CFRelease(session)
    return True


def offscreen_kind() -> str | None:
    """``"egl"``, ``"cocoa"``, or None when this backend cannot draw offscreen."""
    if egl_window_class() is not None:
        return "egl"
    if cocoa_window_class() is not None and macos_window_server_session():
        return "cocoa"
    return None


def new_offscreen_window(kind: str):
    """An offscreen render window of ``kind`` (see :func:`offscreen_kind`)."""
    if kind == "egl":
        window = egl_window_class()()
    elif kind == "cocoa":
        # Before VTK makes the NSApplication (its first Render): afterwards the
        # Dock tile is there for good.
        hide_from_dock()
        window = cocoa_window_class()()
        # A context only: no NSWindow, so no regular-app policy and nothing
        # that could ever be put on screen.
        window.SetConnectContextToNSView(False)
        window.SetShowWindow(False)
    else:
        raise RuntimeError("No offscreen OpenGL window on this backend.")
    window.SetOffScreenRendering(1)
    return window


def _objc():
    """objc_msgSend with the three signatures used here, and the selector and
    class lookups."""
    objc = ctypes.cdll.LoadLibrary(_OBJC)
    objc.objc_getClass.restype = ctypes.c_void_p
    objc.objc_getClass.argtypes = [ctypes.c_char_p]
    objc.sel_registerName.restype = ctypes.c_void_p
    objc.sel_registerName.argtypes = [ctypes.c_char_p]
    send = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)(
        ("objc_msgSend", objc))
    send_ptr = ctypes.CFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p,
                                ctypes.c_void_p)(("objc_msgSend", objc))
    send_long = ctypes.CFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p,
                                 ctypes.c_long)(("objc_msgSend", objc))
    send_set = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                                ctypes.c_void_p)(("objc_msgSend", objc))
    return objc, send, send_ptr, send_long, send_set


def hide_from_dock() -> None:
    """Make this process a background-only app: no Dock tile, no menu bar.

    Before the process has an NSApplication, ``LSBackgroundOnly`` goes into its
    main bundle's info dictionary, which is what LaunchServices reads when the
    application checks in; an interpreter that is not in a bundle (the
    standalone Python of the release builds) has no tile anyway. Once there is
    an NSApplication only its activation policy can still change, which hides
    the tile only from then on. Best effort."""
    if sys.platform != "darwin":
        return
    try:
        appkit = ctypes.cdll.LoadLibrary(_APPKIT)
        cf = ctypes.cdll.LoadLibrary(_COREFOUNDATION)
        objc, send, send_ptr, send_long, send_set = _objc()
        sel = objc.sel_registerName
        if not ctypes.c_void_p.in_dll(appkit, "NSApp").value:
            cf.CFStringCreateWithCString.restype = ctypes.c_void_p
            cf.CFStringCreateWithCString.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                                     ctypes.c_uint32]
            bundle = send(objc.objc_getClass(b"NSBundle"), sel(b"mainBundle"))
            info = send(bundle, sel(b"infoDictionary")) if bundle else None
            # The bundle's dictionary is a mutable one; a selector it does not
            # answer would abort the process, so ask first.
            if info and send_ptr(info, sel(b"respondsToSelector:"), sel(b"setObject:forKey:")):
                utf8 = 0x08000100
                send_set(info, sel(b"setObject:forKey:"),
                         cf.CFStringCreateWithCString(None, b"1", utf8),
                         cf.CFStringCreateWithCString(None, b"LSBackgroundOnly", utf8))
        app = send(objc.objc_getClass(b"NSApplication"), sel(b"sharedApplication"))
        if app:
            send_long(app, sel(b"setActivationPolicy:"), _POLICY_PROHIBITED)
    except (OSError, AttributeError, ValueError) as exc:
        logger.debug("[render] could not hide the render process from the Dock: %s", exc)


class RenderThreadError(RuntimeError):
    """A Cocoa session's VTK call came from a thread other than the main one
    and was refused before reaching AppKit."""


class CallerThreadExecutor(Executor):
    """Runs each call at once, on the thread that submits it.

    For a Cocoa window, which must be made and drawn on the main thread: in a
    render child the session's requests arrive on the main thread, so its VTK
    calls stay there. A call from any other thread fails with an error instead
    of reaching AppKit, which would abort the whole process."""

    def __init__(self) -> None:
        self._shutdown = False

    def submit(self, fn, /, *args, **kwargs) -> Future:
        future: Future = Future()
        if self._shutdown:
            raise RuntimeError("cannot schedule new futures after shutdown")
        if threading.current_thread() is not threading.main_thread():
            future.set_exception(RenderThreadError(
                "A macOS render window can only be used from the main thread "
                "(render sessions run in a process of their own; "
                "unset VISIVO_RENDER_IN_PROCESS)."))
            return future
        try:
            future.set_result(fn(*args, **kwargs))
        except BaseException as exc:   # noqa: BLE001 - delivered through the future
            future.set_exception(exc)
        return future

    def shutdown(self, wait: bool = True, *, cancel_futures: bool = False) -> None:
        self._shutdown = True


def render_executor(kind: str | None, name: str) -> Executor:
    """Where a session's VTK calls run: a thread of their own (EGL contexts
    are bound to the thread that made them), or the caller's thread for
    Cocoa."""
    if kind == "cocoa":
        return CallerThreadExecutor()
    return ThreadPoolExecutor(max_workers=1, thread_name_prefix=name)
