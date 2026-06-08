"""WebRTC spike — Day 4: VTK frame source on V100 EGL.

Replaces the synthetic colour-shift source with a real VTK scene
rendered offscreen on the V100 EGL context:

  * One sphere + outline at 30 FPS, slowly rotating
  * vtkRenderer → vtkEGLRenderWindow → vtkWindowToImageFilter
  * RGBA → drop alpha → numpy RGB → av.VideoFrame → NVENC → RTP

Threading: VTK objects are thread-bound to the EGL context that
created them. We do the render INSIDE the asyncio loop's
``run_in_executor`` so the encoder + transport stay async while the
render is synchronous on its own thread. For the spike a single
``ThreadPoolExecutor(max_workers=1)`` is enough; the real
``RenderSession`` already runs VTK on its own pinned thread, so when
we wire this into the backend we keep that pattern intact.

Run inside the SIF:

  apptainer exec --nv backend-egl-h265.sif \
      /opt/conda/bin/python apptainer/spike_h265_webrtc_vtk.py
"""
from __future__ import annotations

# Redirect VTK / fontconfig caches OFF the BeeGFS/NFS mount BEFORE any
# import that might touch them. On Power9 the first vtkRenderWindow
# Render() probes fonts via fontconfig; fontconfig writes its cache to
# ${XDG_CACHE_HOME}, default ``~/.cache``. With ``~`` on NFS the write
# acquires an NFS lock that blocks forever (lockd timeout) → the spike
# appears to hang at VtkScene.initialize(). The SSR runner script
# (run_spike_egl.sh) already sets these for the production backend; we
# replicate them here so the standalone spike is self-sufficient.
import os
os.environ.setdefault("XDG_CACHE_HOME", "/tmp/visivo-cache")
os.environ.setdefault("FONTCONFIG_PATH", "/etc/fonts")
os.environ.setdefault(
    "__EGL_VENDOR_LIBRARY_FILENAMES",
    "/usr/share/glvnd/egl_vendor.d/10_nvidia.json",
)
os.environ.setdefault("__GLX_VENDOR_LIBRARY_NAME", "nvidia")

import asyncio
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from fractions import Fraction

import numpy as np
import uvicorn
from aiohttp import ClientSession
from aiortc import (
    MediaStreamTrack,
    RTCPeerConnection,
    RTCSessionDescription,
)
from av import VideoFrame
from fastapi import FastAPI
from pydantic import BaseModel

PORT = 8765
encode_times_ms_singleton: list[list[float]] = [[]]
render_times_ms_singleton: list[list[float]] = [[]]


# ── NVENC patch (carried over from Day 2/3) ──────────────────────────────────


def _install_nvenc_encoder() -> str:
    from aiortc.codecs import h264 as h264_mod
    import av

    encode_times_ms: list[float] = []
    encode_times_ms_singleton[0] = encode_times_ms

    original_create = h264_mod.create_encoder_context

    def create_encoder_context_nvenc(codec_name, width, height, bitrate):
        try:
            codec = av.CodecContext.create("h264_nvenc", "w")
            codec.width = width
            codec.height = height
            codec.pix_fmt = "yuv420p"
            codec.framerate = Fraction(30, 1)
            codec.time_base = Fraction(1, 90000)
            codec.bit_rate = bitrate
            codec.gop_size = 30
            try:
                codec.options = {
                    "preset": "p1", "tune": "ll", "rc": "cbr", "delay": "0",
                }
            except Exception:
                pass
            codec.open(strict=False)
            return codec, False
        except Exception as e:
            print(f"[nvenc-patch] WARN: NVENC build failed ({e})")
            return original_create(codec_name, width, height, bitrate)

    h264_mod.create_encoder_context = create_encoder_context_nvenc

    original_encode = h264_mod.H264Encoder.encode

    def _encode_timed(self, frame, force_keyframe=False):
        t0 = time.perf_counter()
        result = original_encode(self, frame, force_keyframe)
        encode_times_ms.append((time.perf_counter() - t0) * 1000.0)
        return result

    h264_mod.H264Encoder.encode = _encode_timed
    return "h264_nvenc"


