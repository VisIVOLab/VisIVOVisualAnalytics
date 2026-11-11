"""Selections in the headset, analysis on the backend (issue #171): the
selection statistics route, the full-resolution velocity axis of
reduced_info, and the identity of the VR and desktop paths of the Kinematic
Lasso and the spectrum (the page's own JavaScript builds the VR requests,
run with node)."""
from __future__ import annotations

import importlib.util
import json
import math
import os
import shutil
import subprocess
from pathlib import Path

import numpy as np
import pytest
from astropy.io import fits
from httpx import AsyncClient

REPO = Path(__file__).resolve().parents[2]
C_KMS = 299792.458


def _velo_cube(path: Path, *, bunit="Jy/beam", beam=True) -> np.ndarray:
    """20 x 30 x 24 (z, y, x): noise, a bright blob, one NaN; VELO axis in m/s
    (8 km/s channels), a 4 x 4 arcsec beam on 1 arcsec pixels."""
    rng = np.random.default_rng(3)
    data = rng.normal(0.0, 1.0, (20, 30, 24)).astype(np.float32)
    zz, yy, xx = np.mgrid[0:20, 0:30, 0:24]
    data += (15.0 * np.exp(-(((xx - 12) ** 2 + (yy - 14) ** 2) / 12.0 + (zz - 9) ** 2 / 6.0))).astype(np.float32)
    data[9, 14, 13] = np.nan
    h = fits.Header()
    h.update({"CTYPE1": "RA---SIN", "CTYPE2": "DEC--SIN", "CTYPE3": "VELO-HEL",
              "CRVAL1": 150.0, "CRVAL2": 20.0, "CRVAL3": 1.0e6, "CRPIX1": 12.0, "CRPIX2": 15.0,
              "CRPIX3": 1.0, "CDELT1": -1 / 3600, "CDELT2": 1 / 3600, "CDELT3": 8000.0,
              "CUNIT1": "deg", "CUNIT2": "deg", "CUNIT3": "m/s", "BUNIT": bunit})
    if beam:
        h.update({"BMAJ": 4 / 3600, "BMIN": 4 / 3600, "BPA": 0.0})
    fits.PrimaryHDU(data, header=h).writeto(path, overwrite=True)
    return data


async def _open(client: AsyncClient, auth_headers: dict, path: Path) -> dict:
    r = await client.post("/v1/datasets/open", json={"path": str(path)}, headers=auth_headers)
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["valid"], body["error"]
    return {"dataset_id": body["dataset_id"], "headers": {**auth_headers, "X-Visivo-Session": body["session_id"]}}


async def _mask_stats(client, ds, mask: np.ndarray, origin_xyz, body: bytes | None = None):
    d, h, w = mask.shape
    return await client.post(
        "/v1/cube/mask_stats_bin",
        params={"dataset_id": ds["dataset_id"], "origin_x": origin_xyz[0], "origin_y": origin_xyz[1],
                "origin_z": origin_xyz[2], "width": w, "height": h, "depth": d},
        content=mask.astype("<f4").tobytes() if body is None else body,
        headers={**ds["headers"], "Content-Type": "application/octet-stream"})


