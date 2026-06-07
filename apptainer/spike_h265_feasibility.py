"""H.265 / NVENC feasibility spike — does the EGL SIF have what we need?

Run this inside the same Apptainer container that already hosts the SSR
backend; it asks four yes/no questions in series so we know exactly
which of the three components needs work before we commit to a full
WebRTC pipeline:

  1. Can we import ``av`` (PyAV) at all?
  2. Does ``av`` see ``hevc_nvenc`` (or ``h264_nvenc``) as an output
     codec at runtime? (PyAV only exposes the codecs FFmpeg was built
     with — even if libnvidia-encode.so is bind-mounted.)
  3. Can we open the NVENC encoder against a small test image and
     pull a valid HEVC NAL out the other side? (Catches "codec listed
     but driver doesn't speak HEVC on this GPU" and "wrong CUDA
     context" failures that don't surface during import.)
  4. Can we import ``aiortc`` for the WebRTC transport? (Optional —
     a full integration also needs ``cryptography`` and ``av`` on the
     server, plus ICE/SDP plumbing.)

The spike prints a one-line verdict per check so the output is easy
to grep from a SLURM log. Exit code is 0 only if all four pass.

Usage::

    apptainer exec --nv backend-egl.sif python spike_h265_feasibility.py

"""
from __future__ import annotations

import os
import sys
import time


def _check_pyav() -> object | None:
    try:
        import av  # type: ignore[import-not-found]
        print(f"[1/4] PyAV import: OK (version {av.__version__})")
        return av
    except Exception as e:
        print(f"[1/4] PyAV import: FAIL ({type(e).__name__}: {e})")
        return None


def _check_nvenc_codec(av) -> str | None:
    """Look for NVENC-backed HEVC/H.264 output codecs in PyAV's registry."""
    try:
        codecs = av.codec.codecs_available  # set[str] in modern PyAV
    except Exception as e:
        print(f"[2/4] NVENC codec probe: FAIL ({e})")
        return None
    candidates = ["hevc_nvenc", "h264_nvenc"]
    for c in candidates:
        if c in codecs:
            print(f"[2/4] NVENC codec available: OK ({c})")
            return c
    # Also try the encoder-list API, which is more reliable on some PyAV builds.
    try:
        from av.codec.codec import Codec  # type: ignore
        for c in candidates:
            try:
                Codec(c, "w")  # opens encoder side
                print(f"[2/4] NVENC codec available: OK ({c}, via Codec())")
                return c
            except Exception:
                continue
    except Exception:
        pass
    print(
        "[2/4] NVENC codec available: FAIL "
        "(neither hevc_nvenc nor h264_nvenc in PyAV registry; "
        "FFmpeg was likely built without --enable-nvenc)"
    )
    return None


def _encode_one_frame(av, codec_name: str) -> bool:
    """Open the encoder against a tiny RGB test frame, return True if a
    non-empty NAL comes out.

    Failure here usually means the bind-mounted libnvidia-encode.so does
    not match the driver version on the host, or the container can't
    see the GPU (``apptainer --nv`` not passed).
    """
    try:
        import numpy as np
        from av.video.frame import VideoFrame  # type: ignore
    except Exception as e:
        print(f"[3/4] NVENC encode test: FAIL (numpy/av import: {e})")
        return False
    width, height = 256, 256
    # Build an RGB24 ramp so the codec has actual entropy to chew on.
    img = np.zeros((height, width, 3), dtype=np.uint8)
    img[..., 0] = np.linspace(0, 255, width, dtype=np.uint8)[None, :]
    img[..., 1] = np.linspace(0, 255, height, dtype=np.uint8)[:, None]
    try:
        # ``CodecContext.create`` is the lower-level API that survives
        # without a containing output stream — ideal for a smoke test.
        from av.codec.context import CodecContext  # type: ignore
        ctx = CodecContext.create(codec_name, "w")
        ctx.width = width
        ctx.height = height
        ctx.pix_fmt = "yuv420p"          # NVENC's native input space
        ctx.bit_rate = 4_000_000
        ctx.time_base = (1, 30)
        # Reasonable low-latency defaults — matches what aiortc would
        # request for a real-time WebRTC session.
        try:
            ctx.options = {"preset": "p1", "tune": "ll"}    # SDK ≥ 11
        except Exception:
            pass
        ctx.open(strict=False)
        frame = VideoFrame.from_ndarray(img, format="rgb24")
        frame = frame.reformat(format="yuv420p")
        frame.pts = 0
        t0 = time.perf_counter()
        packets = list(ctx.encode(frame)) + list(ctx.encode(None))
        t1 = time.perf_counter()
    except Exception as e:
        print(f"[3/4] NVENC encode test: FAIL ({type(e).__name__}: {e})")
        return False
    total_bytes = sum(len(bytes(p)) for p in packets)
    if total_bytes == 0:
        print("[3/4] NVENC encode test: FAIL (no output bytes)")
        return False
    print(
        f"[3/4] NVENC encode test: OK "
        f"({len(packets)} packets, {total_bytes} bytes, "
        f"{(t1 - t0) * 1000:.1f} ms wall)"
    )
    return True


def _check_aiortc() -> bool:
    try:
        import aiortc  # type: ignore
        print(f"[4/4] aiortc import: OK (version {aiortc.__version__})")
        return True
    except Exception as e:
        print(
            f"[4/4] aiortc import: FAIL ({type(e).__name__}: {e}) — "
            "WebRTC signalling will need pip install."
        )
        return False


def main() -> int:
    print("=" * 64)
    print("H.265 / NVENC feasibility spike — VisIVO SSR Phase 3 prep")
    print("=" * 64)
    # Surface the GPU/driver context up front so failures past this
    # point can be correlated.
    nvidia = os.environ.get("CUDA_VISIBLE_DEVICES", "<unset>")
    print(f"CUDA_VISIBLE_DEVICES = {nvidia}")
    print(f"sys.executable       = {sys.executable}")
    print()

    av = _check_pyav()
    if av is None:
        return 1
    codec = _check_nvenc_codec(av)
    if codec is None:
        return 2
    if not _encode_one_frame(av, codec):
        return 3
    aiortc_ok = _check_aiortc()
    if not aiortc_ok:
        # aiortc is the only optional one — the encoder is the
        # high-risk piece. Return 4 (advisory) instead of failing
        # the whole spike.
        print()
        print("VERDICT: encoder pipeline OK, WebRTC signalling needs pip.")
        return 4

    print()
    print("VERDICT: full H.265 + WebRTC pipeline is feasible on this "
          "container — proceed with Phase 3 plumbing.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
