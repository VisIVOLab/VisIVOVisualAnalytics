"""Selections shared between the headset and the desktop (issue #188): the
hub's routes, auth, bounds, expiry and the latest-wins rule."""
from __future__ import annotations

import asyncio
import gc
import json
import time
from pathlib import Path

import numpy as np
import pytest
from astropy.io import fits
from httpx import AsyncClient

from app import selection_hub as hub_mod


def _cube(path: Path) -> np.ndarray:
    """16 x 20 x 18 (z, y, x) of noise with a bright blob; VELO in m/s, a beam."""
    rng = np.random.default_rng(7)
    data = rng.normal(0.0, 1.0, (16, 20, 18)).astype(np.float32)
    zz, yy, xx = np.mgrid[0:16, 0:20, 0:18]
    data += (12.0 * np.exp(-(((xx - 9) ** 2 + (yy - 10) ** 2) / 10.0 + (zz - 8) ** 2 / 5.0))).astype(np.float32)
    h = fits.Header()
    h.update({"CTYPE1": "RA---SIN", "CTYPE2": "DEC--SIN", "CTYPE3": "VELO-HEL",
              "CRVAL1": 150.0, "CRVAL2": 20.0, "CRVAL3": 1.0e6, "CRPIX1": 9.0, "CRPIX2": 10.0,
              "CRPIX3": 1.0, "CDELT1": -1 / 3600, "CDELT2": 1 / 3600, "CDELT3": 8000.0,
              "CUNIT1": "deg", "CUNIT2": "deg", "CUNIT3": "m/s", "BUNIT": "Jy/beam",
              "BMAJ": 4 / 3600, "BMIN": 4 / 3600, "BPA": 0.0})
    fits.PrimaryHDU(data, header=h).writeto(path, overwrite=True)
    return data


@pytest.fixture(autouse=True)
def _drop_the_sessions_made():
    """Each test opens sessions of its own; they go with it, so the file
    stays under the per-token session cap (32) whatever ran before."""
    from app.sessions import REGISTRY
    before = set(REGISTRY._sessions)
    yield
    with REGISTRY._lock:
        made = [sid for sid in REGISTRY._sessions if sid not in before]
        dropped = [REGISTRY._sessions.pop(sid) for sid in made]
    for s in dropped:
        s.close()


async def _session(client: AsyncClient, auth_headers: dict) -> dict:
    r = await client.post("/v1/sessions/new", json={}, headers=auth_headers)
    assert r.status_code == 200, r.text
    return {**auth_headers, "X-Visivo-Session": r.json()["session_id"]}


async def _open(client: AsyncClient, headers: dict, path: Path) -> str:
    r = await client.post("/v1/datasets/open", json={"path": str(path)}, headers=headers)
    assert r.status_code == 200 and r.json()["valid"], r.text
    return r.json()["dataset_id"]


def _mask(data: np.ndarray, thr: float = 4.0):
    """The blob above thr as a mask over its bounding box: (mask zyx, origin xyz)."""
    sel = data > thr
    zs, ys, xs = (np.nonzero(sel.any(a))[0] for a in ((1, 2), (0, 2), (0, 1)))
    box = sel[zs[0]:zs[-1] + 1, ys[0]:ys[-1] + 1, xs[0]:xs[-1] + 1]
    return box, (int(xs[0]), int(ys[0]), int(zs[0]))


async def _publish(client, headers, ds, mask, origin, *, source="headset", client_id="page-1",
                   meta=None, kind="lasso", body=None):
    d, h, w = mask.shape
    return await client.post("/v1/share/selection", params={
        "dataset_id": ds, "kind": kind, "source": source, "client": client_id,
        "origin_x": origin[0], "origin_y": origin[1], "origin_z": origin[2],
        "width": w, "height": h, "depth": d, "meta": json.dumps(meta or {})},
        content=(mask.astype("<f4").tobytes() if kind == "lasso" else b"") if body is None else body,
        headers={**headers, "Content-Type": "application/octet-stream"})