@pytest.mark.asyncio
async def test_mask_stats_are_the_selection_on_the_full_cube(client, auth_headers, tmp_path) -> None:
    path = tmp_path / "velo.fits"
    data = _velo_cube(path)
    ds = await _open(client, auth_headers, path)
    full = np.zeros(data.shape, dtype=bool)
    full[6:13, 10:19, 8:17] = data[6:13, 10:19, 8:17] > 3.0
    full[9, 14, 13] = True                       # a blank voxel inside the selection
    origin = (8, 10, 6)                          # x, y, z of the box below
    box = full[6:13, 10:19, 8:17]
    r = await _mask_stats(client, ds, box, origin)
    assert r.status_code == 200, r.text
    s = r.json()
    ok = full & np.isfinite(data)
    vals = data[ok].astype(np.float64)
    assert s["selected_voxels"] == int(full.sum())
    assert s["valid_voxels"] == int(ok.sum()) and s["blank_voxels"] == 1
    assert s["sum"] == pytest.approx(vals.sum(), rel=1e-6)
    assert s["mean"] == pytest.approx(vals.mean(), rel=1e-6)
    assert s["rms"] == pytest.approx(vals.std(), rel=1e-6)
    assert s["min"] == pytest.approx(vals.min()) and s["max"] == pytest.approx(vals.max())
    pz, py, px = np.unravel_index(np.nanargmax(np.where(ok, data, -np.inf)), data.shape)
    assert s["peak_voxel"] == [int(px), int(py), int(pz)]
    zs = np.nonzero(full.any((1, 2)))[0]
    ys = np.nonzero(full.any((0, 2)))[0]
    xs = np.nonzero(full.any((0, 1)))[0]
    assert s["bbox"] == [int(xs[0]), int(xs[-1]), int(ys[0]), int(ys[-1]), int(zs[0]), int(zs[-1])]
    # Velocity: 1000 km/s + 8 km/s per channel (VELO-HEL, its own convention).
    assert s["velocity_min_kms"] == pytest.approx(1000.0 + 8.0 * zs[0])
    assert s["velocity_max_kms"] == pytest.approx(1000.0 + 8.0 * zs[-1])
    # Flux: masked sum / beam area (pixels) x 8 km/s, summed over channels.
    beam_px = math.pi / (4 * math.log(2)) * 16.0
    assert s["flux_jy_kms"] == pytest.approx(vals.sum() / beam_px * 8.0, rel=1e-5)
    assert s["spectrum_unit"] == "Jy"
    per_chan = np.where(ok, data, 0.0).sum((1, 2))[zs[0]:zs[-1] + 1] / beam_px
    np.testing.assert_allclose(s["spectrum"], per_chan, rtol=1e-5)
    assert s["spectrum_channels"] == list(range(int(zs[0]), int(zs[-1]) + 1))
    np.testing.assert_allclose(s["spectrum_velocity_kms"], 1000.0 + 8.0 * np.arange(zs[0], zs[-1] + 1))


@pytest.mark.asyncio
async def test_mask_stats_without_a_beam_gives_no_flux_in_jy(client, auth_headers, tmp_path) -> None:
    for name, kw in (("nobeam.fits", {"beam": False}), ("kelvin.fits", {"bunit": "K"})):
        path = tmp_path / name
        data = _velo_cube(path, **kw)
        ds = await _open(client, auth_headers, path)
        m = np.ones((2, 2, 2), dtype=bool)
        r = await _mask_stats(client, ds, m, (0, 0, 0))
        assert r.status_code == 200, r.text
        s = r.json()
        assert s["flux_jy_kms"] is None and s["flux_note"]
        assert s["sum"] == pytest.approx(float(data[0:2, 0:2, 0:2].astype(np.float64).sum()), rel=1e-6)
        assert s["spectrum_unit"].startswith("sum")


@pytest.mark.asyncio
async def test_mask_stats_refuses_bad_masks(client, auth_headers, tmp_path) -> None:
    path = tmp_path / "velo.fits"
    _velo_cube(path)
    ds = await _open(client, auth_headers, path)
    m = np.ones((2, 2, 2), dtype=bool)
    r = await _mask_stats(client, ds, m, (0, 0, 0), body=b"\0" * 7)
    assert r.status_code == 422, "the body is not width x height x depth float32"
    r = await _mask_stats(client, ds, np.zeros((2, 2, 2), dtype=bool), (0, 0, 0))
    assert r.status_code == 422, "an empty selection"
    r = await _mask_stats(client, ds, m, (23, 0, 0))
    assert r.status_code == 422, "the box leaves the cube"
    r = await client.post("/v1/cube/mask_stats_bin", params={"dataset_id": ds["dataset_id"], "origin_x": 0,
                          "origin_y": 0, "origin_z": 0, "width": 1, "height": 1, "depth": 1},
                          content=b"\0\0\x80?", headers={"X-Visivo-Session": ds["headers"]["X-Visivo-Session"]})
    assert r.status_code == 401


