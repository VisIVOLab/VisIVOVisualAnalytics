#!/usr/bin/env python
"""A selection made in the headset is the desktop's selection (issue #171).

The VR page picks a seed on the REDUCED cube it draws (a ray to the first
sample above the threshold, the brightest sample of the object the ray enters,
moved to the brightest full-resolution voxel of its reduced voxel's block) and
then asks the backend for the Kinematic Lasso and the spectrum on the FULL
cube, through the routes the desktop uses. This checks, on HCG 44 and
NGC 3187:

1. Identity. For the seed the VR path picks, the mask from the VR request
   (``/v1/cube/kinematic_select_bin`` with the body the page's ``recipe.js``
   builds) equals the desktop's two paths voxel for voxel (Dice 1.0): what the
   desktop shows (``/v1/cube/kinematic_levels_bin`` with reach_max 64,
   thresholded at the reach, as ``KinematicLevelField::thresholdAt`` does) and
   what its recipe replays (``kinematic_select_bin`` with LassoReplay's
   request). The spectrum from the VR request equals the desktop's request on
   ``/v1/spectral/probe`` and the cube's own line of sight.
2. Agreement with the references. Dice of the VR mask against the published
   Serra et al. (2013) mask (its 6-connected component containing the source)
   and our SoFiA-2 run (source 5 = NGC 3187), to be read against the paper's
   ranges for the default click: 0.47-0.69 against the published mask,
   0.66-0.84 against SoFiA-2 (``out/metrics*.md``); and the same at the paper's
   own seed (the SoFiA source's peak), which must reproduce its row.

Everything runs the app's code: the routes in-process (httpx over ASGI), the
page's JavaScript under node (``vr_pick.mjs``).

Run:  VISIVO_TOKEN=x backend/.venv/bin/python validation/vr_selection_identity.py \
          [--cube PATH] [--sofia-mask PATH] [--serra-mask PATH]
"""
from __future__ import annotations

import argparse
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
REPO = HERE.parent
sys.path.insert(0, str(REPO / "backend"))

DEFAULT_CUBE = "/Users/fvitello/downloads/DemoData/hgc44_3axes.fits"
DEFAULT_SOFIA = Path(os.environ.get("VISIVO_SOFIA_OUT", HERE / "sofia_out")) / "sofia_hgc44_mask.fits"
DEFAULT_SERRA = Path("/Users/fvitello/Downloads/DemoData/hcg44_serra2013_binmask.fits")
#: NGC 3187 (NED): 10h17m47.85s +21d52m24.0s, cz about 1580 km/s.
NGC3187_RA, NGC3187_DEC, NGC3187_V = 154.449375, 21.873333, 1580.0
SOFIA_ID = 5                   # SoFiA-2 source 5 = NGC 3187 = row A of the paper
#: The paper's ranges for the default click (Dice), out/metrics*.md.
RANGE_SERRA = (0.47, 0.69)
RANGE_SOFIA = (0.66, 0.84)
#: The desktop's interactive lasso: the Reach slider's level field (budget 64)
#: thresholded at the reach; and its recipe's request (LassoReplay, src/app).
DESKTOP_LEVELS = {"reach": 8.0, "lambda_v": 1.0, "sigma": 0.0, "nsigma": 2.5,
                  "roi_radius": 0, "reach_max": 64.0}
DESKTOP_RECIPE = {"reach": 8.0, "lambda_v": 1.0, "sigma": 0.0, "nsigma": 2.5,
                  "roi_radius": 0, "reach_max": 64.0}