@pytest.mark.asyncio
async def test_a_lasso_is_shared_with_its_statistics_and_record(client, auth_headers, tmp_path):
    data = _cube(tmp_path / "c.fits")
    hd = await _session(client, auth_headers)
    ds = await _open(client, hd, tmp_path / "c.fits")
    mask, origin = _mask(data)
    grow = {"seed_z": 8, "seed_y": 10, "seed_x": 9, "reach": 8, "lambda_v": 1}
    r = await _publish(client, hd, ds, mask, origin, meta={"grow": grow})
    assert r.status_code == 200, r.text
    pub = r.json()
    assert pub["count"] == int(mask.sum())

    info = (await client.get("/v1/share/selection", params={"dataset_id": ds}, headers=hd)).json()
    assert info["seq"] == pub["seq"] and info["kind"] == "lasso"
    assert info["source"] == "headset" and info["client"] == "page-1"
    assert info["origin"] == list(origin) and info["dims"] == [mask.shape[2], mask.shape[1], mask.shape[0]]
    assert info["full_shape"] == [18, 20, 16] and info["count"] == int(mask.sum())
    assert info["meta"] == {"grow": grow}
    # The backend instance: seqs restart with it, so clients reset on a change.
    assert info["instance"] == hub_mod.INSTANCE == pub["instance"] and len(info["instance"]) == 16
    # The statistics are mask_stats_bin's on the same mask.
    ref = await client.post("/v1/cube/mask_stats_bin", params={
        "dataset_id": ds, "origin_x": origin[0], "origin_y": origin[1], "origin_z": origin[2],
        "width": mask.shape[2], "height": mask.shape[1], "depth": mask.shape[0]},
        content=mask.astype("<f4").tobytes(), headers={**hd, "Content-Type": "application/octet-stream"})
    ref = ref.json()
    assert info["stats"]["valid"]
    for k in ("selected_voxels", "sum", "flux_jy_kms", "bbox", "channel_min", "channel_max"):
        assert info["stats"][k] == ref[k], k

    m = await client.get("/v1/share/selection/mask", params={"dataset_id": ds, "seq": info["seq"]}, headers=hd)
    assert m.status_code == 200
    got = np.frombuffer(m.content, dtype="<f4").reshape(mask.shape)
    np.testing.assert_array_equal(got, mask.astype(np.float32))
    assert m.headers["X-Visivo-Selected-Count"] == str(int(mask.sum()))
    assert m.headers["X-Visivo-Origin"] == ",".join(map(str, origin))
    assert m.headers["X-Visivo-Bbox"] == ",".join(map(str, ref["bbox"]))


@pytest.mark.asyncio
async def test_sharing_needs_the_token_and_stays_in_its_session(client, auth_headers, tmp_path):
    data = _cube(tmp_path / "c.fits")
    hd = await _session(client, auth_headers)
    ds = await _open(client, hd, tmp_path / "c.fits")
    mask, origin = _mask(data)
    no_token = {"X-Visivo-Session": hd["X-Visivo-Session"]}
    assert (await _publish(client, no_token, ds, mask, origin)).status_code == 401
    bad = {**hd, "X-Visivo-Token": "wrong"}
    assert (await client.get("/v1/share/selection", params={"dataset_id": ds}, headers=bad)).status_code == 401
    assert (await _publish(client, hd, ds, mask, origin)).status_code == 200
    # Another session, even on the same file, sees nothing of it.
    other = await _session(client, auth_headers)
    assert (await client.get("/v1/share/selection", params={"dataset_id": ds}, headers=other)).status_code == 404
    ds2 = await _open(client, other, tmp_path / "c.fits")
    assert (await client.get("/v1/share/selection", params={"dataset_id": ds2}, headers=other)).status_code == 204
    # The same session holding the cube under another dataset id (the headset
    # opened the desktop's path again) sees it.
    ds3 = await _open(client, hd, tmp_path / "c.fits")
    assert ds3 != ds
    r = await client.get("/v1/share/selection", params={"dataset_id": ds3}, headers=hd)
    assert r.status_code == 200 and r.json()["dataset_id"] == ds


@pytest.mark.asyncio
async def test_the_latest_selection_wins(client, auth_headers, tmp_path):
    data = _cube(tmp_path / "c.fits")
    hd = await _session(client, auth_headers)
    ds = await _open(client, hd, tmp_path / "c.fits")
    mask, origin = _mask(data)
    first = (await _publish(client, hd, ds, mask, origin, source="headset")).json()
    small, so = _mask(data, 8.0)
    second = (await _publish(client, hd, ds, small, so, source="desktop", client_id="desk")).json()
    assert second["seq"] > first["seq"]
    info = (await client.get("/v1/share/selection", params={"dataset_id": ds}, headers=hd)).json()
    assert info["seq"] == second["seq"] and info["source"] == "desktop" and info["count"] == int(small.sum())
    gone = await client.get("/v1/share/selection/mask", params={"dataset_id": ds, "seq": first["seq"]}, headers=hd)
    assert gone.status_code == 404
    # after = the latest: nothing newer.
    r = await client.get("/v1/share/selection", params={"dataset_id": ds, "after": second["seq"]}, headers=hd)
    assert r.status_code == 204
    # Cleared: nothing at all.
    assert (await client.delete("/v1/share/selection", params={"dataset_id": ds}, headers=hd)).json() == {"cleared": True}
    assert (await client.get("/v1/share/selection", params={"dataset_id": ds}, headers=hd)).status_code == 204