@pytest.mark.asyncio
async def test_reduced_info_gives_every_channel_its_velocity_and_width(
        client: AsyncClient, opened_cube: dict, synthetic_fits_cube) -> None:
    r = await client.post("/v1/cube/reduced_info",
                          json={"dataset_id": opened_cube["dataset_id"], "factor_xy": 2, "factor_z": 4},
                          headers=opened_cube["headers"])
    assert r.status_code == 200, r.text
    z = r.json()["axes"]["z"]
    h = fits.getheader(synthetic_fits_cube)
    nu = h["CRVAL3"] + (np.arange(32) + 1 - h["CRPIX3"]) * h["CDELT3"]
    v = C_KMS * (h["RESTFRQ"] / nu - 1.0)
    assert len(z["velocity_kms"]) == 8, "the reduced axis is binned by 4"
    np.testing.assert_allclose(z["full_velocity_kms"], v, rtol=1e-9)
    edges = h["CRVAL3"] + (np.arange(33) + 0.5 - h["CRPIX3"]) * h["CDELT3"]
    np.testing.assert_allclose(z["full_channel_width_kms"],
                               np.abs(np.diff(C_KMS * (h["RESTFRQ"] / edges - 1.0))), rtol=1e-6)


# ── VR path = desktop path ───────────────────────────────────────────────────

