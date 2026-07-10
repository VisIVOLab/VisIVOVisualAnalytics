#!/usr/bin/env python3
"""
benchmark_visivo.py - reproducible responsiveness benchmark for VisIVO Next.

Fills Table 2 of the paper: for each cube it measures
  1. preview interactive      POST /v1/cube/preview   (downsample=4)
  2. full resolution ready    POST /v1/cube/subvolume_bin  (whole cube, streamed)
     + camera-ROI variant     POST /v1/cube/subvolume_bin  (central half-box)
  3. lasso grow               POST /v1/cube/kinematic_select_bin
  4. masked M0 (auto 3-sigma) POST /v1/products/moment
  5. PV extraction            POST /v1/cube/pv

Each operation is repeated --repeats times with slightly varied parameters so
the backend product cache (keyed on dataset+params) cannot serve a cached
result; the script reports median and MAD-based spread.

Usage (backend already running, token in ~/.visivo_token):

    python3 benchmark_visivo.py --cube /data/cube_1gb.fits \
                                --cube /data/cube_8gb.fits \
                                --cube /data/cube_30gb.fits \
                                --repeats 3 --out results

Options:
    --base-url URL     backend base URL          (default http://127.0.0.1:8000)
    --token TOKEN      X-Visivo-Token            (default: read ~/.visivo_token)
    --seed Z,Y,X       lasso seed for ALL cubes  (default: auto from brightest
                       pixel of the central channel; always correct for cubes
                       made with make_synthetic_cube.py)
    --roi-only-above GB  fetch full resolution only up to this size; above it,
                       time the camera-ROI fetch instead (default 10)
    --skip OP[,OP]     skip operations (preview,fullres,lasso,m0,pv)

Outputs in --out directory:
    results.json       raw timings + environment record
    results.md         human-readable table
    table2_rows.tex    LaTeX rows ready to paste into Table 2
"""
from __future__ import annotations

import argparse
import datetime
import json
import platform
import statistics
import sys
import time
from pathlib import Path

import numpy as np

try:
    import requests
except ImportError:
    sys.exit("pip install requests")


# --------------------------------------------------------------------------
def log(msg: str) -> None:
    print(f"[{datetime.datetime.now():%H:%M:%S}] {msg}", flush=True)


class Client:
    def __init__(self, base_url: str, token: str):
        self.base = base_url.rstrip("/")
        self.s = requests.Session()
        self.s.headers["X-Visivo-Token"] = token
        self.session_id: str | None = None

    def post_json(self, path: str, payload: dict, timeout: int = 36000) -> dict:
        h = {}
        if self.session_id:
            h["X-Visivo-Session"] = self.session_id
        r = self.s.post(self.base + path, json=payload, headers=h, timeout=timeout)
        r.raise_for_status()
        return r.json()

    def post_stream(self, path: str, payload: dict, timeout: int = 36000):
        """POST returning a streamed binary body; consumes it fully and
        returns (n_bytes, headers). Consuming the stream is part of the
        measured time: 'ready' means the client HAS the voxels."""
        h = {}
        if self.session_id:
            h["X-Visivo-Session"] = self.session_id
        with self.s.post(self.base + path, json=payload, headers=h,
                         timeout=timeout, stream=True) as r:
            r.raise_for_status()
            n = 0
            for chunk in r.iter_content(chunk_size=1 << 22):
                n += len(chunk)
            return n, dict(r.headers)


def timed(fn, *a, **kw):
    t0 = time.perf_counter()
    out = fn(*a, **kw)
    return time.perf_counter() - t0, out


def med_mad(xs: list[float]) -> tuple[float, float]:
    m = statistics.median(xs)
    mad = statistics.median([abs(x - m) for x in xs]) if len(xs) > 1 else 0.0
    return m, 1.4826 * mad


# --------------------------------------------------------------------------
def find_seed(cli: Client, ds: str, w: int, h: int, d: int) -> tuple[int, int, int]:
    """Auto seed: brightest pixel of the central channel (fetched as a
    1-channel slab through subvolume_bin)."""
    zc = d // 2
    payload = dict(dataset_id=ds, x0=0, x1=w - 1, y0=0, y1=h - 1, z0=zc, z1=zc)
    h_ = {"X-Visivo-Session": cli.session_id}
    r = cli.s.post(cli.base + "/v1/cube/subvolume_bin", json=payload,
                   headers=h_, timeout=36000)
    r.raise_for_status()
    arr = np.frombuffer(r.content, dtype="<f4").reshape(h, w)
    arr = np.nan_to_num(arr, nan=-np.inf)
    y, x = np.unravel_index(np.argmax(arr), arr.shape)
    return zc, int(y), int(x)


