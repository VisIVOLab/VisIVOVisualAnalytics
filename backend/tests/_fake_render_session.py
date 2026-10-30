"""A GPU-free stand-in for RenderSession, loaded by the render child process
in tests/test_render_process.py (spawned children import it by name)."""
from __future__ import annotations

import os
import time

from app.render_session._common import SessionStats


class FakeRenderSession:
    def __init__(self, session_id: str, dataset_path: str) -> None:
        self.session_id = session_id
        self.dataset_path = dataset_path
        self.stats = SessionStats()
        self.view = type("View", (), {"width": 0, "height": 0})()
        self.applied: list[str] = []
        self.version = 0
        self.idr = False

    async def initialize(self) -> None:
        self.view.width, self.view.height = 640, 480

    async def apply_command(self, command: dict) -> None:
        kind = command["type"]
        if kind == "crash":
            os._exit(3)                     # what a segfault in the driver looks like
        if kind == "fail":
            raise ValueError("bad command")
        if kind == "busy":
            time.sleep(1.0)                 # a long render, from the pipe's point of view
        if kind == "lut":
            self.idr = True

        self.applied.append(kind)
        self.version += 1

    async def render_frame(self) -> bytes:
        self.stats.frames_rendered += 1
        if self.applied and self.applied[-1] == "break_next_frame":
            self.applied.append("broken")
            raise RuntimeError("readback failed")
        return ",".join(self.applied).encode()

    async def render_frame_if_changed(self, since: int):
        if since == self.version:
            return None, since
        return await self.render_frame(), self.version

    async def render_rgb_frame(self):
        return [[1, 2], [3, 4]], 1.5

    def consume_idr_flag(self) -> bool:
        flag, self.idr = self.idr, False
        return flag

    async def close(self) -> None:
        pass