def _identity_module():
    spec = importlib.util.spec_from_file_location("vr_selection_identity",
                                                  REPO / "validation" / "vr_selection_identity.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _node():
    node = shutil.which("node")
    if node is None:
        pytest.skip("node is not installed: the page's JavaScript builds the VR requests")
    return node


@pytest.mark.asyncio
async def test_vr_lasso_and_spectrum_equal_the_desktops(client, auth_headers, tmp_path) -> None:
    """For one seed, the VR request (recipe.js) gives the desktop's mask, both
    the one it shows (level field at the reach) and the one its recipe replays,
    and the desktop's spectrum."""
    _node()
    mod = _identity_module()
    path = tmp_path / "velo.fits"
    data = _velo_cube(path)
    ds = await _open(client, auth_headers, path)
    s = await mod.selections(client, ds["headers"], ds["dataset_id"], data.shape, [12, 14, 9])
    assert s["vr"].sum() > 50
    assert mod.dice(s["vr"], s["desk_shown"]) == 1.0
    assert mod.dice(s["vr"], s["desk_replay"]) == 1.0
    assert s["requests_equal"], s["vr_request"]
    assert s["count_header"] == int(s["vr"].sum())
    assert s["stats"]["selected_voxels"] == int(s["vr"].sum())
    assert s["spectrum_vr"] == s["spectrum_desktop"]
    want = [None if not math.isfinite(v) else pytest.approx(float(v)) for v in data[:, 14, 12]]
    assert s["spectrum_vr"]["values"] == want


@pytest.mark.asyncio
async def test_vr_pick_seeds_the_desktops_selection(client, auth_headers, tmp_path) -> None:
    """The page's pick on the reduced cube (selection.js under node, through
    reduced_bin and the block snap) lands in the source, and the selection
    grown from it is the one grown from the source's peak."""
    _node()
    mod = _identity_module()
    path = tmp_path / "velo.fits"
    data = _velo_cube(path)
    ds = await _open(client, auth_headers, path)
    info = (await client.post("/v1/cube/reduced_info", headers=ds["headers"],
                              json={"dataset_id": ds["dataset_id"], "factor_xy": 2, "factor_z": 1})).json()
    assert info["factors"] == [2, 2, 1]
    n = info["full_shape"]
    ray = {"o": [(12 + 0.5) / n[0], (14 + 0.5) / n[1], 1.5], "d": [0.0, 0.0, -1.0]}
    pick = await mod.vr_seed(client, ds["headers"], ds["dataset_id"], info, ray, tmp_path)
    x, y, z = pick["seed"]
    assert data[z, y, x] > 8.0, "the seed is in the bright source"
    s = await mod.selections(client, ds["headers"], ds["dataset_id"], data.shape, pick["seed"])
    p = await mod.selections(client, ds["headers"], ds["dataset_id"], data.shape, [12, 14, 9])
    assert mod.dice(s["vr"], p["vr"]) == 1.0
    assert mod.dice(s["vr"], s["desk_shown"]) == 1.0


@pytest.mark.asyncio
async def test_vr_box_statistics_are_the_boxes(client, auth_headers, tmp_path) -> None:
    """The page's 3-D box: the one call it makes (mask_stats_bin, box=true, no
    body) gives the statistics of the voxels of the box."""
    _node()
    mod = _identity_module()
    path = tmp_path / "velo.fits"
    data = _velo_cube(path)
    ds = await _open(client, auth_headers, path)
    n = [24, 30, 20]
    a = [(8 + 0.5) / n[0], (19 + 0.5) / n[1], (12 + 0.5) / n[2]]
    b = [(15 + 0.5) / n[0], (10 + 0.5) / n[1], (6 + 0.5) / n[2]]
    plan = mod.node_op({"op": "box", "a": a, "b": b, "fullShape": n, "rayCorners": False})
    assert plan["range"] == [[8, 15], [10, 19], [6, 12]]
    (step,) = plan["steps"]
    assert step["route"] == "/v1/cube/mask_stats_bin"
    params = {k: (str(v).lower() if isinstance(v, bool) else v) for k, v in step["request"].items()}
    r = await client.post("/v1/cube/mask_stats_bin", params={"dataset_id": ds["dataset_id"], **params},
                          content=b"", headers={**ds["headers"], "Content-Type": "application/octet-stream"})
    assert r.status_code == 200, r.text
    got = r.json()
    box = data[6:13, 10:20, 8:16].astype(np.float64)
    fin = box[np.isfinite(box)]
    assert got["selected_voxels"] == box.size and got["valid_voxels"] == fin.size
    assert got["blank_voxels"] == box.size - fin.size == 1
    assert got["sum"] == pytest.approx(fin.sum(), rel=1e-9)
    assert got["mean"] == pytest.approx(fin.mean(), rel=1e-9)
    assert got["rms"] == pytest.approx(fin.std(), rel=1e-9)          # population, as region_stats
    assert got["min"] == pytest.approx(fin.min()) and got["max"] == pytest.approx(fin.max())
    assert got["bbox"] == [8, 15, 10, 19, 6, 12]
    beam_px = math.pi / (4 * math.log(2)) * 16.0
    assert got["flux_jy_kms"] == pytest.approx(fin.sum() / beam_px * 8.0, rel=1e-9)
    # The same box as region statistics per channel, the route the desktop's 2-D
    # region uses: mean, rms and flux agree.
    sums = []
    for c in range(6, 13):
        rs = await client.post(f"/v1/regions/stats/{ds['dataset_id']}", headers=ds["headers"], json={
            "channel": c, "region": {"shape": "box", "frame": "image", "unit": "pix",
                                     "center": [11.5, 14.5], "size": [7, 9], "angle_deg": 0}})
        assert rs.status_code == 200, rs.text
        sums.append(rs.json()["flux_jy"])
    assert got["flux_jy_kms"] == pytest.approx(sum(sums) * 8.0, rel=1e-6)
    # A body with box=true, or none without it, is refused.
    bad = await client.post("/v1/cube/mask_stats_bin", params={"dataset_id": ds["dataset_id"], **params},
                            content=b"\0\0\0\0", headers={**ds["headers"], "Content-Type": "application/octet-stream"})
    assert bad.status_code == 422
    # Corners placed by rays take the velocity slab's channels.
    slab = mod.node_op({"op": "box", "a": a, "b": b, "fullShape": n, "rayCorners": True, "zClip": [0.25, 0.75]})
    assert slab["range"][2] == [5, 14]


@pytest.mark.asyncio
async def test_mask_stats_refuses_a_box_over_the_cap_before_reading_it(client, auth_headers, tmp_path) -> None:
    path = tmp_path / "velo.fits"
    _velo_cube(path)
    ds = await _open(client, auth_headers, path)
    r = await client.post("/v1/cube/mask_stats_bin", params={
        "dataset_id": ds["dataset_id"], "origin_x": 0, "origin_y": 0, "origin_z": 0,
        "width": 4096, "height": 4096, "depth": 8}, content=b"\0" * 16,
        headers={**ds["headers"], "Content-Type": "application/octet-stream"})
    assert r.status_code == 422 and "limit" in r.text
    # A declared length that is not the box's is refused from the header.
    r = await client.post("/v1/cube/mask_stats_bin", params={
        "dataset_id": ds["dataset_id"], "origin_x": 0, "origin_y": 0, "origin_z": 0,
        "width": 2, "height": 2, "depth": 2}, content=b"\0" * 64,
        headers={**ds["headers"], "Content-Type": "application/octet-stream"})
    assert r.status_code == 422 and "expected 32" in r.text


def _freq_multibeam_cube(path: Path) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """A FREQ cube (channels uniform in frequency, so NOT uniform in optical
    velocity: the widths differ by channel) with a BEAMS table whose beam grows
    with channel and a primary-header beam unlike any of them."""
    nz, ny, nx = 12, 10, 10
    data = (np.arange(nz, dtype=np.float32)[:, None, None] + 1.0) * np.ones((nz, ny, nx), np.float32)
    h = fits.Header()
    h.update({"CTYPE1": "RA---SIN", "CTYPE2": "DEC--SIN", "CTYPE3": "FREQ",
              "CRVAL1": 150.0, "CRVAL2": 20.0, "CRVAL3": 1.30e9, "CRPIX1": 5.0, "CRPIX2": 5.0,
              "CRPIX3": 1.0, "CDELT1": -1 / 3600, "CDELT2": 1 / 3600, "CDELT3": 5.0e6,
              "CUNIT1": "deg", "CUNIT2": "deg", "CUNIT3": "Hz", "RESTFRQ": 1.420405752e9,
              "BUNIT": "Jy/beam", "BMAJ": 10 / 3600, "BMIN": 10 / 3600, "BPA": 0.0})
    bmaj = 2.0 + 0.5 * np.arange(nz)                     # arcsec, one per channel
    cols = fits.ColDefs([
        fits.Column(name="BMAJ", format="E", unit="arcsec", array=bmaj),
        fits.Column(name="BMIN", format="E", unit="arcsec", array=bmaj),
        fits.Column(name="BPA", format="E", unit="deg", array=np.zeros(nz)),
        fits.Column(name="CHAN", format="J", array=np.arange(nz)),
        fits.Column(name="POL", format="J", array=np.zeros(nz, dtype=np.int32)),
    ])
    beams = fits.BinTableHDU.from_columns(cols)
    beams.header["EXTNAME"] = "BEAMS"
    fits.HDUList([fits.PrimaryHDU(data, header=h), beams]).writeto(path, overwrite=True)
    nu = 1.30e9 + np.arange(nz) * 5.0e6
    edges = 1.30e9 + (np.arange(nz + 1) - 0.5) * 5.0e6
    widths = np.abs(np.diff(C_KMS * (1.420405752e9 / edges - 1.0)))
    return data, bmaj, widths


@pytest.mark.asyncio
async def test_mask_stats_flux_uses_each_channels_beam_and_width(client, auth_headers, tmp_path) -> None:
    """Per-channel BEAMS and FREQ widths: a header-beam or mean-width flux fails."""
    path = tmp_path / "multibeam.fits"
    data, bmaj, widths = _freq_multibeam_cube(path)
    assert np.ptp(widths) > 1e-3 * widths.mean(), "the channel widths must differ"
    ds = await _open(client, auth_headers, path)
    r = await client.post("/v1/cube/mask_stats_bin", params={
        "dataset_id": ds["dataset_id"], "origin_x": 2, "origin_y": 3, "origin_z": 1,
        "width": 4, "height": 3, "depth": 9, "box": "true"}, content=b"",
        headers={**ds["headers"], "Content-Type": "application/octet-stream"})
    assert r.status_code == 200, r.text
    s = r.json()
    chans = np.arange(1, 10)
    beam_px = math.pi / (4 * math.log(2)) * bmaj[chans] ** 2      # 1 arcsec pixels
    per_chan = data[chans, 3:6, 2:6].sum((1, 2)).astype(np.float64)
    want = float(np.sum(per_chan / beam_px * widths[chans]))
    assert s["flux_jy_kms"] == pytest.approx(want, rel=1e-6)
    header_beam = float(np.sum(per_chan / (math.pi / (4 * math.log(2)) * 100.0) * widths[chans]))
    mean_width = float(np.sum(per_chan / beam_px) * widths[chans].mean())
    assert abs(s["flux_jy_kms"] - header_beam) > 1e-3 * abs(want)
    assert abs(s["flux_jy_kms"] - mean_width) > 1e-6 * abs(want)
    np.testing.assert_allclose(s["spectrum"], per_chan / beam_px, rtol=1e-6)


@pytest.mark.asyncio
async def test_mask_stats_blank_selections_and_transposed_cubes(client, auth_headers, tmp_path) -> None:
    path = tmp_path / "blank.fits"
    data = _velo_cube(path)
    data[:] = np.nan
    data[3, 0, 0] = 1.0
    fits.writeto(path, data, fits.getheader(path), overwrite=True)
    ds = await _open(client, auth_headers, path)
    q = {"dataset_id": ds["dataset_id"], "origin_x": 5, "origin_y": 5, "origin_z": 1,
         "width": 2, "height": 2, "depth": 3, "box": "true"}
    hd = {**ds["headers"], "Content-Type": "application/octet-stream"}
    s = (await client.post("/v1/cube/mask_stats_bin", params=q, content=b"", headers=hd)).json()
    assert s["valid_voxels"] == 0 and s["flux_jy_kms"] is None and s["sum"] is None
    assert s["spectrum"] == [None, None, None], "a channel with no valid voxel has no value"
    # One channel holds a value, the others none: null there, not 0.
    q.update(origin_x=0, origin_y=0, origin_z=2, width=1, height=1)
    s = (await client.post("/v1/cube/mask_stats_bin", params=q, content=b"", headers=hd)).json()
    assert s["spectrum"][0] is None and s["spectrum"][1] is not None and s["spectrum"][2] is None
    # FREQ first: the third numpy axis is not spectral.
    h = fits.Header()
    h.update({"CTYPE1": "FREQ", "CTYPE2": "RA---SIN", "CTYPE3": "DEC--SIN", "CRVAL1": 1.4e9,
              "CDELT1": 1e5, "CRPIX1": 1, "CUNIT1": "Hz", "CRVAL2": 150.0, "CDELT2": -0.01, "CRPIX2": 5,
              "CRVAL3": 20.0, "CDELT3": 0.01, "CRPIX3": 5, "BUNIT": "Jy/beam"})
    tpath = tmp_path / "transposed.fits"
    fits.PrimaryHDU(np.ones((8, 9, 10), dtype=np.float32), header=h).writeto(tpath)
    tds = await _open(client, auth_headers, tpath)
    r = await client.post("/v1/cube/mask_stats_bin", params={**q, "dataset_id": tds["dataset_id"]},
                          content=b"", headers={**tds["headers"], "Content-Type": "application/octet-stream"})
    assert r.status_code == 422 and "spectral axis" in r.text


HCG44 = Path(os.environ.get("VISIVO_VALIDATION_CUBE", "/Users/fvitello/downloads/DemoData/hgc44_3axes.fits"))
SERRA = Path("/Users/fvitello/Downloads/DemoData/hcg44_serra2013_binmask.fits")


@pytest.mark.asyncio
async def test_hcg44_ngc3187_vr_selection_is_the_desktops(client, auth_headers) -> None:
    """The #171 done-when on real data: NGC 3187 in HCG 44, picked as the page
    picks it; Dice 1.0 against both desktop paths, the spectrum identical, and
    the paper's agreement with both references (row A: Dice 0.610 against
    Serra et al. 2013, 0.844 against SoFiA-2)."""
    _node()
    if not HCG44.is_file():
        pytest.skip(f"HCG 44 cube not found: {HCG44} (set VISIVO_VALIDATION_CUBE)")
    mod = _identity_module()
    serra = SERRA if SERRA.is_file() else None
    sofia = mod.DEFAULT_SOFIA if mod.DEFAULT_SOFIA.is_file() else None
    rows = await mod.run(str(HCG44), sofia, serra, client=client, headers=auth_headers)
    assert len(rows) >= 3
    for r in rows:
        assert r["dice_desktop_shown"] == 1.0, r
        assert r["dice_desktop_replay"] == 1.0, r
        assert r["requests_equal"] and r["spectrum_equal_desktop"] and r["spectrum_equal_cube"], r
        if serra is not None:
            assert r["serra_component"] == 13
            assert mod.RANGE_SERRA[0] <= r["dice_serra2013"] <= mod.RANGE_SERRA[1]
            assert r["dice_serra2013"] == pytest.approx(0.610, abs=0.0015)
        if sofia is not None:
            assert r["dice_sofia2"] == pytest.approx(0.844, abs=0.0015)
        assert r["flux_jy_kms"] == pytest.approx(6.06, abs=0.01)     # the paper's F_Lasso, row A
