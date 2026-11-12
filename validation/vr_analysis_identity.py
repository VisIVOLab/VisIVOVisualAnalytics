#!/usr/bin/env python
"""The headset's analysis tools give the desktop's numbers (issue #191).

On HCG 44 and NGC 3187, through the routes in-process (httpx over ASGI) and
the page's own JavaScript under node (``vr_tools.mjs``, ``vr_pick.mjs``):

1. Lasso refinement. A seed picked in VR on NGC 3187, a seed ADDED on
   NGC 3190 (SoFiA-2 source 7) and a seed CARVED inside NGC 3187, each placed
   by the page's ray pick. The page's grow request after each press equals the
   desktop's (LassoReplay with the seed lists), and its mask equals what the
   desktop shows (the Reach slider's level field with the same seeds,
   thresholded at the reach) and what its recipe replays, voxel for voxel.
2. Isolate and Remove. The page posts the mask it received to
   ``mask_apply_bin``; the desktop posts the mask it shows (the level field's
   ROI). The two new cubes are identical, and Isolate's finite voxels are the
   selection's (Remove's the rest).
3. Moment maps M0, M1, M2 of the selection: the page's request on its isolated
   cube equals the desktop's on its own (``/v1/products/moment``, the whole
   channel range, no threshold mask), bit for bit. Physics checks: the sum of
   M0 over the map divided by the beam area is the selection's flux from
   ``mask_stats_bin`` (6.061 Jy km/s for the default selection); the
   M0-weighted mean of M1 is the flux-weighted velocity of its spectrum.
4. PV along a path whose vertices the page's ray pick places across NGC 3187:
   the page's request equals the desktop's for the same vertices, the diagram
   is identical, its rows are in the desktop's order (increasing spectral
   coordinate from the bottom, PvPathGeometry's rowsOnUniformSpectralGrid),
   and the emission peaks at NGC 3187's velocity. The same path on a copy of
   the cube with the spectral axis reversed (CDELT3 < 0, written to the
   scratch directory) gives the same picture row for row.

New cubes go to VISIVO_EXPORTS_DIR (set it to a scratch directory).

Run:  VISIVO_TOKEN=x VISIVO_EXPORTS_DIR=/tmp/x VISIVO_SOFIA_OUT=.../sofia_out \\
      backend/.venv/bin/python validation/vr_analysis_identity.py [--cube PATH] [--scratch DIR]
"""
from __future__ import annotations

import argparse
import base64
import asyncio
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import vr_selection_identity as VS  # noqa: E402  (the #171 identity check's helpers)

SOFIA_NGC3190 = 7      # SoFiA-2 source 7 = NGC 3190 (the neighbour of NGC 3187)
DESKTOP = {"reach": 8.0, "lambda_v": 1.0, "sigma": 0.0, "nsigma": 2.5, "roi_radius": 0, "reach_max": 64.0}


def node(script: str, request: dict) -> dict:
    exe = shutil.which("node")
    if exe is None:
        raise RuntimeError("node is not installed: the page's JavaScript needs it")
    r = subprocess.run([exe, str(HERE / script)], input=json.dumps(request), capture_output=True,
                       text=True, timeout=180)
    if r.returncode != 0:
        raise RuntimeError(r.stderr[-2000:])
    return json.loads(r.stdout)


REPO = HERE.parent


def cpp_json_keys(function: str) -> list[str]:
    """The JSON keys a BackendClient function sends, read from its C++ source
    (``{ QStringLiteral("key"), value }`` pairs of its QJsonObject)."""
    import re  # noqa: PLC0415
    src = (REPO / "src/app/BackendClient.cpp").read_text()
    start = src.index(f"BackendClient::{function}(")
    end = src.index("performPost", start)
    return re.findall(r'\{\s*QStringLiteral\("(\w+)"\)\s*,', src[start:end])


def desktop_pv_width() -> int:
    """vtkWindowCube's default slit width, read from its header."""
    import re  # noqa: PLC0415
    m = re.search(r"int pvWidthPixels\{\s*(\d+)\s*\}", (REPO / "src/gui/vtkWindowCube.h").read_text())
    return int(m.group(1))