def _force_h264_only(pc) -> None:
    from aiortc.rtcrtpsender import RTCRtpSender
    caps = RTCRtpSender.getCapabilities("video")
    h264_codecs = [c for c in caps.codecs
                   if c.mimeType.lower() == "video/h264"]
    if not h264_codecs:
        return
    for tr in pc.getTransceivers():
        if tr.kind == "video":
            tr.setCodecPreferences(h264_codecs)


# ── VTK offscreen scene ──────────────────────────────────────────────────────
#
# Built once on a dedicated thread (EGL context = thread-bound), then
# render_to_rgb() is called by the asyncio loop via run_in_executor.
# The scene is a rotating sphere inside a wireframe cube — visually
# obvious that VTK is doing something, and easy to spot if the
# rendering broke (the test image stays static or goes blank).


class VtkScene:
    """One offscreen VTK render window + the actors. Render-thread local.

    The thread that calls ``initialize()`` becomes the EGL-context
    owner; every render must happen from the same thread. The spike
    uses a single-worker executor for this; the real RenderSession
    already does the same pattern.
    """

    def __init__(self, width: int = 640, height: int = 480):
        self._w, self._h = width, height
        self._render_window = None
        self._renderer = None
        self._sphere_actor = None
        self._w2if = None
        self._counter = 0
        self._owner_thread: int | None = None

    def initialize(self) -> None:
        print("[vtk-init] importing vtk…", flush=True)
        import vtk
        from vtkmodules.vtkRenderingOpenGL2 import vtkEGLRenderWindow
        self._owner_thread = threading.get_ident()
        print(f"[vtk-init] vtk {vtk.VTK_VERSION} imported, "
              f"thread={self._owner_thread}", flush=True)

        # Sphere actor — rotates over time.
        sphere = vtk.vtkSphereSource()
        sphere.SetRadius(1.0)
        sphere.SetThetaResolution(48)
        sphere.SetPhiResolution(48)
        sphere_mapper = vtk.vtkPolyDataMapper()
        sphere_mapper.SetInputConnection(sphere.GetOutputPort())
        sphere_actor = vtk.vtkActor()
        sphere_actor.SetMapper(sphere_mapper)
        sphere_actor.GetProperty().SetColor(1.0, 0.6, 0.1)
        sphere_actor.GetProperty().SetInterpolationToPhong()
        self._sphere_actor = sphere_actor

        # Outline cube around the sphere — proves the camera moves.
        cube = vtk.vtkCubeSource()
        cube.SetBounds(-1.5, 1.5, -1.5, 1.5, -1.5, 1.5)
        outline_mapper = vtk.vtkPolyDataMapper()
        outline_mapper.SetInputConnection(cube.GetOutputPort())
        outline_actor = vtk.vtkActor()
        outline_actor.SetMapper(outline_mapper)
        outline_actor.GetProperty().SetRepresentationToWireframe()
        outline_actor.GetProperty().SetColor(0.8, 0.8, 0.8)

        # Renderer + EGL window.
        ren = vtk.vtkRenderer()
        ren.SetBackground(0.05, 0.05, 0.15)
        ren.AddActor(sphere_actor)
        ren.AddActor(outline_actor)
        print("[vtk-init] creating vtkEGLRenderWindow…", flush=True)
        win = vtkEGLRenderWindow()
        win.SetOffScreenRendering(1)
        win.SetSize(self._w, self._h)
        if hasattr(win, "SetDeviceIndex"):
            win.SetDeviceIndex(0)
        win.AddRenderer(ren)
        ren.ResetCamera()
        print("[vtk-init] warm-up Render() — this is where NFS fontconfig "
              "would hang…", flush=True)
        win.Render()      # warm-up so first real render isn't slow
        print("[vtk-init] warm-up Render() OK", flush=True)

        # vtkWindowToImageFilter to extract pixels.
        w2if = vtk.vtkWindowToImageFilter()
        w2if.SetInput(win)
        w2if.ReadFrontBufferOff()
        w2if.SetInputBufferTypeToRGBA()
        self._render_window = win
        self._renderer = ren
        self._w2if = w2if

    def render_to_rgb(self) -> np.ndarray:
        """Render one frame and return an (H, W, 3) uint8 RGB array.

        Must run on the same thread as ``initialize()``.
        """
        assert self._owner_thread == threading.get_ident(), \
            "render_to_rgb called from wrong thread (EGL context is thread-bound)"

        import vtk
        from vtk.util.numpy_support import vtk_to_numpy

        # Rotate camera around the volume so motion is visible end-to-end.
        cam = self._renderer.GetActiveCamera()
        cam.Azimuth(2.0)   # 2°/frame at 30 FPS → 60°/s

        self._render_window.Render()
        self._w2if.Modified()    # invalidate cached output
        self._w2if.Update()

        out = self._w2if.GetOutput()
        scalars = out.GetPointData().GetScalars()
        arr = vtk_to_numpy(scalars).reshape((self._h, self._w, 4))[::-1]
        # Drop alpha — NVENC takes RGB or YUV.
        rgb = np.ascontiguousarray(arr[:, :, :3])
        self._counter += 1
        return rgb