def bench_cube(cli: Client, cube: str, repeats: int, seed, roi_only_above_gb: float,
               skip: set[str]) -> dict:
    rec: dict = {"cube": cube, "ops": {}}
    log(f"=== {cube}")

    # -- open (once; not a Table-2 row but recorded) -----------------------
    t_open, resp = timed(cli.post_json, "/v1/datasets/open", {"path": cube})
    if not resp.get("valid", False):
        raise RuntimeError(f"open failed: {resp.get('error')}")
    ds = resp["dataset_id"]
    cli.session_id = resp["session_id"]
    w, h, d = resp["width"], resp["height"], resp["depth"]
    vox = w * h * d
    size_gb = vox * 4 / 1e9
    rec.update(width=w, height=h, depth=d, size_gb_f32=round(size_gb, 2),
               open_s=round(t_open, 3))
    log(f"    open {t_open:.2f}s  dims {w}x{h}x{d}  ({size_gb:.1f} GB float32)")

    # -- 1. preview --------------------------------------------------------
    if "preview" not in skip:
        ts = []
        for _ in range(repeats):
            t, r = timed(cli.post_json, "/v1/cube/preview",
                         {"dataset_id": ds, "downsample": 4})
            assert r.get("valid", True), r.get("error")
            ts.append(t)
        rec["ops"]["preview"] = ts
        log(f"    preview        {med_mad(ts)[0]:.2f}s")

    # -- 2. full resolution / camera ROI -----------------------------------
    if "fullres" not in skip:
        if size_gb <= roi_only_above_gb:
            ts = []
            for _ in range(repeats):
                t, (n, _) = timed(cli.post_stream, "/v1/cube/subvolume_bin",
                                  dict(dataset_id=ds, x0=0, x1=w - 1,
                                       y0=0, y1=h - 1, z0=0, z1=d - 1))
                ts.append(t)
            rec["ops"]["fullres"] = ts
            log(f"    full-res       {med_mad(ts)[0]:.2f}s  ({n/1e9:.1f} GB)")
        # camera ROI: central half-box (1/8 of the voxels), always measured
        ts = []
        for _ in range(repeats):
            t, (n, _) = timed(cli.post_stream, "/v1/cube/subvolume_bin",
                              dict(dataset_id=ds,
                                   x0=w // 4, x1=w // 4 + w // 2 - 1,
                                   y0=h // 4, y1=h // 4 + h // 2 - 1,
                                   z0=d // 4, z1=d // 4 + d // 2 - 1))
            ts.append(t)
        rec["ops"]["roi"] = ts
        log(f"    camera ROI     {med_mad(ts)[0]:.2f}s  ({n/1e9:.2f} GB)")

    # -- seed for the lasso --------------------------------------------------
    if "lasso" not in skip:
        if seed is None:
            t_seed, seed = timed(find_seed, cli, ds, w, h, d)
            log(f"    seed auto      z,y,x = {seed}  (found in {t_seed:.1f}s)")
        rec["seed_zyx"] = list(seed)
        ts, counts = [], []
        for r_i in range(repeats):
            t, resp2 = timed(
                cli.s.post, cli.base + "/v1/cube/kinematic_select_bin",
                json=dict(dataset_id=ds, seed_z=seed[0], seed_y=seed[1],
                          seed_x=seed[2],
                          # vary reach epsilon to defeat the product cache
                          reach=8.0 + 0.001 * r_i, lambda_v=0.7),
                headers={"X-Visivo-Session": cli.session_id}, timeout=36000)
            if resp2.status_code != 200:
                raise RuntimeError(f"lasso: HTTP {resp2.status_code}: "
                                   f"{resp2.text[:200]}")
            ts.append(t)
            counts.append(int(resp2.headers.get("X-Visivo-Selected-Count", 0)))
        rec["ops"]["lasso"] = ts
        rec["lasso_selected_voxels"] = counts[0]
        log(f"    lasso grow     {med_mad(ts)[0]:.2f}s  "
            f"({counts[0]} voxels selected)")

    # -- 4. masked M0 (auto 3-sigma threshold) -------------------------------
    if "m0" not in skip:
        ts = []
        for r_i in range(repeats):
            t, r = timed(cli.post_json, "/v1/products/moment",
                         dict(dataset_id=ds, moment_order=0,
                              # vary the start channel to defeat the cache
                              channel_start=r_i, channel_end=d - 1,
                              mask_enabled=True, threshold_auto=True))
            assert r.get("valid", True), r.get("error")
            ts.append(t)
        rec["ops"]["m0"] = ts
        log(f"    masked M0      {med_mad(ts)[0]:.2f}s")

    # -- 5. PV extraction ----------------------------------------------------
    if "pv" not in skip:
        ts = []
        for r_i in range(repeats):
            verts = [[w // 8 + r_i, h // 8], [w // 2, h // 2],
                     [7 * w // 8, 7 * h // 8]]
            t, r = timed(cli.post_json, "/v1/cube/pv",
                         dict(dataset_id=ds, vertices=verts, width_pixels=3))
            assert r.get("valid", True), r.get("error")
            ts.append(t)
        rec["ops"]["pv"] = ts
        log(f"    PV extraction  {med_mad(ts)[0]:.2f}s")

    cli.session_id = None  # next cube opens its own session
    return rec


# --------------------------------------------------------------------------
ROWS = [("preview", "Preview interactive"),
        ("fullres", "Full resolution ready"),
        ("roi", "Camera ROI ready"),
        ("lasso", "Lasso grow (typical object)"),
        ("m0", "Masked $M_0$"),
        ("pv", "PV extraction")]


def fmt(cell: list[float] | None) -> str:
    if not cell:
        return "n/a"
    m, s = med_mad(cell)
    return f"{m:.1f} $\\pm$ {s:.1f}" if s else f"{m:.1f}"


def write_outputs(records: list[dict], out: Path, env: dict) -> None:
    out.mkdir(parents=True, exist_ok=True)
    (out / "results.json").write_text(
        json.dumps({"environment": env, "records": records}, indent=2))
    # Markdown
    heads = [f"{r['size_gb_f32']} GB" for r in records]
    md = ["| operation | " + " | ".join(heads) + " |",
          "|---" * (len(records) + 1) + "|"]
    for key, label in ROWS:
        md.append("| " + label.replace("$M_0$", "M0").replace("$\\pm$", "±")
                  + " | " + " | ".join(
                      fmt(r["ops"].get(key)).replace("$\\pm$", "±")
                      for r in records) + " |")
    (out / "results.md").write_text("\n".join(md) + "\n")
    # LaTeX rows for Table 2 (median values, seconds)
    tex = [f"% generated {env['timestamp']} on {env['platform']}"]
    for key, label in ROWS:
        cells = " & ".join(fmt(r["ops"].get(key)) + ("\\,s" if r["ops"].get(key) else "")
                           for r in records)
        tex.append(f"{label} & {cells} \\\\")
    (out / "table2_rows.tex").write_text("\n".join(tex) + "\n")
    log(f"written: {out}/results.json  results.md  table2_rows.tex")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cube", action="append", required=True,
                    help="path to a FITS cube ON THE BACKEND HOST (repeatable)")
    ap.add_argument("--base-url", default="http://127.0.0.1:8000")
    ap.add_argument("--token", default=None)
    ap.add_argument("--repeats", type=int, default=3)
    ap.add_argument("--seed", default=None, help="Z,Y,X lasso seed")
    ap.add_argument("--roi-only-above", type=float, default=10.0, metavar="GB")
    ap.add_argument("--skip", default="", help="comma list: preview,fullres,lasso,m0,pv")
    ap.add_argument("--out", default="benchmark_results")
    args = ap.parse_args()

    token = args.token or (Path.home() / ".visivo_token").read_text().strip()
    seed = tuple(int(v) for v in args.seed.split(",")) if args.seed else None
    skip = set(filter(None, args.skip.split(",")))

    cli = Client(args.base_url, token)
    env = {"timestamp": datetime.datetime.now().isoformat(timespec="seconds"),
           "base_url": args.base_url, "repeats": args.repeats,
           "platform": platform.platform(), "machine": platform.machine(),
           "python": platform.python_version(),
           "note": "add CPU/RAM/GPU/disk of the backend host here"}

    records = [bench_cube(cli, c, args.repeats, seed,
                          args.roi_only_above, skip) for c in args.cube]
    write_outputs(records, Path(args.out), env)


if __name__ == "__main__":
    main()
