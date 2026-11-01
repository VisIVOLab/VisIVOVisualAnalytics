"""The starting values of a remote view come from the backend (issue #82):
create_session and /levels return the threshold the session renders with and
the cube's data range, so the client shows them instead of its own."""
from __future__ import annotations

import pytest

from app.render_session import RenderSession


class _Session:
    session_id = "c0ffee00"
    owner_token = None

    def __init__(self, levels):
        self._levels = levels
        self.view = type("View", (), {"width": 4, "height": 3})()

    async def render_frame(self) -> bytes:
        return b"jpeg"

    def levels(self):
        return self._levels


class _Manager:
    def __init__(self, sess):
        self.sess = sess

    async def start(self):
        pass

    async def create_session(self, dataset_path, owner_token=None):
        self.sess.owner_token = owner_token
        return self.sess

    async def get_session(self, session_id):
        return self.sess if session_id == self.sess.session_id else None


@pytest.fixture
def remote(monkeypatch):
    from app.routers import render

    monkeypatch.setattr(render, "server_side_rendering_available", lambda: True)

    async def ready(entry):
        return None
    monkeypatch.setattr(render, "_pyramid_preparing", ready)

    def use(levels):
        manager = _Manager(_Session(levels))
        monkeypatch.setattr(render, "get_render_manager", lambda: manager)
        return manager.sess
    return use


@pytest.mark.asyncio
async def test_create_session_returns_the_backends_threshold_and_range(client, opened_cube, remote):
    remote({"threshold": 2.5, "data_min": -1.0, "data_max": 40.0, "pending": False})
    resp = await client.post("/v1/render/create_session",
                             json={"dataset_id": opened_cube["dataset_id"]},
                             headers=opened_cube["headers"])
    assert resp.status_code == 200
    body = resp.json()
    assert (body["threshold"], body["data_min"], body["data_max"]) == (2.5, -1.0, 40.0)
    assert body["levels_pending"] is False


@pytest.mark.asyncio
async def test_a_cube_still_loading_is_asked_again_until_complete(client, opened_cube, remote):
    sess = remote({"threshold": None, "data_min": None, "data_max": None, "pending": True})
    resp = await client.post("/v1/render/create_session",
                             json={"dataset_id": opened_cube["dataset_id"]},
                             headers=opened_cube["headers"])
    body = resp.json()
    assert body["threshold"] is None and body["levels_pending"] is True

    sess._levels = {"threshold": 3.0, "data_min": 0.0, "data_max": 9.0, "pending": False}
    resp = await client.get(f"/v1/render/{sess.session_id}/levels",
                            headers=opened_cube["headers"])
    assert resp.status_code == 200
    assert resp.json() == {"threshold": 3.0, "data_min": 0.0, "data_max": 9.0, "pending": False}

    resp = await client.get("/v1/render/unknown/levels", headers=opened_cube["headers"])
    assert resp.status_code == 404


def test_levels_are_withheld_until_a_streaming_cube_is_shown():
    sess = RenderSession("feedc0de", "/data/cube.fits")
    try:
        sess._vmin, sess._vmax = -2.0, 30.0
        sess.view.threshold = 1.25
        assert sess.levels() == {"threshold": 1.25, "data_min": -2.0, "data_max": 30.0,
                                 "pending": False}

        # The chunked reader: placeholders until the volume is revealed, then
        # the values, still pending until the load completes.
        sess._cube_reader = object()
        sess._cube_complete = False
        sess._volume_revealed = False
        sess.view.threshold = 0.0
        assert sess.levels() == {"threshold": None, "data_min": None, "data_max": None,
                                 "pending": True}
        sess._volume_revealed = True
        sess.view.threshold = 1.5
        assert sess.levels() == {"threshold": 1.5, "data_min": -2.0, "data_max": 30.0,
                                 "pending": True}
        sess._cube_complete = True
        assert sess.levels()["pending"] is False
    finally:
        sess._cube_reader = None
        sess._executor.shutdown(wait=False)


class _Reader:
    def __init__(self):
        self.events = []

    def poll_progress(self):
        out, self.events = self.events, []
        return out

    def close(self):
        pass


def _streaming_session(cube):
    """A session on the chunked reader path, with the VTK steps stubbed out."""
    sess = RenderSession("feedc0de", "/data/cube.fits")
    sess._cube_reader = _Reader()
    sess._cube_complete = False
    sess._volume_revealed = False
    sess._cube_np = cube
    sess._chunks_total = 4
    sess._vmin = sess._vmax = 0.0
    sess.view.threshold = 0.0
    sess._image_data = type("Image", (), {"Modified": lambda self: None})()
    for name in ("_rebuild_volume_tfs_on_thread", "_rebuild_slice_lut_on_thread",
                 "_apply_cube_render_mode_on_thread", "request_idr", "mark_dirty"):
        setattr(sess, name, lambda *a: None)
    return sess


def _chunk(lo, hi):
    return {"event": "chunk", "vmin": lo, "vmax": hi}


def test_a_streaming_cube_gets_its_threshold_at_reveal_and_again_when_complete():
    from app.noise_threshold import default_threshold
    import numpy as np
    cube = np.zeros((40, 40, 40), dtype=np.float32)
    cube[:10] = np.random.default_rng(0).normal(size=(10, 40, 40))   # the first quarter
    sess = _streaming_session(cube)
    try:
        sess._cube_reader.events = [_chunk(float(cube.min()), float(cube.max()))]
        sess._poll_cube_chunks()
        first = sess.levels()
        assert first["pending"] is True
        # From the noise of the part loaded, for the size of the whole cube.
        assert first["threshold"] == pytest.approx(default_threshold(cube[:10], cube.size),
                                                   rel=0.05)

        cube[10:] = 10 * np.random.default_rng(1).normal(size=(30, 40, 40))
        sess._cube_reader.events = [{"event": "complete", "vmin": float(cube.min()),
                                     "vmax": float(cube.max())}]
        sess._poll_cube_chunks()
        final = sess.levels()
        assert final["pending"] is False
        assert final["threshold"] > 5 * first["threshold"]          # over the whole cube now
        assert final["data_max"] == pytest.approx(float(cube.max()))
    finally:
        sess._cube_reader = None
        sess._executor.shutdown(wait=False)


@pytest.mark.asyncio
async def test_a_threshold_the_client_sent_is_kept_even_zero():
    import numpy as np
    cube = np.random.default_rng(0).normal(size=(40, 40, 40)).astype(np.float32)
    sess = _streaming_session(cube)
    try:
        sess._threshold_from_client = False
        await sess.apply_command({"type": "set_threshold", "value": 0.0})
        sess._cube_reader.events = [_chunk(float(cube.min()), float(cube.max())),
                                    {"event": "complete", "vmin": float(cube.min()),
                                     "vmax": float(cube.max())}]
        sess._poll_cube_chunks()
        assert sess.levels()["threshold"] == 0.0
        assert sess.levels()["pending"] is False
    finally:
        sess._cube_reader = None
        sess._executor.shutdown(wait=False)


def test_a_reader_that_fails_ends_the_wait():
    import numpy as np
    sess = _streaming_session(np.zeros((8, 8, 8), dtype=np.float32))
    try:
        sess._cube_reader.events = [{"event": "error", "message": "disk gone"}]
        sess._poll_cube_chunks()
        assert sess.levels()["pending"] is False
    finally:
        sess._cube_reader = None
        sess._executor.shutdown(wait=False)