# ── VTK-backed MediaStreamTrack ──────────────────────────────────────────────


class VtkVideoTrack(MediaStreamTrack):
    """Pulls frames from a VtkScene running on its own executor."""
    kind = "video"

    def __init__(self, scene: VtkScene, executor: ThreadPoolExecutor,
                 fps: float = 30.0):
        super().__init__()
        self._scene = scene
        self._executor = executor
        self._fps = fps
        self._counter = 0
        self._start_wall = time.perf_counter()

    async def recv(self) -> VideoFrame:
        # Pace to target FPS so the encoder isn't blasted from a hot loop.
        target_t = self._start_wall + self._counter / self._fps
        delta = target_t - time.perf_counter()
        if delta > 0:
            await asyncio.sleep(delta)

        loop = asyncio.get_event_loop()
        t0 = time.perf_counter()
        rgb = await loop.run_in_executor(self._executor, self._scene.render_to_rgb)
        render_times_ms_singleton[0].append((time.perf_counter() - t0) * 1000.0)

        frame = VideoFrame.from_ndarray(rgb, format="rgb24")
        frame.pts = self._counter
        frame.time_base = Fraction(1, int(self._fps))
        self._counter += 1
        return frame


# ── FastAPI signalling (same shape as Day 3) ─────────────────────────────────


class SdpExchange(BaseModel):
    sdp: str
    type: str


def build_server_app(scene: VtkScene, executor: ThreadPoolExecutor) -> FastAPI:
    app = FastAPI()
    pcs: list[RTCPeerConnection] = []

    @app.post("/spike/sdp/offer", response_model=SdpExchange)
    async def handle_offer(offer: SdpExchange) -> SdpExchange:
        pc = RTCPeerConnection()
        pcs.append(pc)
        pc.addTrack(VtkVideoTrack(scene, executor))
        _force_h264_only(pc)
        await pc.setRemoteDescription(
            RTCSessionDescription(sdp=offer.sdp, type=offer.type)
        )
        answer = await pc.createAnswer()
        await pc.setLocalDescription(answer)
        return SdpExchange(sdp=pc.localDescription.sdp,
                            type=pc.localDescription.type)

    app.state.pcs = pcs
    return app


def run_server_in_thread(scene, executor) -> threading.Thread:
    app = build_server_app(scene, executor)
    config = uvicorn.Config(app, host="127.0.0.1", port=PORT, log_level="warning")
    server = uvicorn.Server(config)
    t = threading.Thread(
        target=lambda: asyncio.run(server.serve()),
        daemon=True, name="spike-uvicorn",
    )
    t.start()
    return t


# ── Client ───────────────────────────────────────────────────────────────────