@pytest.mark.asyncio
async def test_a_long_poll_answers_as_soon_as_a_selection_is_published(client, auth_headers, tmp_path):
    data = _cube(tmp_path / "c.fits")
    hd = await _session(client, auth_headers)
    ds = await _open(client, hd, tmp_path / "c.fits")
    mask, origin = _mask(data)
    t0 = time.monotonic()
    r = await client.get("/v1/share/selection", params={"dataset_id": ds, "wait": 0.3}, headers=hd)
    assert r.status_code == 204 and time.monotonic() - t0 >= 0.25

    async def poll():
        t = time.monotonic()
        resp = await client.get("/v1/share/selection", params={"dataset_id": ds, "wait": 10}, headers=hd)
        return resp, time.monotonic() - t

    waiting = asyncio.create_task(poll())
    await asyncio.sleep(0.2)
    t_pub = time.monotonic()
    pub = (await _publish(client, hd, ds, mask, origin)).json()
    resp, waited = await waiting
    assert resp.status_code == 200 and resp.json()["seq"] == pub["seq"]
    assert waited < 5.0 and time.monotonic() - t_pub < 1.0


@pytest.mark.asyncio
async def test_bounds_of_the_hub(client, auth_headers, tmp_path, monkeypatch):
    data = _cube(tmp_path / "c.fits")
    hd = await _session(client, auth_headers)
    ds = await _open(client, hd, tmp_path / "c.fits")
    mask, origin = _mask(data)
    # A wrong body, an empty mask, a box outside the cube, a bad meta.
    assert (await _publish(client, hd, ds, mask, origin, body=b"\0" * 8)).status_code == 422
    assert (await _publish(client, hd, ds, np.zeros_like(mask), origin)).status_code == 422
    assert (await _publish(client, hd, ds, mask, (17, 0, 0))).status_code == 422
    r = await client.post("/v1/share/selection", params={
        "dataset_id": ds, "source": "headset", "client": "p", "origin_x": 0, "origin_y": 0, "origin_z": 0,
        "width": 1, "height": 1, "depth": 1, "meta": '{"x": NaN}'},
        content=np.ones(1, "<f4").tobytes(), headers=hd)
    assert r.status_code == 422
    r = await client.post("/v1/share/selection", params={
        "dataset_id": ds, "source": "elsewhere", "client": "p", "origin_x": 0, "origin_y": 0, "origin_z": 0,
        "width": 1, "height": 1, "depth": 1}, content=np.ones(1, "<f4").tobytes(), headers=hd)
    assert r.status_code == 422
    # Over the voxel cap, with a body of the right length: refused for the cap.
    import app.routers.share as share_mod
    monkeypatch.setattr(share_mod, "MAX_SHARE_VOXELS", 10)
    r = await _publish(client, hd, ds, mask, origin)
    assert r.status_code == 422 and "10-voxel limit of a shared selection" in r.json()["detail"], r.text
    monkeypatch.setattr(share_mod, "MAX_SHARE_VOXELS", hub_mod.MAX_SHARE_VOXELS)
    # A box whose record names another box.
    box = np.ones((2, 2, 2), dtype=np.float32)
    r = await _publish(client, hd, ds, box, (1, 1, 1), kind="box", meta={"box": [[0, 1], [1, 2], [1, 2]]})
    assert r.status_code == 422 and "meta.box" in r.json()["detail"]
    r = await _publish(client, hd, ds, box, (1, 1, 1), kind="box", meta={"box": [[1, 2], [1, 2], [1, 2]]})
    assert r.status_code == 200, r.text
    # Waiters: none allowed -> a waiting poll is refused, a plain read is not.
    monkeypatch.setattr(share_mod, "MAX_WAITERS_PER_SESSION", 0)
    assert (await client.get("/v1/share/selection", params={"dataset_id": ds, "wait": 1}, headers=hd)).status_code == 429
    assert (await client.get("/v1/share/selection", params={"dataset_id": ds}, headers=hd)).status_code == 200