def rows_on_uniform_spectral_grid(values: np.ndarray, spectral: np.ndarray) -> np.ndarray:
    """A line-for-line port of the desktop's pvpath::rowsOnUniformSpectralGrid
    (src/gui/PvPathGeometry.cpp): the rows the PV widget draws, bottom first."""
    ny, nx = values.shape
    if ny <= 1 or spectral.size != ny:
        return values.copy()
    order = sorted(range(ny), key=lambda i: spectral[i])      # std::sort by coordinate
    srt = spectral[order]
    lo, hi = float(srt[0]), float(srt[-1])
    if not (hi > lo) or not math.isfinite(lo) or not math.isfinite(hi):
        return values.copy()
    steps = np.diff(srt)
    min_step, max_step = float(steps.min()), float(steps.max())
    linear = min_step > 0.0 and (max_step - min_step) <= 1e-6 * max_step
    needed = math.ceil((hi - lo) / min_step) + 1.0 if min_step > 0.0 else 0.0
    fine = (not linear) and min_step > 0.0 and needed <= max(4096.0, 8.0 * ny)
    if not fine:
        return values[order].copy()
    rows = int(needed)
    out = np.empty((rows, nx), dtype=values.dtype)
    for r in range(rows):
        y = lo + (hi - lo) * r / (rows - 1)
        k = int(np.searchsorted(srt, y, side="left"))
        if k >= srt.size:
            k = srt.size - 1
        elif k > 0 and (y - srt[k - 1]) <= (srt[k] - y):
            k -= 1
        out[r] = values[order[k]]
    return out


def roi_of(headers) -> tuple[list[int], list[int]]:
    dims = [int(headers[k]) for k in ("X-Visivo-Width", "X-Visivo-Height", "X-Visivo-Depth")]
    origin = [int(v) for v in headers["X-Visivo-Origin"].split(",")]
    return origin, dims


def levels_bytes(body: bytes, headers, reach: float) -> bytes:
    """The desktop's displayed mask (KinematicLevelField::thresholdAt) as the
    float32 bytes it applies, over the level field's ROI."""
    origin, (w, h, d) = roi_of(headers)
    unreachable = int(headers["X-Visivo-Peak"])
    n = w * h * d
    raw = np.frombuffer(body, dtype=np.uint8)
    add, sub = raw[:n], raw[n:2 * n]
    sel = (add <= reach) & (add != unreachable) & ~((sub <= reach) & (sub != unreachable))
    return sel.astype("<f4").tobytes()


def nan_equal(a: np.ndarray, b: np.ndarray) -> bool:
    return a.shape == b.shape and bool(np.array_equal(np.isnan(a), np.isnan(b))) \
        and bool(np.array_equal(a[~np.isnan(a)], b[~np.isnan(b)]))


async def post_mask(client, hd, route, ds, origin, dims, body, **extra):
    q = {"dataset_id": ds, "origin_x": origin[0], "origin_y": origin[1], "origin_z": origin[2],
         "width": dims[0], "height": dims[1], "depth": dims[2], **extra}
    r = await client.post(route, headers={**hd, "Content-Type": "application/octet-stream"}, params=q, content=body)
    r.raise_for_status()
    return r.json()


async def spectral_ray_seed(client, hd, ds, info, cube, px, py, tmp, seed_at="peak"):
    """The page's pick along the spectral axis at sky pixel (px, py), snapped as the page does."""
    n = info["full_shape"]
    ray = {"o": [(px + 0.5) / n[0], (py + 0.5) / n[1], 1.5], "d": [0.0, 0.0, -1.0]}
    return await VS.vr_seed(client, hd, ds, info, ray, tmp, seed_at=seed_at)


