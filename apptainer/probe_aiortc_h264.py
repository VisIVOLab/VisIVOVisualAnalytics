"""Quick introspection: where does aiortc 1.9 build its H264 encoder?

Run inside the SIF; this dumps the full source of the ``aiortc.codecs.h264``
module + the public method list of ``H264Encoder``. From the output we
can write a correct monkey-patch (or subclass) that swaps the codec
name from ``libx264`` to ``h264_nvenc`` in whatever method actually
calls ``CodecContext.create()``.
"""
from __future__ import annotations

import inspect

from aiortc.codecs import h264

print("=" * 64)
print(f"aiortc.codecs.h264 located at: {inspect.getsourcefile(h264)}")
print("=" * 64)
print()
print("H264Encoder public attributes:")
for name in sorted(dir(h264.H264Encoder)):
    if name.startswith("_") and not name.startswith("__"):
        print(f"  {name}")
    elif not name.startswith("__"):
        print(f"  {name}  (public)")
print()
print("─" * 64)
print("Module source (libx264 / CodecContext / encoder mentions):")
print("─" * 64)
src = inspect.getsource(h264)
# Print only the interesting lines so the output isn't enormous.
for lineno, line in enumerate(src.splitlines(), start=1):
    if any(k in line for k in (
        "libx264", "h264_nvenc", "openh264", "CodecContext", "create_encoder",
        "def encode", "def _encode", "self.codec", "create(",
    )):
        print(f"  {lineno:4d} | {line}")

print()
print("─" * 64)
print("FULL source of H264Encoder class:")
print("─" * 64)
print(inspect.getsource(h264.H264Encoder))