@pytest.mark.asyncio
async def test_memory_caps_drop_the_oldest(client, auth_headers, tmp_path, monkeypatch):
    data = _cube(tmp_path / "c.fits")
    mask, origin = _mask(data)
    monkeypatch.setattr(hub_mod, "MAX_ENTRIES_PER_SESSION", 2)
    hd = await _session(client, auth_headers)
    ids = []
    for i in range(3):
        p = tmp_path / f"c{i}.fits"
        _cube(p)
        ids.append(await _open(client, hd, p))
        assert (await _publish(client, hd, ids[-1], mask, origin)).status_code == 200
    got = [(await client.get("/v1/share/selection", params={"dataset_id": d}, headers=hd)).status_code for d in ids]
    assert got == [204, 200, 200]
    # The total over sessions: one mask's bytes -> only the newest is kept.
    one = len(np.packbits(mask.ravel() != 0))
    monkeypatch.setattr(hub_mod, "MAX_TOTAL_BYTES", one)
    other = await _session(client, auth_headers)
    od = await _open(client, other, tmp_path / "c0.fits")
    assert (await _publish(client, other, od, mask, origin)).status_code == 200
    assert (await client.get("/v1/share/selection", params={"dataset_id": od}, headers=other)).status_code == 200
    for d in ids[1:]:
        assert (await client.get("/v1/share/selection", params={"dataset_id": d}, headers=hd)).status_code == 204
    assert hub_mod.total_bytes() <= one


@pytest.mark.asyncio
async def test_selections_go_with_the_session_evicted_after_its_idle_time(client, auth_headers, tmp_path, monkeypatch):
    from app.sessions import REGISTRY
    data = _cube(tmp_path / "c.fits")
    hd = await _session(client, auth_headers)
    ds = await _open(client, hd, tmp_path / "c.fits")
    mask, origin = _mask(data)
    assert (await _publish(client, hd, ds, mask, origin)).status_code == 200
    sid = hd["X-Visivo-Session"]
    session = REGISTRY.peek(sid)
    assert session is not None and session in hub_mod._HUBS
    # Idle past the TTL: the registry's own eviction pass drops the session,
    # and its hub (held weakly) with it.
    # Only this session is idle past the TTL: the other tests' sessions (the
    # shared fixtures) are idle for less than an hour and stay.
    monkeypatch.setattr(REGISTRY, "_ttl", 3600)
    session.last_accessed -= 7200
    REGISTRY.evict_stale_once()
    assert REGISTRY.peek(sid) is None
    hubs_before = len(hub_mod._HUBS)
    del session
    gc.collect()
    assert len(hub_mod._HUBS) == hubs_before - 1
    # The id comes back as a new, empty session: no dataset, no selection.
    r = await client.get("/v1/share/selection", params={"dataset_id": ds}, headers=hd)
    assert r.status_code == 404


@pytest.mark.asyncio
async def test_a_selection_goes_when_its_cube_is_closed(client, auth_headers, tmp_path):
    data = _cube(tmp_path / "c.fits")
    hd = await _session(client, auth_headers)
    ds = await _open(client, hd, tmp_path / "c.fits")
    mask, origin = _mask(data)
    _cube(tmp_path / "other.fits")
    other = await _open(client, hd, tmp_path / "other.fits")
    assert (await _publish(client, hd, ds, mask, origin)).status_code == 200
    assert (await client.delete(f"/v1/datasets/{ds}", headers=hd)).status_code == 200
    # The next request of the session drops the closed cube's selection.
    assert (await client.get("/v1/share/selection", params={"dataset_id": other}, headers=hd)).status_code == 204
    from app.sessions import REGISTRY
    assert hub_mod.hub_for(REGISTRY.peek(hd["X-Visivo-Session"])).entries == {}


@pytest.mark.asyncio
async def test_a_slow_earlier_publish_does_not_replace_a_later_one(client, auth_headers, tmp_path, monkeypatch):
    import app.routers.share as share_mod
    data = _cube(tmp_path / "c.fits")
    hd = await _session(client, auth_headers)
    ds = await _open(client, hd, tmp_path / "c.fits")
    mask, origin = _mask(data)
    small, so = _mask(data, 8.0)
    real = share_mod._run_heavy_with_limit
    calls = []

    async def slow_first(*a):
        calls.append(1)
        if len(calls) == 1:
            await asyncio.sleep(0.5)     # A's statistics take longer than B's
        return await real(*a)

    monkeypatch.setattr(share_mod, "_run_heavy_with_limit", slow_first)
    a = asyncio.create_task(_publish(client, hd, ds, mask, origin, client_id="A"))
    await asyncio.sleep(0.1)
    rb = await _publish(client, hd, ds, small, so, client_id="B")
    ra = await a
    assert rb.status_code == 200, rb.text
    assert ra.status_code == 409 and ra.json()["detail"].startswith("stale:"), ra.text
    info = (await client.get("/v1/share/selection", params={"dataset_id": ds}, headers=hd)).json()
    assert info["client"] == "B" and info["seq"] == rb.json()["seq"] and info["count"] == int(small.sum())