def reverse_spectral(cube: str, out: Path) -> Path:
    """A copy of the cube with the spectral axis reversed (CDELT3 < 0)."""
    from astropy.io import fits  # noqa: PLC0415
    with fits.open(cube) as hdul:
        h = hdul[0].header.copy()
        data = np.asarray(hdul[0].data)
    n = int(h["NAXIS3"])
    crval, cdelt, crpix = float(h["CRVAL3"]), float(h["CDELT3"]), float(h.get("CRPIX3", 1.0))
    h["CRVAL3"] = crval + (n - crpix) * cdelt       # the old last channel
    h["CRPIX3"] = 1.0
    h["CDELT3"] = -cdelt
    h.add_history("VisIVO validation: spectral axis reversed (issue #191 PV orientation check)")
    flipped = np.ascontiguousarray(data[..., ::-1, :, :] if data.ndim == 4 else data[::-1, :, :])
    fits.writeto(out, flipped, h, overwrite=True)
    return out


async def run(cube: str, scratch: Path, sofia_mask: Path | None) -> dict:
    from astropy.io import fits  # noqa: PLC0415
    import httpx  # noqa: PLC0415
    from app.main import app  # noqa: PLC0415
    client = httpx.AsyncClient(transport=httpx.ASGITransport(app=app), base_url="http://vr", timeout=600)
    headers = {"X-Visivo-Token": os.environ["VISIVO_TOKEN"]}
    out: dict = {}
    try:
        r = await client.post("/v1/datasets/open", json={"path": cube}, headers=headers)
        r.raise_for_status()
        body = r.json()
        hd = {**headers, "X-Visivo-Session": body["session_id"]}
        ds = body["dataset_id"]
        info = (await client.post("/v1/cube/reduced_info", headers=hd, json={"dataset_id": ds})).json()
        hdr = fits.getheader(cube)
        data = np.squeeze(fits.getdata(cube)).astype(np.float32)
        shape = data.shape
        sofia = np.squeeze(fits.getdata(sofia_mask)).astype(np.int32) if sofia_mask and sofia_mask.is_file() else None
        n = info["full_shape"]
        with tempfile.TemporaryDirectory(dir=scratch) as td:
            tmp = Path(td)
            # ── 1. seeds: NGC 3187 (new), NGC 3190 (add), a carve inside NGC 3187 ──
            ray0 = VS.source_ray(info, cube, "spectral")
            px0, py0 = ray0["pixel"]
            p_new = await spectral_ray_seed(client, hd, ds, info, cube, px0, py0, tmp)
            if sofia is not None:
                zz, yy, xx = np.where(sofia == SOFIA_NGC3190)
                k = int(np.nanargmax(data[zz, yy, xx]))
                ax, ay = int(xx[k]), int(yy[k])
            else:
                ax, ay = 434, 279
            p_add = await spectral_ray_seed(client, hd, ds, info, cube, ax, ay, tmp)
            # The carve: a few pixels from NGC 3187's centre (the first offset whose ray
            # draws), at the first drawn sample.
            p_sub = None
            for dx, dy in ((8, 4), (6, 0), (4, 4), (-6, 0), (0, 6), (3, 0)):
                try:
                    p_sub = await spectral_ray_seed(client, hd, ds, info, cube, px0 + dx, py0 + dy, tmp, seed_at="hit")
                    out["carve_offset"] = [dx, dy]
                    break
                except RuntimeError:
                    continue
            if p_sub is None:
                raise RuntimeError("no carve ray draws near NGC 3187")
            steps = [{"mode": "new", "seed": p_new["seed"]}, {"mode": "add", "seed": p_add["seed"]},
                     {"mode": "carve", "seed": p_sub["seed"]}]
            page = node("vr_tools.mjs", {"op": "seeds", "steps": steps})["steps"]
            rows = []
            masks = {}
            for i, st in enumerate(page):
                req = st["request"]
                s = st["seeds"]
                desk = {"dataset_id": ds, "seed_z": s["seed"][0], "seed_y": s["seed"][1], "seed_x": s["seed"][2],
                        **({"add_seeds": s["add"]} if s["add"] else {}), **({"sub_seeds": s["sub"]} if s["sub"] else {}),
                        **DESKTOP}
                a = await client.post("/v1/cube/kinematic_select_bin", headers=hd, json={"dataset_id": ds, **req})
                a.raise_for_status()
                c = await client.post("/v1/cube/kinematic_select_bin", headers=hd, json=desk)
                c.raise_for_status()
                lv = await client.post("/v1/cube/kinematic_levels_bin", headers=hd, json=desk)
                lv.raise_for_status()
                vr = VS.full_mask(shape, a.content, a.headers)
                shown = VS.levels_mask(shape, lv.content, lv.headers, DESKTOP["reach"])
                replay = VS.full_mask(shape, c.content, c.headers)
                origin, dims = roi_of(a.headers)
                stats = await post_mask(client, hd, "/v1/cube/mask_stats_bin", ds, origin, dims, a.content)
                rows.append({"step": steps[i]["mode"], "seed_xyz": steps[i]["seed"], "voxels": int(vr.sum()),
                             "request_equal": req == {k: v for k, v in desk.items() if k != "dataset_id"},
                             "dice_shown": VS.dice(vr, shown), "dice_replay": VS.dice(vr, replay),
                             "flux": stats["flux_jy_kms"]})
                masks[steps[i]["mode"]] = {"vr": a, "levels": lv, "req": req, "stats": stats, "mask": vr}
            out["seeds"] = rows

            # ── 2. Isolate / Remove of the default selection (NGC 3187, one seed) and of the refined one ──
            apply_rows = []
            iso_ids = {}
            for which in ("new", "carve"):
                m = masks[which]
                origin, dims = roi_of(m["vr"].headers)
                lv_origin, lv_dims = roi_of(m["levels"].headers)
                lv_bytes = levels_bytes(m["levels"].content, m["levels"].headers, DESKTOP["reach"])
                for mode in ("blank_outside", "blank_inside"):
                    pq = node("vr_tools.mjs", {"op": "apply", "request": m["req"], "datasetId": ds, "origin": origin,
                                               "dims": dims, "mode": mode, "trimBox": int(m["vr"].headers.get("X-Visivo-Trim-Box", 0)),
                                               "count": int(m["mask"].sum())})["query"]
                    q = {k: v for k, v in pq.items() if k != "dataset_id"}
                    rv = await client.post("/v1/cube/mask_apply_bin", headers={**hd, "Content-Type": "application/octet-stream"},
                                           params={"dataset_id": ds, **q}, content=m["vr"].content)
                    rv.raise_for_status()
                    rv = rv.json()
                    rd = await post_mask(client, hd, "/v1/cube/mask_apply_bin", ds, lv_origin, lv_dims, lv_bytes, mode=mode)
                    assert rv["valid"] and rd["valid"], (rv, rd)
                    cv = np.squeeze(fits.getdata(rv["output_path"])).astype(np.float32)
                    cd = np.squeeze(fits.getdata(rd["output_path"])).astype(np.float32)
                    finite = np.isfinite(cv)
                    expect = (m["mask"] if mode == "blank_outside" else ~m["mask"]) & np.isfinite(data)
                    apply_rows.append({"selection": which, "mode": mode, "identical": nan_equal(cv, cd),
                                       "finite": int(finite.sum()), "expected_finite": int(expect.sum()),
                                       "kept_voxels_equal": bool(np.array_equal(finite, expect)),
                                       "file": rv["workspace_filename"]})
                    if mode == "blank_outside":
                        iso_ids[which] = (rv["new_dataset_id"], rd["new_dataset_id"], rv["workspace_filename"])
            out["apply"] = apply_rows

            # ── 3. Moments of the selections ──
            bmaj, bmin = float(hdr["BMAJ"]), float(hdr["BMIN"])
            pix = abs(float(hdr["CDELT1"]) * float(hdr["CDELT2"]))
            beam_pix = math.pi / (4 * math.log(2)) * bmaj * bmin / pix
            mom_rows = []
            for which in ("new", "carve"):
                vr_id, desk_id, file = iso_ids[which]
                st = masks[which]["stats"]
                maps = {}
                for order in (0, 1, 2):
                    preq = node("vr_tools.mjs", {"op": "momentRequest", "order": order, "depth": n[2]})["request"]
                    rv = await client.post("/v1/products/moment", headers=hd, json={"dataset_id": vr_id, **preq})
                    rv.raise_for_status()
                    # The desktop's request (BackendClient::requestMoment's keys, read from
                    # its source) with the page's values: the dialog's default channel range
                    # (MomentMapController: 0..depth-1) and the mask OFF (the dialog defaults to
                    # an automatic ~3 sigma mask; on an isolated selection the page turns it off).
                    dkeys = cpp_json_keys("requestMoment")
                    dreq = {"dataset_id": desk_id, **preq}
                    rd = await client.post("/v1/products/moment", headers=hd, json=dreq)
                    rd.raise_for_status()
                    resp_file = tmp / f"m{order}.json"
                    resp_file.write_text(rv.text)
                    map_file = tmp / f"m{order}.bin"
                    page = node("vr_tools.mjs", {"op": "moment", "order": order, "depth": n[2], "bbox": st["bbox"],
                                                 "response": str(resp_file), "out": str(map_file), "file": file,
                                                 "grow": masks[which]["req"]})
                    jd = rd.json()
                    mv = np.frombuffer(map_file.read_bytes(), dtype="<f4").reshape(jd["height"], jd["width"])
                    md = np.frombuffer(base64.b64decode(jd["data_base64"]), dtype="<f4").reshape(jd["height"], jd["width"])
                    maps[order] = (mv, page, jd)
                    mom_rows.append({"selection": which, "order": order, "request_equal": sorted(["dataset_id", *page["request"]]) == sorted(dkeys)
                                     and page["request"]["channel_start"] == 0 and page["request"]["channel_end"] == n[2] - 1,
                                     "mask": "off" if not page["request"]["mask_enabled"] else "on",
                                     "identical": nan_equal(mv, md), "unit": jd["moment_unit"], "display_unit": page["display"]["unit"],
                                     "range": page["range"], "box": page["box"], "pixels": int(np.isfinite(mv).sum())})
                m0 = maps[0][0].astype(np.float64)
                m1 = maps[1][0].astype(np.float64)
                # The units as the page shows them (its displayUnit scale), not chosen here.
                scale = maps[0][1]["display"]["scale"]
                if maps[0][1]["display"]["unit"].lower().replace("jy/beam", "").strip() != "km/s":
                    raise RuntimeError(f"M0 unit {maps[0][1]['display']['unit']} is not Jy/beam km/s")
                flux_m0 = float(np.nansum(m0)) / beam_pix * scale
                both = np.isfinite(m0) & np.isfinite(m1)
                v_m1 = float((m0[both] * m1[both]).sum() / m0[both].sum()) * maps[1][1]["display"]["scale"]
                spec = np.asarray([np.nan if v is None else v for v in st["spectrum"]], dtype=float)
                vel = np.asarray([np.nan if v is None else v for v in st["spectrum_velocity_kms"]], dtype=float)
                ok = np.isfinite(spec) & np.isfinite(vel)
                v_spec = float((spec[ok] * vel[ok]).sum() / spec[ok].sum())
                out.setdefault("moment_checks", []).append({
                    "selection": which, "flux_m0": flux_m0, "flux_stats": st["flux_jy_kms"],
                    "v_m1_weighted": v_m1, "v_spectrum_weighted": v_spec, "beam_pixels": beam_pix})
            out["moments"] = mom_rows

            # ── 4. PV across NGC 3187 ──
            reduced = await client.post("/v1/cube/reduced_bin", headers=hd, json={
                "dataset_id": ds, "factor_xy": info["factors"][0], "factor_z": info["factors"][2], "mode": "mean", "encoding": "uint8"})
            reduced.raise_for_status()
            rfile = tmp / "reduced.bin"
            rfile.write_bytes(reduced.content)
            lo, hi = float(reduced.headers["X-Visivo-Range-Min"]), float(reduced.headers["X-Visivo-Range-Max"])
            thr = max(0.0, (float(reduced.headers["X-Visivo-Threshold"]) - lo) / (hi - lo))
            rdims = [int(reduced.headers[k]) for k in ("X-Visivo-Width", "X-Visivo-Height", "X-Visivo-Depth")]
            verts = []
            for dx, dy in ((-22, -10), (0, 0), (22, 10)):
                ray = {"o": [(px0 + dx + 0.5) / n[0], (py0 + dy + 0.5) / n[1], 1.5], "d": [0.0, 0.0, -1.0]}
                v = node("vr_tools.mjs", {"op": "pvVertex", "cube": str(rfile), "dims": rdims, "encoding": "uint8",
                                          "info": info, "ray": ray, "thr": thr})
                verts.append(v["vertex"])
            pv_rows = []
            for label, path, dsid, inf in (("HCG 44", cube, ds, info), ("HCG 44, spectral axis reversed", None, None, None)):
                if path is None:
                    rev = reverse_spectral(cube, scratch / "hcg44_reversed_cdelt3.fits")
                    r2 = await client.post("/v1/datasets/open", json={"path": str(rev)}, headers=hd)
                    r2.raise_for_status()
                    dsid = r2.json()["dataset_id"]
                    inf = (await client.post("/v1/cube/reduced_info", headers=hd, json={"dataset_id": dsid})).json()
                page_req = node("vr_tools.mjs", {"op": "pvRequest", "vertices": verts, "width": 1})["request"]
                rv = await client.post("/v1/cube/pv", headers=hd, json={"dataset_id": dsid, **page_req})
                rv.raise_for_status()
                # The desktop: BackendClient::requestPv's keys (read from its source) with
                # the same vertices and vtkWindowCube's default width (read from its header).
                pv_keys = cpp_json_keys("requestPv")
                desk_req = {"dataset_id": dsid, "vertices": [[float(a), float(b)] for a, b in verts],
                            "width_pixels": desktop_pv_width()}
                assert sorted(desk_req) == sorted(pv_keys), (desk_req.keys(), pv_keys)
                rd = await client.post("/v1/cube/pv", headers=hd, json=desk_req)
                rd.raise_for_status()
                pfile = tmp / "pv.json"
                pfile.write_text(rv.text)
                img_file = tmp / "pv_rows.bin"
                page = node("vr_tools.mjs", {"op": "pv", "vertices": verts, "width": 1, "response": str(pfile), "info": inf, "out": str(img_file),
                                             "rgbaOut": str(tmp / "pv_rgba.bin")})
                jd = rd.json()
                desk_vals = np.frombuffer(qt_uncompress(jd["data_base64"]), dtype="<f4").reshape(jd["depth"], jd["num_samples"])
                spec = np.frombuffer(base64.b64decode(jd["spectral_coordinates_base64"]), dtype="<f8")
                desk_rows = rows_on_uniform_spectral_grid(desk_vals, spec)
                page_rows = np.frombuffer(img_file.read_bytes(), dtype="<f4").reshape(page["depth"], page["samples"])
                # What the page DRAWS: its colourise output, canvas row 0 at the top,
                # against the desktop's rows coloured with the page's table and range.
                rgba = np.frombuffer((tmp / "pv_rgba.bin").read_bytes(), dtype=np.uint8).reshape(page["depth"], page["samples"], 4)
                lut = np.asarray(page["lut"], dtype=np.uint8).reshape(256, 3)
                lo_r, hi_r = page["range"]
                idx = np.clip(np.rint((desk_rows.astype(np.float64) - lo_r) * (255.0 / (hi_r - lo_r))), 0, 255)
                finite = np.isfinite(desk_rows)
                want = np.zeros_like(rgba)
                want[..., :3][finite] = lut[idx[finite].astype(int)]
                want[..., 3][finite] = 255
                drawn_ok = bool(np.array_equal(rgba[::-1], want))
                vel = np.asarray(page["velocities"], dtype=float)
                k = np.unravel_index(np.nanargmax(page_rows), page_rows.shape)
                pv_rows.append({"cube": label, "request_equal": page_req == {k: v for k, v in desk_req.items() if k != "dataset_id"},
                                "drawn_as_desktop_rows": drawn_ok, "desktop_rows": int(desk_rows.shape[0]), "identical_to_desktop_rows": nan_equal(page_rows, desk_rows),
                                "rows_ascending": bool(np.all(np.diff(spec[np.asarray(page["order"])]) > 0)),
                                "velocity_bottom_top": [vel[0], vel[-1]], "peak_velocity_kms": float(vel[k[0]]),
                                "peak_offset": float(k[1]), "samples": page["samples"], "depth": page["depth"],
                                "cdelt3": float(jd["spectral_cdelt"]), "rows": page_rows, "params": page["params"],
                                "origin": page["origin"]})
            out["pv"] = pv_rows
            out["pv_vertices"] = verts
            a, b = pv_rows[0]["rows"], pv_rows[1]["rows"]
            out["pv_reversed_same"] = bool(a.shape == b.shape and np.allclose(np.nan_to_num(a), np.nan_to_num(b), rtol=0, atol=1e-7))
            out["pv_reversed_maxdiff"] = float(np.nanmax(np.abs(a - b)))
            for r in pv_rows:
                r.pop("rows")
    finally:
        await client.aclose()
    return out


