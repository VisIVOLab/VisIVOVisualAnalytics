"""Stereo frames and the VR page's clipping box for a cube drawn out of core
(part of epic #166).

A session that draws its cube from the brick pyramid gives the headset the same
frames as an in-core one: the same placement, the depth of the first sample
above the threshold, the clipping box. The eyes are drawn from one whole
pyramid level, the finest within the GPU budget. Checked on the synthetic
sphere of ``test_render_stereo``, forced out of core, against the in-core
session and against the geometry; the block logic is checked without a GPU.
"""
from __future__ import annotations

import io

import numpy as np
import pytest

from app.render_session import _stereo as S
from tests.test_render_stereo import (IPD, DIST, N, RADIUS, SIDE, _can_render, _sphere,
                                      expected_distance, pose)

vtk = pytest.importorskip("vtk")

from app.render_session import _ooc  # noqa: E402


# ── Without a GPU: the block container ──────────────────────────────────────

def _blocks_with(keys_crops):
    prop = vtk.vtkVolumeProperty()
    blocks = _ooc.BlockVolumes(prop)
    blocks.attach(vtk.vtkRenderer())
    arrays = {}
    for key, (origin, spacing, dims, crop) in keys_crops.items():
        nx, ny, nz = dims
        arrays[key] = (np.zeros((nz, ny, nx), np.float32), origin, spacing, crop)
    blocks.replace(arrays)
    return blocks


def test_the_clipping_box_crops_every_block_within_its_own_region():
    # Two blocks side by side along x: [0, 15.5) and [15.5, 31].
    blocks = _blocks_with({
        "a": ((-1.0, -1.0, -1.0), 1.0, (18, 34, 34), (0.0, 15.5, 0.0, 31.0, 0.0, 31.0)),
        "b": ((14.0, -1.0, -1.0), 1.0, (19, 34, 34), (15.5, 31.0, 0.0, 31.0, 0.0, 31.0)),
    })
    mapper = {k: blocks._blocks[k][1] for k in "ab"}
    volume = {k: blocks._blocks[k][2] for k in "ab"}
    own = {k: tuple(mapper[k].GetCroppingRegionPlanes()) for k in "ab"}
    # The box x in [-0.5, 9.5]: block a cropped to [0, 9.5], block b hidden.
    blocks.set_user_crop((-0.5, 9.5, -0.5, 31.5, -0.5, 31.5))
    want = _ooc._vtk_crop_planes((0.0, 9.5, 0.0, 31.0, 0.0, 31.0), (-1.0, -1.0, -1.0), 1.0,
                                 (18, 34, 34))
    assert tuple(mapper["a"].GetCroppingRegionPlanes()) == pytest.approx(want)
    assert volume["a"].GetVisibility() == 1 and volume["b"].GetVisibility() == 0
    # A block added while the box is set is cropped too.
    blocks.replace({**{k: (np.zeros((34, 34, 18), np.float32), (-1.0, -1.0, -1.0), 1.0,
                           (0.0, 15.5, 0.0, 31.0, 0.0, 31.0)) for k in ["a"]},
                    "c": (np.zeros((34, 34, 18), np.float32), (14.0, -1.0, -1.0), 1.0,
                          (15.5, 31.0, 0.0, 31.0, 0.0, 15.0))})
    assert blocks._blocks["c"][2].GetVisibility() == 0
    # No box: each block back to its own region, all shown.
    blocks.set_user_crop(None)
    assert tuple(blocks._blocks["a"][1].GetCroppingRegionPlanes()) == pytest.approx(own["a"])
    assert all(v.GetVisibility() == 1 for _, _, v in blocks._blocks.values())