@pytest.mark.asyncio
async def test_an_earlier_publish_cannot_land_after_a_clear(client, auth_headers, tmp_path, monkeypatch):
    import app.routers.share as share_mod
    data = _cube(tmp_path / "c.fits")
    hd = await _session(client, auth_headers)
    ds = await _open(client, hd, tmp_path / "c.fits")
    mask, origin = _mask(data)
    real = share_mod._run_heavy_with_limit
    calls = []

    async def slow_first(*a):
        calls.append(1)
        if len(calls) == 1:
            await asyncio.sleep(0.5)
        return await real(*a)

    monkeypatch.setattr(share_mod, "_run_heavy_with_limit", slow_first)
    a = asyncio.create_task(_publish(client, hd, ds, mask, origin, client_id="A"))
    await asyncio.sleep(0.1)
    assert (await _publish(client, hd, ds, mask, origin, client_id="B")).status_code == 200
    assert (await client.delete("/v1/share/selection", params={"dataset_id": ds}, headers=hd)).json() == {"cleared": True}
    ra = await a
    assert ra.status_code == 409, ra.text
    assert (await client.get("/v1/share/selection", params={"dataset_id": ds}, headers=hd)).status_code == 204


@pytest.mark.asyncio
async def test_the_unpacked_mask_goes_with_its_selection(client, auth_headers, tmp_path):
    data = _cube(tmp_path / "c.fits")
    hd = await _session(client, auth_headers)
    ds = await _open(client, hd, tmp_path / "c.fits")
    mask, origin = _mask(data)
    seq = (await _publish(client, hd, ds, mask, origin)).json()["seq"]
    assert (await client.get("/v1/share/selection/mask", params={"dataset_id": ds, "seq": seq}, headers=hd)).status_code == 200
    assert hub_mod._mask_cache["seq"] == seq and hub_mod._mask_cache["raw"]
    # Replaced: dropped.
    seq2 = (await _publish(client, hd, ds, mask, origin)).json()["seq"]
    assert hub_mod._mask_cache["seq"] is None
    # Cached again, then cleared: dropped.
    await client.get("/v1/share/selection/mask", params={"dataset_id": ds, "seq": seq2}, headers=hd)
    assert hub_mod._mask_cache["seq"] == seq2
    await client.delete("/v1/share/selection", params={"dataset_id": ds}, headers=hd)
    assert hub_mod._mask_cache["seq"] is None and hub_mod._mask_cache["raw"] == b""


@pytest.mark.asyncio
async def test_count_statistics_and_mask_follow_one_rule(client, auth_headers, tmp_path):
    """A mask [1, 0.25] over voxels [10, 100]: one voxel selected (> 0.5),
    everywhere: the count, the statistics, the mask handed on."""
    path = tmp_path / "two.fits"
    data = np.zeros((2, 3, 3), dtype=np.float32)    # 2 channels of 3 x 3
    data[0, 0, :2] = [10.0, 100.0]
    h = fits.Header()
    h.update({"CTYPE1": "RA---SIN", "CTYPE2": "DEC--SIN", "CTYPE3": "VELO-HEL", "CRVAL3": 1.0e6,
              "CDELT3": 8000.0, "CUNIT3": "m/s", "CRPIX3": 1.0, "BUNIT": "Jy/beam",
              "CDELT1": -1 / 3600, "CDELT2": 1 / 3600, "CUNIT1": "deg", "CUNIT2": "deg",
              "BMAJ": 4 / 3600, "BMIN": 4 / 3600, "BPA": 0.0})
    fits.PrimaryHDU(data, header=h).writeto(path)
    hd = await _session(client, auth_headers)
    ds = await _open(client, hd, path)
    mask = np.array([[[1.0, 0.25]]], dtype=np.float32)
    r = await _publish(client, hd, ds, mask, (0, 0, 0))
    assert r.status_code == 200, r.text
    pub = r.json()
    assert pub["count"] == 1
    assert pub["stats"]["selected_voxels"] == 1 and pub["stats"]["sum"] == pytest.approx(10.0)
    m = await client.get("/v1/share/selection/mask", params={"dataset_id": ds, "seq": pub["seq"]}, headers=hd)
    got = np.frombuffer(m.content, "<f4")
    assert got.tolist() == [1.0, 0.0] and m.headers["X-Visivo-Selected-Count"] == "1"
    # The received mask's statistics are the published ones.
    again = await client.post("/v1/cube/mask_stats_bin", params={
        "dataset_id": ds, "origin_x": 0, "origin_y": 0, "origin_z": 0, "width": 2, "height": 1, "depth": 1},
        content=m.content, headers={**hd, "Content-Type": "application/octet-stream"})
    assert again.json()["sum"] == pytest.approx(10.0) and again.json()["selected_voxels"] == 1