def qt_uncompress(b64: str) -> bytes:
    import base64  # noqa: PLC0415
    import zlib  # noqa: PLC0415
    raw = base64.b64decode(b64)
    return zlib.decompress(raw[4:])


def report(o: dict) -> str:
    L = ["# VR analysis = desktop analysis — HCG 44, NGC 3187 (issue #191)", "",
         "Everything runs the app's code: the routes in-process, the page's modules under node "
         "(`tools-lasso.js`, `tools-moment.js`, `tools-pv.js`, `selection.js`). Lasso defaults reach 8, "
         "λv 1, nσ 2.5, σ measured by the service; the desktop's displayed selection is the Reach "
         "slider's level field (reach_max 64) thresholded at the reach.", "",
         "## 1. Lasso seeds (new, add, carve)", "",
         "| press | seed (x, y, ch) | voxels | request = desktop recipe | Dice / desktop shown | Dice / desktop recipe | flux (Jy km/s) |",
         "|---|---|---|---|---|---|---|"]
    for r in o["seeds"]:
        L.append(f"| {r['step']} | ({', '.join(map(str, r['seed_xyz']))}) | {r['voxels']} | {'yes' if r['request_equal'] else 'NO'} "
                 f"| {r['dice_shown']:.4f} | {r['dice_replay']:.4f} | {r['flux']:.3f} |")
    L += ["", "## 2. Isolate and Remove", "",
          "| selection | mode | cube = desktop's | finite voxels | expected | kept = selection |", "|---|---|---|---|---|---|"]
    for r in o["apply"]:
        L.append(f"| {r['selection']} | {r['mode']} | {'yes' if r['identical'] else 'NO'} | {r['finite']} | {r['expected_finite']} "
                 f"| {'yes' if r['kept_voxels_equal'] else 'NO'} |")
    L += ["", "## 3. Moments of the selection", "",
          "*request = desktop*: the page's request has exactly the keys `BackendClient::requestMoment` sends "
          "(read from its C++ source) and the desktop dialog's default channel range; the threshold mask is OFF "
          "(the dialog's default is an automatic ~3σ mask: on an isolated selection the selection is the mask). "
          "*map = desktop's*: the page's decoded map equals the backend's answer to that request on the desktop's "
          "own isolated cube, bit for bit (it checks the page's decoding and that both isolated cubes are the same).", "",
          "| selection | order | request = desktop | mask | map = desktop's | unit (backend) | shown as | colour range (backend unit) | pixels |",
          "|---|---|---|---|---|---|---|---|---|"]
    for r in o["moments"]:
        L.append(f"| {r['selection']} | M{r['order']} | {'yes' if r['request_equal'] else 'NO'} | {r['mask']} | {'yes' if r['identical'] else 'NO'} "
                 f"| {r['unit']} | {r['display_unit']} | {r['range'][0]:.4g} to {r['range'][1]:.4g} | {r['pixels']} |")
    L += ["", "Physics checks, independent of the request: the map's flux and mean velocity from the page's units "
          "(its displayUnit scale) against the selection's own statistics (`mask_stats_bin`).", "",
          "| selection | Σ M0 / beam (Jy km/s) | flux of mask_stats (Jy km/s) | M0-weighted M1 (km/s) | spectrum-weighted velocity (km/s) | beam (pixels) |",
          "|---|---|---|---|---|---|"]
    for r in o["moment_checks"]:
        L.append(f"| {r['selection']} | {r['flux_m0']:.4f} | {r['flux_stats']:.4f} | {r['v_m1_weighted']:.2f} | {r['v_spectrum_weighted']:.2f} | {r['beam_pixels']:.2f} |")
    L += ["", "## 4. PV across NGC 3187", "",
          f"Vertices placed by the page's pick (fractional pixels): {', '.join('(%.1f, %.1f)' % tuple(v) for v in o['pv_vertices'])}; width 1 px.", "",
          "*request = desktop*: the page's request equals `BackendClient::requestPv`'s keys (read from its C++ "
          "source) with the same vertices and vtkWindowCube's default width (read from its header). *rows = desktop's*: "
          "the page's rows equal a line-for-line Python port of the desktop's `rowsOnUniformSpectralGrid` applied to "
          "the backend's answer; *drawn*: the page's own `colourise` output (canvas pixels) equals those desktop rows "
          "coloured with the page's table and range, bottom row at the bottom.", "",
          "| cube | CDELT3 | request = desktop | rows = desktop's rows | drawn = desktop rows | rows ascending | velocity bottom → top (km/s) | peak at (km/s) | samples × channels |",
          "|---|---|---|---|---|---|---|---|---|"]
    for r in o["pv"]:
        vb = r["velocity_bottom_top"]
        L.append(f"| {r['cube']} | {r['cdelt3']:.4g} | {'yes' if r['request_equal'] else 'NO'} | {'yes' if r['identical_to_desktop_rows'] else 'NO'} "
                 f"| {'yes' if r['drawn_as_desktop_rows'] else 'NO'} | {'yes' if r['rows_ascending'] else 'NO'} | {vb[0]:.1f} → {vb[1]:.1f} | {r['peak_velocity_kms']:.1f} | {r['samples']} × {r['depth']} |")
    L += ["", f"Reversed spectral axis: the page's picture is the same row for row: "
          f"{'yes' if o['pv_reversed_same'] else 'NO'} (largest difference {o['pv_reversed_maxdiff']:.3g})."]
    p = o["pv"][0]
    L += ["", f"PV recipe parameters: {json.dumps(p['params'], ensure_ascii=False)}; origin {json.dumps(p['origin'])}."]
    return "\n".join(L) + "\n"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cube", default=os.environ.get("VISIVO_VALIDATION_CUBE", VS.DEFAULT_CUBE))
    ap.add_argument("--sofia-mask", type=Path, default=VS.DEFAULT_SOFIA)
    ap.add_argument("--scratch", type=Path, default=Path(os.environ.get("VISIVO_EXPORTS_DIR", tempfile.gettempdir())))
    args = ap.parse_args()
    if "VISIVO_TOKEN" not in os.environ:
        sys.exit("set VISIVO_TOKEN (importing the app without it writes ~/.visivo_token)")
    if "VISIVO_EXPORTS_DIR" not in os.environ:
        sys.exit("set VISIVO_EXPORTS_DIR to a scratch directory: Isolate and Remove write new cubes there")
    if not Path(args.cube).is_file():
        sys.exit(f"cube not found: {args.cube}")
    args.scratch.mkdir(parents=True, exist_ok=True)
    o = asyncio.run(run(args.cube, args.scratch, args.sofia_mask))
    text = report(o)
    (HERE / "out").mkdir(exist_ok=True)
    (HERE / "out" / "vr_analysis_identity.md").write_text(text)
    print(text)
    bad = [r for r in o["seeds"] if not (r["request_equal"] and r["dice_shown"] == 1.0 and r["dice_replay"] == 1.0)]
    bad += [r for r in o["apply"] if not (r["identical"] and r["kept_voxels_equal"])]
    bad += [r for r in o["moments"] if not (r["request_equal"] and r["identical"])]
    bad += [r for r in o["pv"] if not (r["request_equal"] and r["identical_to_desktop_rows"] and r["rows_ascending"]
                                       and r["drawn_as_desktop_rows"])]
    if not o["pv_reversed_same"]:
        bad.append("pv reversed")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