def node_op(request: dict) -> dict:
    node = shutil.which("node")
    if node is None:
        raise RuntimeError("node is not installed: the page's JavaScript needs it")
    r = subprocess.run([node, str(HERE / "vr_pick.mjs")], input=json.dumps(request),
                       capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        raise RuntimeError(r.stderr[-2000:])
    return json.loads(r.stdout)


def dice(a: np.ndarray, b: np.ndarray) -> float:
    s = int(a.sum()) + int(b.sum())
    return 2.0 * int((a & b).sum()) / s if s else 1.0


def full_mask(shape, body: bytes, headers, dtype="<f4") -> np.ndarray:
    w, h, d = (int(headers[k]) for k in ("X-Visivo-Width", "X-Visivo-Height", "X-Visivo-Depth"))
    ox, oy, oz = (int(v) for v in headers["X-Visivo-Origin"].split(","))
    m = np.zeros(shape, dtype=bool)
    m[oz:oz + d, oy:oy + h, ox:ox + w] = np.frombuffer(body, dtype=dtype).reshape(d, h, w) > 0.5
    return m


def levels_mask(shape, body: bytes, headers, reach: float) -> np.ndarray:
    """What the desktop draws from the level field: add <= r minus sub <= r."""
    w, h, d = (int(headers[k]) for k in ("X-Visivo-Width", "X-Visivo-Height", "X-Visivo-Depth"))
    ox, oy, oz = (int(v) for v in headers["X-Visivo-Origin"].split(","))
    unreachable = int(headers["X-Visivo-Peak"])
    n = w * h * d
    raw = np.frombuffer(body, dtype=np.uint8)
    add, sub = raw[:n].reshape(d, h, w), raw[n:2 * n].reshape(d, h, w)
    sel = (add <= reach) & (add != unreachable) & ~((sub <= reach) & (sub != unreachable))
    m = np.zeros(shape, dtype=bool)
    m[oz:oz + d, oy:oy + h, ox:ox + w] = sel
    return m


async def vr_seed(client, headers, ds: str, info: dict, ray: dict, tmp: Path,
                  seed_at: str = "peak") -> dict:
    """The VR page's pick: the reduced cube it receives, its own ray pick, the
    snap to the block's brightest voxel through subvolume_bin."""
    r = await client.post("/v1/cube/reduced_bin", headers=headers,
                          json={"dataset_id": ds, "factor_xy": info["factors"][0],
                                "factor_z": info["factors"][2], "mode": "mean", "encoding": "uint8"})
    r.raise_for_status()
    cube_file = tmp / "reduced.bin"
    cube_file.write_bytes(r.content)
    lo, hi = float(r.headers["X-Visivo-Range-Min"]), float(r.headers["X-Visivo-Range-Max"])
    thr = float(r.headers["X-Visivo-Threshold"])            # the page's default threshold
    thr_norm = max(0.0, (thr - lo) / (hi - lo))
    dims = [int(r.headers[k]) for k in ("X-Visivo-Width", "X-Visivo-Height", "X-Visivo-Depth")]
    pick = node_op({"op": "pick", "cube": str(cube_file), "dims": dims, "encoding": "uint8",
                    "info": info, "ray": ray, "thr": thr_norm, "mode": 0, "seedAt": seed_at})
    if not pick.get("hit"):
        raise RuntimeError("the ray draws nothing")
    (x0, x1), (y0, y1), (z0, z1) = pick["block"]
    sub = await client.post("/v1/cube/subvolume_bin", headers=headers,
                            json={"dataset_id": ds, "x0": x0, "x1": x1, "y0": y0, "y1": y1,
                                  "z0": z0, "z1": z1})
    sub.raise_for_status()
    vfile = tmp / "block.bin"
    vfile.write_bytes(sub.content)
    bdims = [int(sub.headers[k]) for k in ("X-Visivo-Width", "X-Visivo-Height", "X-Visivo-Depth")]
    best = node_op({"op": "brightest", "values": str(vfile), "dims": bdims, "origin": [x0, y0, z0]})
    return {**pick, "seed": best["seed"] or pick["picked"], "threshold": thr, "thr_norm": thr_norm,
            "reduced_dims": dims}


async def selections(client, headers, ds: str, shape, seed_xyz) -> dict:
    """The VR request and the desktop's two paths for one seed, and the spectra."""
    x, y, z = seed_xyz
    reqs = node_op({"op": "request", "seed": [x, y, z]})
    vr_req = {"dataset_id": ds, **reqs["lasso"]}
    desk_recipe = {"dataset_id": ds, "seed_z": z, "seed_y": y, "seed_x": x, **DESKTOP_RECIPE}
    desk_levels = {"dataset_id": ds, "seed_z": z, "seed_y": y, "seed_x": x, **DESKTOP_LEVELS}
    a = await client.post("/v1/cube/kinematic_select_bin", headers=headers, json=vr_req)
    a.raise_for_status()
    c = await client.post("/v1/cube/kinematic_select_bin", headers=headers, json=desk_recipe)
    c.raise_for_status()
    b = await client.post("/v1/cube/kinematic_levels_bin", headers=headers, json=desk_levels)
    b.raise_for_status()
    vr = full_mask(shape, a.content, a.headers)
    desk_shown = levels_mask(shape, b.content, b.headers, DESKTOP_LEVELS["reach"])
    desk_replay = full_mask(shape, c.content, c.headers)
    # The selection statistics of the VR panel, from the grow's own response.
    w, h, d = (int(a.headers[k]) for k in ("X-Visivo-Width", "X-Visivo-Height", "X-Visivo-Depth"))
    ox, oy, oz = (int(v) for v in a.headers["X-Visivo-Origin"].split(","))
    st = await client.post(
        "/v1/cube/mask_stats_bin", headers={**headers, "Content-Type": "application/octet-stream"},
        params={"dataset_id": ds, "origin_x": ox, "origin_y": oy, "origin_z": oz,
                "width": w, "height": h, "depth": d}, content=a.content)
    st.raise_for_status()
    # Spectra: the VR body and the desktop's (BackendClient::spectralProbe).
    sv = await client.post("/v1/spectral/probe", headers=headers, json={"dataset_id": ds, **reqs["probe"]})
    sd = await client.post("/v1/spectral/probe", headers=headers, json={"dataset_id": ds, "x": x, "y": y})
    sv.raise_for_status(); sd.raise_for_status()
    return {"vr": vr, "desk_shown": desk_shown, "desk_replay": desk_replay,
            "vr_request": reqs["lasso"], "requests_equal": reqs["lasso"] == {k: v for k, v in desk_recipe.items() if k != "dataset_id"},
            "count_header": int(a.headers["X-Visivo-Selected-Count"]),
            "stats": st.json(), "spectrum_vr": sv.json(), "spectrum_desktop": sd.json()}


def source_ray(info: dict, cube_path: str, kind: str) -> dict:
    """A ray (box coordinates) through NGC 3187: along the spectral axis from the
    front, or from the page's default desktop camera to the source."""
    from astropy.io import fits  # noqa: PLC0415
    from astropy.wcs import WCS  # noqa: PLC0415
    w = WCS(fits.getheader(cube_path)).celestial
    px, py = (float(v) for v in w.all_world2pix([[NGC3187_RA, NGC3187_DEC]], 0)[0])
    n = info["full_shape"]
    ux, uy = (px + 0.5) / n[0], (py + 0.5) / n[1]
    vel = np.asarray(info["axes"]["z"]["full_velocity_kms"], dtype=float)
    uz = (int(np.nanargmin(np.abs(vel - NGC3187_V))) + 0.5) / n[2]
    if kind == "spectral":
        return {"o": [ux, uy, 1.5], "d": [0.0, 0.0, -1.0], "pixel": [px, py]}
    # The page's desktop camera: yaw -0.5, pitch 0.35, 2 m from the box centre,
    # model box of sides full_shape / max(full_shape).
    yaw, pitch, dist = -0.5, 0.35, 2.0
    cam = np.array([math.sin(yaw) * math.cos(pitch), math.sin(pitch), math.cos(yaw) * math.cos(pitch)]) * dist
    ext = np.asarray(n, dtype=float) / max(n)
    o = cam / ext + 0.5
    target = np.array([ux, uy, uz])
    return {"o": o.tolist(), "d": (target - o).tolist(), "pixel": [px, py]}


async def run(cube: str, sofia_mask: Path | None, serra_mask: Path | None, client=None,
              headers=None) -> list[dict]:
    from astropy.io import fits  # noqa: PLC0415
    from scipy import ndimage  # noqa: PLC0415
    own = client is None
    if own:
        import httpx  # noqa: PLC0415
        from app.main import app  # noqa: PLC0415
        client = httpx.AsyncClient(transport=httpx.ASGITransport(app=app), base_url="http://vr")
        headers = {"X-Visivo-Token": os.environ["VISIVO_TOKEN"]}
    try:
        r = await client.post("/v1/datasets/open", json={"path": cube}, headers=headers)
        r.raise_for_status()
        body = r.json()
        assert body["valid"], body["error"]
        hd = {**headers, "X-Visivo-Session": body["session_id"]}
        ds = body["dataset_id"]
        info = (await client.post("/v1/cube/reduced_info", headers=hd, json={"dataset_id": ds})).json()
        data = np.squeeze(fits.getdata(cube)).astype(np.float32)
        shape = data.shape
        sofia = serra = None
        if sofia_mask and Path(sofia_mask).is_file():
            sofia = np.squeeze(fits.getdata(sofia_mask)).astype(np.int32) == SOFIA_ID
        if serra_mask and Path(serra_mask).is_file():
            serra, _ = ndimage.label(np.squeeze(fits.getdata(serra_mask)) > 0)
        rows = []
        cases = [("VR pick, ray along the spectral axis", "spectral"),
                 ("VR pick, ray from the default view", "oblique"),
                 ("VR pick, default view, seed at the first drawn sample", "oblique-hit")]
        if sofia is not None:
            cases.append(("the paper's seed (SoFiA-2 source peak)", "paper"))
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            for label, kind in cases:
                if kind == "paper":
                    zz, yy, xx = np.where(sofia)
                    k = int(np.nanargmax(data[zz, yy, xx]))
                    seed = [int(xx[k]), int(yy[k]), int(zz[k])]
                    pick = {"seed": seed}
                else:
                    ray_kind, _, at = kind.partition("-")
                    pick = await vr_seed(client, hd, ds, info, source_ray(info, cube, ray_kind), tmp,
                                         seed_at=at or "peak")
                    seed = pick["seed"]
                s = await selections(client, hd, ds, shape, seed)
                vr = s["vr"]
                row = {"case": label, "seed_xyz": seed, "picked_xyz": pick.get("picked"),
                       "first_hit_xyz": pick.get("firstHitPixel"), "reduced": pick.get("reduced"),
                       "voxels": int(vr.sum()), "count_header": s["count_header"],
                       "dice_desktop_shown": dice(vr, s["desk_shown"]),
                       "dice_desktop_replay": dice(vr, s["desk_replay"]),
                       "requests_equal": s["requests_equal"],
                       "flux_jy_kms": s["stats"]["flux_jy_kms"],
                       "velocity_kms": [s["stats"]["velocity_min_kms"], s["stats"]["velocity_max_kms"]]}
                sv, sd = s["spectrum_vr"], s["spectrum_desktop"]
                los = data[:, seed[1], seed[0]].astype(float)
                cube_vals = [None if not math.isfinite(v) else v for v in los]
                row["spectrum_equal_desktop"] = sv == sd
                row["spectrum_equal_cube"] = all(
                    (a is None and b is None) or (a is not None and b is not None and abs(a - b) <= 1e-7 * max(1.0, abs(b)))
                    for a, b in zip(sv["values"], cube_vals)) and len(sv["values"]) == len(cube_vals)
                if sofia is not None:
                    row["dice_sofia2"] = dice(vr, sofia)
                if serra is not None:
                    lab = int(np.bincount(serra[vr]).argmax()) if vr.any() else 0
                    if lab == 0:
                        nz = serra[vr]
                        lab = int(np.bincount(nz[nz > 0]).argmax()) if (nz > 0).any() else 0
                    row["serra_component"] = lab
                    row["dice_serra2013"] = dice(vr, serra == lab) if lab else 0.0
                rows.append(row)
        return rows
    finally:
        if own:
            await client.aclose()


def report(rows: list[dict]) -> str:
    out = ["# VR selection = desktop selection — HCG 44, NGC 3187 (issue #171)", "",
           "The VR path: the reduced cube of `reduced_bin` (auto binning, mean, 8 bits, the page's "
           "default threshold), the page's own ray pick and seed (`selection.js`, run by node), "
           "the request the page builds (`recipe.js`). The desktop: the Reach slider's level "
           "field (`kinematic_levels_bin`, reach_max 64) thresholded at reach 8, and its recipe's "
           "`kinematic_select_bin` request. Lasso defaults reach 8, λv 1, nσ 2.5, σ measured by "
           "the service. Paper ranges for the default click: Dice "
           f"{RANGE_SERRA[0]}–{RANGE_SERRA[1]} against Serra et al. (2013), "
           f"{RANGE_SOFIA[0]}–{RANGE_SOFIA[1]} against SoFiA-2.", "",
           "| case | seed (x, y, ch) | voxels | Dice VR / desktop shown | Dice VR / desktop recipe "
           "| spectrum = desktop | spectrum = cube | Dice vs Serra+2013 | Dice vs SoFiA-2 | flux (Jy km/s) "
           "| velocity (km/s) |",
           "|---|---|---|---|---|---|---|---|---|---|---|"]
    for r in rows:
        v = r["velocity_kms"]
        out.append(
            f"| {r['case']} | ({', '.join(map(str, r['seed_xyz']))}) | {r['voxels']} "
            f"| {r['dice_desktop_shown']:.4f} | {r['dice_desktop_replay']:.4f} "
            f"| {'yes' if r['spectrum_equal_desktop'] else 'NO'} | {'yes' if r['spectrum_equal_cube'] else 'NO'} "
            f"| {r.get('dice_serra2013', float('nan')):.3f} (component {r.get('serra_component', '-')}) "
            f"| {r.get('dice_sofia2', float('nan')):.3f} | {r['flux_jy_kms']:.3f} "
            f"| {v[0]:.1f} to {v[1]:.1f} |")
    out.append("")
    for r in rows:
        if r.get("picked_xyz"):
            out.append(f"- {r['case']}: the ray's first drawn sample is in pixel "
                       f"({', '.join(map(str, r['first_hit_xyz']))}); the seed's reduced voxel is "
                       f"({', '.join(map(str, r['reduced']))}) (from pixel {', '.join(map(str, r['picked_xyz']))}), "
                       f"the seed its block's brightest voxel.")
    return "\n".join(out) + "\n"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cube", default=os.environ.get("VISIVO_VALIDATION_CUBE", DEFAULT_CUBE))
    ap.add_argument("--sofia-mask", type=Path, default=DEFAULT_SOFIA)
    ap.add_argument("--serra-mask", type=Path, default=DEFAULT_SERRA)
    args = ap.parse_args()
    if "VISIVO_TOKEN" not in os.environ:
        sys.exit("set VISIVO_TOKEN (importing the app without it writes ~/.visivo_token)")
    if not Path(args.cube).is_file():
        sys.exit(f"cube not found: {args.cube} (set VISIVO_VALIDATION_CUBE or --cube)")
    rows = asyncio.run(run(args.cube, args.sofia_mask, args.serra_mask))
    text = report(rows)
    (HERE / "out").mkdir(exist_ok=True)
    (HERE / "out" / "vr_selection_identity.md").write_text(text)
    print(text)
    bad = [r for r in rows if r["dice_desktop_shown"] != 1.0 or r["dice_desktop_replay"] != 1.0
           or not r["spectrum_equal_desktop"] or not r["spectrum_equal_cube"]]
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