@pytest.mark.asyncio
async def test_a_publish_under_way_cannot_bring_back_a_cleared_selection(client, auth_headers, tmp_path, monkeypatch):
    """Publish starts (its seq issued), clear, publish completes: refused."""
    import app.routers.share as share_mod
    data = _cube(tmp_path / "c.fits")
    hd = await _session(client, auth_headers)
    ds = await _open(client, hd, tmp_path / "c.fits")
    mask, origin = _mask(data)
    real = share_mod._run_heavy_with_limit
    started = asyncio.Event()

    async def slow(*a):
        started.set()
        await asyncio.sleep(0.4)
        return await real(*a)

    monkeypatch.setattr(share_mod, "_run_heavy_with_limit", slow)
    p = asyncio.create_task(_publish(client, hd, ds, mask, origin, client_id="late"))
    await started.wait()                       # the publish is under way, its seq issued
    assert (await client.delete("/v1/share/selection", params={"dataset_id": ds}, headers=hd)).status_code == 200
    r = await p
    assert r.status_code == 409, r.text
    assert r.json()["detail"].startswith("cleared:"), r.text     # not "a later selection"
    assert (await client.get("/v1/share/selection", params={"dataset_id": ds}, headers=hd)).status_code == 204
    # A publish that arrives after the clear is kept.
    monkeypatch.setattr(share_mod, "_run_heavy_with_limit", real)
    assert (await _publish(client, hd, ds, mask, origin)).status_code == 200


@pytest.mark.asyncio
async def test_a_poller_sees_its_own_publish_and_can_tell_it_is_its_own(client, auth_headers, tmp_path):
    data = _cube(tmp_path / "c.fits")
    hd = await _session(client, auth_headers)
    ds = await _open(client, hd, tmp_path / "c.fits")
    mask, origin = _mask(data)
    poll = asyncio.create_task(client.get("/v1/share/selection", params={"dataset_id": ds, "wait": 5}, headers=hd))
    await asyncio.sleep(0.1)
    pub = (await _publish(client, hd, ds, mask, origin, client_id="me")).json()
    info = (await poll).json()
    # The echo: the same seq, and the publisher's own client id, which the
    # clients skip (share.js shouldPresent, SharedSelection refusal "own").
    assert info["seq"] == pub["seq"] and info["client"] == "me"


@pytest.mark.asyncio
async def test_mask_reads_are_cached_and_off_the_event_loop(client, auth_headers, tmp_path, monkeypatch):
    data = _cube(tmp_path / "c.fits")
    hd = await _session(client, auth_headers)
    ds = await _open(client, hd, tmp_path / "c.fits")
    mask, origin = _mask(data)
    seq = (await _publish(client, hd, ds, mask, origin)).json()["seq"]
    unpacks = []
    real = hub_mod.SharedSelection.mask_float32
    monkeypatch.setattr(hub_mod.SharedSelection, "mask_float32", lambda self: unpacks.append(1) or real(self))
    hub_mod._mask_cache.update(seq=None, raw=b"")
    rs = await asyncio.gather(*[client.get("/v1/share/selection/mask", params={"dataset_id": ds, "seq": seq}, headers=hd)
                                for _ in range(6)])
    assert all(r.status_code == 200 and r.content == rs[0].content for r in rs)
    assert len(unpacks) == 1


@pytest.mark.asyncio
async def test_a_box_is_shared_as_a_mask_of_ones(client, auth_headers, tmp_path):
    data = _cube(tmp_path / "c.fits")
    hd = await _session(client, auth_headers)
    ds = await _open(client, hd, tmp_path / "c.fits")
    box = np.ones((4, 5, 6), dtype=np.float32)
    r = await _publish(client, hd, ds, box, (2, 3, 4), kind="box")
    assert r.status_code == 200, r.text
    assert r.json()["count"] == 120
    assert r.json()["stats"]["sum"] == pytest.approx(float(data[4:8, 3:8, 2:8].astype(np.float64).sum()), rel=1e-6)
    info = (await client.get("/v1/share/selection", params={"dataset_id": ds}, headers=hd)).json()
    m = await client.get("/v1/share/selection/mask", params={"dataset_id": ds, "seq": info["seq"]}, headers=hd)
    assert np.all(np.frombuffer(m.content, "<f4") == 1.0) and len(m.content) == 120 * 4
    assert m.headers["X-Visivo-Selection-Kind"] == "box"


def _gated_publish_setup(share_mod, monkeypatch, raw_len):
    """A budget with room for one body, and packing that waits for a gate (in
    a thread, as the real one: a cancelled request does not stop it)."""
    import threading
    budget = hub_mod.ByteBudget(raw_len)
    monkeypatch.setattr(share_mod, "PUBLISH_BUDGET", budget)
    gate = threading.Event()
    packing = []

    async def gated_pack(body, n):
        def run():
            packing.append(len(body))
            gate.wait(30)
            return hub_mod.pack_mask(body, n)
        return await asyncio.to_thread(run)

    monkeypatch.setattr(share_mod, "pack_mask_async", gated_pack)
    return budget, gate, packing