def test_render_to_image_and_the_stretch_reach_every_block():
    blocks = _blocks_with({
        "a": ((-1.0, -1.0, -1.0), 1.0, (10, 10, 10), (0.0, 7.5, 0.0, 7.0, 0.0, 7.0))})
    stretch = vtk.vtkMatrix4x4()
    stretch.SetElement(2, 2, 3.0)
    blocks.SetUserMatrix(stretch)
    blocks.SetRenderToImage(1)
    _, mapper, volume = blocks._blocks["a"]
    assert mapper.GetRenderToImage() == 1
    assert volume.GetUserMatrix().GetElement(2, 2) == 3.0
    # ...and to a block shown later.
    blocks.replace({"b": (np.zeros((10, 10, 10), np.float32), (-1.0, -1.0, -1.0), 1.0,
                          (0.0, 7.5, 0.0, 7.0, 0.0, 7.0))})
    _, mapper, volume = blocks._blocks["b"]
    assert mapper.GetRenderToImage() == 1 and volume.GetUserMatrix().GetElement(2, 2) == 3.0
    blocks.SetUserMatrix(None)
    blocks.SetRenderToImage(0)
    assert volume.GetUserMatrix() is None and mapper.GetRenderToImage() == 0


# ── With a GPU: in core against out of core ─────────────────────────────────

def _lit(jpeg: bytes, size: int) -> np.ndarray:
    from PIL import Image
    return np.asarray(Image.open(io.BytesIO(jpeg)).convert("L"), dtype=np.float32)


async def _frames(path, session_id: str) -> dict:
    from app.render_session._process import RenderSessionProcess
    size = 192
    session = RenderSessionProcess(session_id, str(path))
    await session.initialize()
    out = {}
    try:
        await session.apply_command({"type": "set_threshold", "value": 0.5})
        out["mono"] = await session.render_frame()
        await session.apply_command(pose(7, size, depth={"scale": 1}))
        out["stereo"] = await session.render_frame()
        await session.apply_command(pose(8, size, depth={"scale": 4}, z_stretch=2.0))
        out["stretched"] = await session.render_frame()
        await session.apply_command({"type": "set_crop",
                                     "box": {"min": [0, 0, 0], "max": [0.5, 1, 1]}})
        await session.apply_command(pose(9, size, ipd=0.0, depth=False))
        out["half"] = await session.render_frame()
        await session.apply_command({"type": "set_crop", "box": None})
        await session.apply_command(pose(10, size, ipd=0.0, depth=False))
        out["whole"] = await session.render_frame()
        await session.apply_command({"type": "xr_stop"})
        out["mono_again"] = await session.render_frame()
    finally:
        await session.close()
    return out


def _depth(frame):
    header, _, depth = S.parse_frame(frame.to_bytes())
    d = header["depth"]
    return header, S.decode_depth(depth, d["width"], d["height"], d["z_near"], d["z_far"])


def _centroid(img):
    cols = np.arange(img.shape[1])
    return (img.sum(0) * cols).sum() / img.sum()


@pytest.mark.skipif(not _can_render(), reason="needs macOS with a logged-in user, or "
                    "VISIVO_TEST_EGL_RENDER=1 on a host with a real EGL GPU")
