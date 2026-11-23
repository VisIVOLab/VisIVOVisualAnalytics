"""The registry of WebRTC peers by render session (issue #411).

A reconnect to the same session makes a new peer while the old one may still
be closing (its ICE consent fails some seconds later). The old one's close
must not unregister the new one, and the new offer closes the old peer.
"""
from __future__ import annotations

import pytest

pytest.importorskip("aiortc")
pytest.importorskip("av")

import app.webrtc_session as ws  # noqa: E402


class _Render:
    def __init__(self, session_id: str) -> None:
        self.session_id = session_id


@pytest.fixture(autouse=True)
def _clean_registry(monkeypatch):
    monkeypatch.setattr(ws, "_active_sessions", {})


async def test_an_old_peer_closing_late_keeps_the_new_one_registered():
    render = _Render("s1")
    old, new = ws.WebRTCSession(render), ws.WebRTCSession(render)
    ws._active_sessions["s1"] = new   # the reconnect's peer took the entry
    await old.close()                 # the old peer's consent failed later
    assert ws.get_active_webrtc_session("s1") is new
    await new.close()
    assert ws.get_active_webrtc_session("s1") is None


async def test_a_new_offer_closes_the_previous_peer(monkeypatch):
    render = _Render("s1")
    old = ws.WebRTCSession(render)
    ws._active_sessions["s1"] = old
    closed: list[str] = []

    async def close_old() -> None:
        closed.append("old")
        await ws.WebRTCSession.close(old)

    monkeypatch.setattr(old, "close", close_old)
    monkeypatch.setattr(ws, "webrtc_available", lambda: None)
    new = ws.WebRTCSession(render)
    # The negotiation itself is not under test: it stops right after the peer
    # is registered, at the remote description.
    class _Stop(Exception):
        pass

    async def stop(*_a, **_k):
        raise _Stop

    monkeypatch.setattr(ws.RTCPeerConnection, "setRemoteDescription", stop)
    monkeypatch.setattr(ws, "VtkFrameTrack", lambda *_a, **_k: _FakeTrack())
    monkeypatch.setattr(ws, "_force_h264_on", lambda *_a, **_k: None)
    with pytest.raises(_Stop):
        await new.create_answer("v=0", "offer")
    assert closed == ["old"]
    assert ws.get_active_webrtc_session("s1") is new
    await new.close()


class _FakeTrack:
    kind = "video"