def _streamed_post(client, hd, ds, mask, origin, raw, name, read):
    async def gen():
        read.append(name)
        yield raw
    d, h, w = mask.shape
    return client.post("/v1/share/selection", params={
        "dataset_id": ds, "source": "headset", "client": name,
        "origin_x": origin[0], "origin_y": origin[1], "origin_z": origin[2],
        "width": w, "height": h, "depth": d},
        content=gen(), headers={**hd, "Content-Type": "application/octet-stream",
                                "Content-Length": str(len(raw))})


async def _until(cond, tries=200):
    for _ in range(tries):
        if cond():
            return
        await asyncio.sleep(0.01)
    raise AssertionError("condition not met")


@pytest.mark.asyncio
async def test_publishes_beyond_the_budget_do_not_read_their_bodies(client, auth_headers, tmp_path, monkeypatch):
    """Each publish buffered its whole body (up to 64 MiB) and only then queued
    for a compute slot, so concurrent publishes held unbounded memory. Now a
    publish takes its body's bytes from a budget BEFORE reading it: with room
    for one, the others wait unread while it is held."""
    import app.routers.share as share_mod
    data = _cube(tmp_path / "c.fits")
    hd = await _session(client, auth_headers)
    ds = await _open(client, hd, tmp_path / "c.fits")
    mask, origin = _mask(data)
    raw = mask.astype("<f4").tobytes()
    budget, gate, packing = _gated_publish_setup(share_mod, monkeypatch, len(raw))
    read = []
    a = asyncio.create_task(_streamed_post(client, hd, ds, mask, origin, raw, "A", read))
    await _until(lambda: packing)
    others = [asyncio.create_task(_streamed_post(client, hd, ds, mask, origin, raw, n, read))
              for n in ("B", "C")]
    await asyncio.sleep(0.3)
    try:
        assert read == ["A"]                        # B and C wait unread
        assert budget.held == len(raw) and budget.waiting == 2
    finally:
        gate.set()
    ra, rb, rc = await asyncio.gather(a, *others)
    assert sorted(read) == ["A", "B", "C"]
    assert rc.status_code == 200, rc.text           # the last to arrive wins
    assert {ra.status_code, rb.status_code} <= {200, 409}
    assert budget.held == 0 and budget.waiting == 0


@pytest.mark.asyncio
async def test_a_cancelled_publish_keeps_its_bytes_until_its_body_is_released(client, auth_headers, tmp_path, monkeypatch):
    """The packing thread outlives a cancelled request and still holds the
    body: the bytes go back to the budget when it ends, not when the request
    does, so the next body stays unread until then."""
    import app.routers.share as share_mod
    data = _cube(tmp_path / "c.fits")
    hd = await _session(client, auth_headers)
    ds = await _open(client, hd, tmp_path / "c.fits")
    mask, origin = _mask(data)
    raw = mask.astype("<f4").tobytes()
    budget, gate, packing = _gated_publish_setup(share_mod, monkeypatch, len(raw))
    read = []
    a = asyncio.create_task(_streamed_post(client, hd, ds, mask, origin, raw, "A", read))
    await _until(lambda: packing)
    a.cancel()
    with pytest.raises(asyncio.CancelledError):
        await a
    b = asyncio.create_task(_streamed_post(client, hd, ds, mask, origin, raw, "B", read))
    await asyncio.sleep(0.3)
    try:
        assert read == ["A"] and budget.held == len(raw)    # A's body is still held
    finally:
        gate.set()
    rb = await b
    assert rb.status_code == 200, rb.text
    assert read == ["A", "B"] and budget.held == 0