@pytest.mark.parametrize("budget_level", [0, 1])
async def test_out_of_core_stereo_frames_match_the_in_core_ones(tmp_path, monkeypatch,
                                                                budget_level):
    from app.brick_pyramid import build_pyramid, default_pyramid_path

    path = tmp_path / "sphere.fits"
    _sphere(path)
    size = 192
    inside = await _frames(path, f"stereoic{budget_level}")
    # Out of core from now on (the render child reads these when it starts):
    # level 0 (64^3 with ghosts) fits the budget, or only level 1 does.
    monkeypatch.setenv("VISIVO_PYRAMID_DIR", str(tmp_path / "pyr"))
    monkeypatch.setenv("VISIVO_RENDER_OUT_OF_CORE_BYTES", "1")
    monkeypatch.setenv("VISIVO_RENDER_OOC_NODE", "32")
    budget = 4 * (N + 2) ** 3 if budget_level == 0 else 4 * (N // 2 + 2) ** 3
    monkeypatch.setenv("VISIVO_RENDER_OOC_BUDGET_BYTES", str(budget))
    build_pyramid(path, default_pyramid_path(path), brick=16)
    outside = await _frames(path, f"stereoooc{budget_level}")

    h_in, d_in = _depth(inside["stereo"])
    h_out, d_out = _depth(outside["stereo"])
    assert "volume" not in h_in
    assert h_out["volume"] == {"out_of_core": True, "level": budget_level, "levels": 3}
    # The ray step: half a voxel of the volume drawn, as the drawing mapper
    # holds it (level 1's voxels are two of the cube's).
    assert [e["sample_distance"] for e in h_in["eye_ms"]] == [0.5, 0.5]
    assert [e["sample_distance"] for e in h_out["eye_ms"]] == [0.5 * 2 ** budget_level] * 2
    assert h_out["depth"]["source"] == "volume-first-hit"
    voxel = SIDE / N
    tol_median, tol_95 = (0.5, 1.0) if budget_level == 0 else (1.0, 2.0)
    for i, ex in enumerate((-IPD / 2, IPD / 2)):
        want = expected_distance(size, ex)
        for got in (d_in[:, i * size:(i + 1) * size], d_out[:, i * size:(i + 1) * size]):
            both = np.isfinite(got) & np.isfinite(want)
            assert both.sum() > 0.9 * np.isfinite(want).sum()
            err = np.abs(got[both] - want[both]) / voxel
            assert np.median(err) < tol_median and np.percentile(err, 95) < tol_95
    # Placement: the sphere where the in-core frame has it, in both eyes.
    _, j_in, _ = S.parse_frame(inside["stereo"].to_bytes())
    _, j_out, _ = S.parse_frame(outside["stereo"].to_bytes())
    a, b = _lit(j_in, size), _lit(j_out, size)
    for eye in (slice(0, size), slice(size, 2 * size)):
        assert _centroid(a[:, eye]) == pytest.approx(_centroid(b[:, eye]), abs=0.5)
    lit_in, lit_out = a > 20, b > 20
    assert abs(int(lit_in.sum()) - int(lit_out.sum())) < 0.03 * lit_in.sum()
    # The spectral stretch: the same reduced depth image size and coverage.
    hs_in, ds_in = _depth(inside["stretched"])
    hs_out, ds_out = _depth(outside["stretched"])
    assert ds_in.shape == ds_out.shape == (size // 4, 2 * size // 4)
    assert abs(np.isfinite(ds_in).sum() - np.isfinite(ds_out).sum()) < 0.05 * np.isfinite(ds_in).sum()
    both = np.isfinite(ds_in) & np.isfinite(ds_out)
    assert np.median(np.abs(ds_in[both] - ds_out[both])) / voxel < (0.5 if budget_level == 0 else 1.0)
    # The clipping box: the left half, as in core.
    halves = []
    for frames in (inside, outside):
        _, jh, _ = S.parse_frame(frames["half"].to_bytes())
        _, jw, _ = S.parse_frame(frames["whole"].to_bytes())
        h, w = _lit(jh, size)[:, :size] > 20, _lit(jw, size)[:, :size] > 20
        assert 0.35 < h.sum() / w.sum() < 0.6
        assert not h[:, size // 2 + 2:].any()
        halves.append(h)
    assert np.logical_xor(*halves).sum() < 0.03 * halves[0].sum()
    # Back to the mono view (its blocks: test_the_mono_view_chooses_its_blocks_again).
    for frames in (inside, outside):
        assert frames["mono"][:2] == b"\xff\xd8" and frames["mono_again"][:2] == b"\xff\xd8"
    assert RADIUS < N / 2 and DIST > SIDE


# ── Without a GPU: the stereo block and the mono view's ────────────────────

def test_the_stereo_level_fits_the_gpus_3d_texture_side(tmp_path, monkeypatch):
    from tests.test_render_ooc import _session_objects
    monkeypatch.setenv("VISIVO_PYRAMID_DIR", str(tmp_path / "pyr"))
    monkeypatch.setattr(_ooc, "OOC_NODE", 16)
    monkeypatch.setattr(_ooc, "OOC_BUDGET_BYTES", 4 * 10 ** 6)
    data = np.random.default_rng(1).normal(size=(12, 40, 120)).astype(np.float32)
    path = tmp_path / "wide.fits"
    from astropy.io import fits
    fits.PrimaryHDU(data).writeto(path)
    ooc, blocks, renderer = _session_objects(path)
    # Level 0 (122 wide with its ghosts) is within the budget but not a 100-texel side.
    assert ooc._whole_level(10 ** 6).level == 0
    assert ooc._whole_level(10 ** 6, max_side=100).level == 1
    with pytest.raises(ValueError, match="per side"):
        ooc._whole_level(10 ** 6, max_side=4)
    assert ooc.show_whole_level(renderer, (64, 64), 100) == 1
    assert ooc.whole_level == 1
    # The real side read after the first pose (the fallback was smaller): the
    # level is chosen again and shown, not refused as "could not be shown".
    assert ooc.show_whole_level(renderer, (64, 64), 16384) == 0
    assert ooc.whole_level == 0
    ooc.close()


def test_only_a_level_that_fits_nowhere_refuses_stereo_for_good():
    """A ValueError (no level fits) becomes an `out_of_core` refusal; a level
    that could not be shown this time is an error without a code, so the page
    keeps its session and asks again."""
    class Stub:
        def __init__(self, exc):
            self.exc = exc

        def show_whole_level(self, *_a):
            raise self.exc

    class Session(S.StereoMixin):
        def __init__(self, exc):
            self._xr_error = None
            self._xr_pose = S.parse_pose(pose(3, 64))
            self._ooc = Stub(exc)
            self._xr_max_3d = 2048
            self._render_window = self._renderer = None

    gone = Session(ValueError("no level of the cube fits the budget")).\
        _render_xr_frame_on_thread()
    assert gone.error and gone.code == "out_of_core"
    flaky = Session(RuntimeError("pyramid level 1 could not be shown"))
    codes = []
    for _ in range(S.STEREO_SHOW_TRIES):
        flaky._xr_pose = S.parse_pose(pose(4, 64))
        codes.append(flaky._render_xr_frame_on_thread().code)
    # Retried a few times, then given up for the session, not forever.
    assert codes == [None] * (S.STEREO_SHOW_TRIES - 1) + ["out_of_core"]


def test_the_mono_view_chooses_its_blocks_again_after_stereo(tmp_path, monkeypatch):
    from tests.test_render_ooc import _session_objects, _settle
    monkeypatch.setenv("VISIVO_PYRAMID_DIR", str(tmp_path / "pyr"))
    monkeypatch.setattr(_ooc, "OOC_NODE", 16)
    monkeypatch.setattr(_ooc, "OOC_BUDGET_BYTES", 4 * 40_000)
    monkeypatch.setattr(_ooc, "OOC_MAX_BLOCKS", 64)
    monkeypatch.setattr(_ooc, "_PROXY_VOXELS", 20 ** 3)
    data = np.random.default_rng(1).normal(size=(40, 48, 56)).astype(np.float32)
    path = tmp_path / "cube.fits"
    from astropy.io import fits
    fits.PrimaryHDU(data).writeto(path)
    ooc, blocks, renderer = _session_objects(path)
    _settle(ooc, renderer)
    mono = set(blocks.nodes)
    assert ooc.whole_level is None and len(mono) > 1
    level = ooc.show_whole_level(renderer, (400, 300))
    assert ooc.whole_level == level and len(blocks.nodes) == 1
    _settle(ooc, renderer)                     # the next mono frame, after xr_stop
    assert ooc.whole_level is None and set(blocks.nodes) == mono
    ooc.close()