async def run_client(duration_s: float = 5.0) -> int:
    client_pc = RTCPeerConnection()
    arrivals: list[float] = []
    consume_done = asyncio.Event()
    target_count = int(duration_s * 30.0)

    @client_pc.on("track")
    def on_track(received_track: MediaStreamTrack) -> None:
        print(f"[client] track received: kind={received_track.kind}")

        async def consume() -> None:
            try:
                while not consume_done.is_set():
                    frame = await received_track.recv()
                    arrivals.append(time.perf_counter())
                    if len(arrivals) % 30 == 0:
                        print(f"[client] {len(arrivals)} frames "
                              f"({frame.width}x{frame.height})")
                    if len(arrivals) >= target_count:
                        consume_done.set()
            except Exception as e:
                print(f"[client] consume() exception: {type(e).__name__}: {e}")
                consume_done.set()

        asyncio.create_task(consume())

    client_pc.addTransceiver("video", direction="recvonly")
    offer = await client_pc.createOffer()
    await client_pc.setLocalDescription(offer)

    async with ClientSession() as session:
        async with session.post(
            f"http://127.0.0.1:{PORT}/spike/sdp/offer",
            json={"sdp": client_pc.localDescription.sdp,
                  "type": client_pc.localDescription.type},
            timeout=5.0,
        ) as resp:
            data = await resp.json()
    await client_pc.setRemoteDescription(
        RTCSessionDescription(sdp=data["sdp"], type=data["type"])
    )

    try:
        await asyncio.wait_for(consume_done.wait(), timeout=duration_s + 5.0)
    except asyncio.TimeoutError:
        print(f"[spike] timeout — only {len(arrivals)} frames received")

    await client_pc.close()

    # ── Stats ───────────────────────────────────────────────────────
    print()
    print("─" * 64)
    print(f"Frames received    : {len(arrivals)} (target {target_count})")
    if len(arrivals) >= 2:
        intervals_ms = np.diff(arrivals) * 1000.0
        print(f"Inter-frame mean   : {intervals_ms.mean():.2f} ms")
        print(f"Inter-frame p99    : {np.percentile(intervals_ms, 99):.2f} ms")
        print(f"Effective FPS      : {1000.0 / intervals_ms.mean():.2f}")

    render_times_ms = render_times_ms_singleton[0]
    if render_times_ms:
        rt = np.asarray(render_times_ms)
        print()
        print(f"VTK render samples : {len(rt)}")
        print(f"VTK render mean    : {rt.mean():.2f} ms")
        print(f"VTK render p99     : {np.percentile(rt, 99):.2f} ms")

    encode_times_ms = encode_times_ms_singleton[0]
    if encode_times_ms:
        et = np.asarray(encode_times_ms)
        print()
        print(f"NVENC encode samples: {len(et)}")
        print(f"NVENC encode mean   : {et.mean():.2f} ms")
        print(f"NVENC encode p99    : {np.percentile(et, 99):.2f} ms")

    # End-to-end budget: render + encode should fit comfortably in
    # one frame period (33 ms at 30 FPS).
    if render_times_ms and encode_times_ms:
        per_frame_total = (np.asarray(render_times_ms).mean()
                            + np.asarray(encode_times_ms).mean())
        print()
        print(f"Render+encode total: {per_frame_total:.2f} ms / frame "
              f"(budget 33.33 ms @ 30 FPS)")
        if per_frame_total < 30:
            print()
            print("VERDICT: VTK→NVENC pipeline well inside the 30 FPS budget. "
                  "Ready for Day 5 (cube data + camera control).")
            return 0
    print()
    print("VERDICT: pipeline degraded — see stats above.")
    return 1


# ── Main ─────────────────────────────────────────────────────────────────────


def _build_scene_on_thread(scene: VtkScene) -> None:
    """Run scene.initialize() on the executor's worker thread."""
    scene.initialize()


async def main() -> int:
    print("=" * 64)
    print("WebRTC spike — Day 4: VTK frame source on V100 EGL")
    print("=" * 64)
    codec = _install_nvenc_encoder()
    print(f"[setup] aiortc patched to use {codec!r}")

    # Single-worker executor that owns the EGL context for this run.
    executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="vtk")
    scene = VtkScene(width=640, height=480)
    # Build the scene on the executor thread so the EGL context is
    # bound there for the lifetime of the spike.
    loop = asyncio.get_event_loop()
    await loop.run_in_executor(executor, _build_scene_on_thread, scene)
    print("[setup] VTK scene initialized on EGL render thread")

    run_server_in_thread(scene, executor)
    print(f"[setup] uvicorn started on 127.0.0.1:{PORT}")
    await asyncio.sleep(0.5)

    return await run_client(duration_s=5.0)


if __name__ == "__main__":
    import sys
    sys.exit(asyncio.run(main()))