@pytest.mark.asyncio
async def test_the_measurement_gets_the_packed_bits_not_the_body(client, auth_headers, tmp_path, monkeypatch):
    """A measurement outlives a request that times out, holding its arguments:
    it is handed the n / 8 packed bytes, and gives the body's statistics."""
    import app.routers.share as share_mod
    data = _cube(tmp_path / "c.fits")
    hd = await _session(client, auth_headers)
    ds = await _open(client, hd, tmp_path / "c.fits")
    mask, origin = _mask(data)
    real = share_mod._run_heavy_with_limit
    sizes = []

    async def spy(session, fn, path, payload, *rest):
        sizes.append(len(payload))
        return await real(session, fn, path, payload, *rest)

    monkeypatch.setattr(share_mod, "_run_heavy_with_limit", spy)
    r = await _publish(client, hd, ds, mask, origin)
    assert r.status_code == 200, r.text
    assert sizes == [(mask.size + 7) // 8]
    direct = await client.post("/v1/cube/mask_stats_bin", params={
        "dataset_id": ds, "origin_x": origin[0], "origin_y": origin[1], "origin_z": origin[2],
        "width": mask.shape[2], "height": mask.shape[1], "depth": mask.shape[0]},
        content=mask.astype("<f4").tobytes(), headers={**hd, "Content-Type": "application/octet-stream"})
    assert direct.status_code == 200, direct.text
    got = {k: v for k, v in r.json()["stats"].items() if k not in ("valid", "error")}
    assert got == {k: v for k, v in direct.json().items() if k not in ("valid", "error")}


@pytest.mark.asyncio
async def test_a_cancelled_or_failed_publish_gives_its_bytes_back():
    budget = hub_mod.ByteBudget(10)
    assert await budget.acquire(8) == 8
    waiter = asyncio.create_task(budget.acquire(5))
    small = asyncio.create_task(budget.acquire(1))   # queued behind: no overtaking
    await asyncio.sleep(0.01)
    assert budget.waiting == 2 and budget.held == 8
    waiter.cancel()
    with pytest.raises(asyncio.CancelledError):
        await waiter
    assert await small == 1 and budget.held == 9      # the head left: the next fits
    budget.release(1)
    budget.release(8)
    assert budget.held == 0
    assert await budget.acquire(10 ** 9) == 10         # over the capacity: all of it, alone
    budget.release(10)


@pytest.mark.asyncio
@pytest.mark.parametrize("meta", ['{"x": 1e999}', '{"x": [-1e400]}', '{"a": {"b": 1E+999}}'])
async def test_a_number_out_of_float_range_in_meta_is_refused(client, auth_headers, tmp_path, meta):
    """json.loads turns 1e999 into inf without calling parse_constant; kept in
    the record, it made every later GET of the selection fail (allow_nan=False)."""
    data = _cube(tmp_path / "c.fits")
    hd = await _session(client, auth_headers)
    ds = await _open(client, hd, tmp_path / "c.fits")
    mask, origin = _mask(data)
    d, h, w = mask.shape
    r = await client.post("/v1/share/selection", params={
        "dataset_id": ds, "source": "headset", "client": "p",
        "origin_x": origin[0], "origin_y": origin[1], "origin_z": origin[2],
        "width": w, "height": h, "depth": d, "meta": meta},
        content=mask.astype("<f4").tobytes(), headers={**hd, "Content-Type": "application/octet-stream"})
    assert r.status_code == 422, r.text
    g = await client.get("/v1/share/selection", params={"dataset_id": ds}, headers=hd)
    assert g.status_code == 204, g.text              # nothing kept to poison the reads
    ok = await _publish(client, hd, ds, mask, origin, meta={"x": 1.5e300, "n": 10 ** 30})
    assert ok.status_code == 200, ok.text
    assert (await client.get("/v1/share/selection", params={"dataset_id": ds}, headers=hd)).status_code == 200


@pytest.mark.asyncio
async def test_packed_bits_waiting_for_a_compute_slot_stay_counted(client, auth_headers, tmp_path, monkeypatch):
    """Once packed, a publish gives back its body's bytes but not its packed
    bits while it waits for a compute slot: waiting publishes cannot pile up
    beyond the budget."""
    import app.routers.share as share_mod
    data = _cube(tmp_path / "c.fits")
    hd = await _session(client, auth_headers)
    ds = await _open(client, hd, tmp_path / "c.fits")
    mask, origin = _mask(data)
    raw = mask.astype("<f4").tobytes()
    packed_len = (mask.size + 7) // 8
    budget = hub_mod.ByteBudget(len(raw))
    monkeypatch.setattr(share_mod, "PUBLISH_BUDGET", budget)
    real = share_mod._run_heavy_with_limit
    slots_free = asyncio.Event()
    waiting = []

    async def busy_slots(*a):
        waiting.append(1)
        await slots_free.wait()                     # every compute slot busy
        return await real(*a)

    monkeypatch.setattr(share_mod, "_run_heavy_with_limit", busy_slots)
    read = []
    a = asyncio.create_task(_streamed_post(client, hd, ds, mask, origin, raw, "A", read))
    await _until(lambda: waiting)
    assert budget.held == packed_len                # the body went back, its bits did not
    b = asyncio.create_task(_streamed_post(client, hd, ds, mask, origin, raw, "B", read))
    await asyncio.sleep(0.3)
    try:
        assert read == ["A"]                        # B's body would not fit beside A's bits
    finally:
        slots_free.set()
    ra, rb = await asyncio.gather(a, b)
    assert rb.status_code == 200, rb.text
    assert ra.status_code in (200, 409), ra.text
    assert budget.held == 0 and budget.waiting == 0
