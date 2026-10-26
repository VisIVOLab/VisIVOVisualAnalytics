#!/usr/bin/env python3
"""
benchmark_visivo.py - reproducible responsiveness benchmark for VisIVO Visual Analytics.

Fills Table 2 of the paper: for each cube it measures
  1. preview interactive      POST /v1/cube/preview   (downsample=4)
  2. full resolution ready    POST /v1/cube/subvolume_bin  (whole cube, streamed)
     + camera-ROI variant     POST /v1/cube/subvolume_bin  (central half-box)
  3. lasso grow               POST /v1/cube/kinematic_select_bin
  4. masked M0 (auto 3-sigma) POST /v1/products/moment
  5. PV extraction            POST /v1/cube/pv
  6. responsiveness under load: latency of single-channel slices
                              (POST /v1/cube/slice) while idle and while a
                              masked M0 runs on the same service

For every operation it also records the bytes the service sent back (what a
remote client would pull over the network) and, with --backend-pid or
--mem-cmd, the service's peak memory while the operation ran.

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
    --skip OP[,OP]     skip operations (preview,fullres,lasso,m0,pv,load)
    --backend-pid PID  sample the resident memory of this process and its
                       children (local backend)
    --mem-cmd CMD      shell command printing the service's current memory in
                       bytes (e.g. a container's cgroup memory.current)
    --note TEXT        free text stored with the results (host, network, ...)

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
import subprocess
import sys
import threading
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
        self.last_bytes = 0   # body bytes of the last response (network cost)

    def post_json(self, path: str, payload: dict, timeout: int = 36000) -> dict:
        h = {}
        if self.session_id:
            h["X-Visivo-Session"] = self.session_id
        r = self.s.post(self.base + path, json=payload, headers=h, timeout=timeout)
        r.raise_for_status()
        self.last_bytes = len(r.content)
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
            self.last_bytes = n
            return n, dict(r.headers)


def timed(fn, *a, **kw):
    t0 = time.perf_counter()
    out = fn(*a, **kw)
    return time.perf_counter() - t0, out


def med_mad(xs: list[float]) -> tuple[float, float]:
    m = statistics.median(xs)
    mad = statistics.median([abs(x - m) for x in xs]) if len(xs) > 1 else 0.0
    return m, 1.4826 * mad


class MemSampler:
    """Peak memory of the service while an operation runs.

    Sampled every 0.2 s from --backend-pid (resident set of the process and its
    descendants, via ps) or from --mem-cmd (any command printing bytes, e.g. a
    container's cgroup memory.current, which also counts page cache). Disabled
    when neither is given; peaks are then reported as null.
    """

    def __init__(self, pid: int | None, cmd: str | None):
        self.pid, self.cmd = pid, cmd
        self.enabled = pid is not None or bool(cmd)
        self._peak = 0
        self._samples = 0
        self._errors = 0
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    def _read(self) -> int:
        if self.cmd:
            out = subprocess.run(self.cmd, shell=True, capture_output=True, text=True,
                                 timeout=5)
            if out.returncode != 0 or not out.stdout.strip():
                raise RuntimeError(f"memory command failed: {out.stderr.strip()[:200]}")
            return int(out.stdout.strip())
        rows = subprocess.run(["ps", "-A", "-o", "pid=,ppid=,rss="], capture_output=True,
                              text=True, timeout=5, check=True).stdout.split()
        procs = {int(rows[i]): (int(rows[i + 1]), int(rows[i + 2]))
                 for i in range(0, len(rows) - 2, 3)}
        if self.pid not in procs:
            raise RuntimeError(f"process {self.pid} is not running")
        tree, frontier = {self.pid}, [self.pid]
        while frontier:
            parent = frontier.pop()
            for pid, (ppid, _) in procs.items():
                if ppid == parent and pid not in tree:
                    tree.add(pid)
                    frontier.append(pid)
        return sum(procs[p][1] for p in tree if p in procs) * 1024

    def _loop(self):
        while not self._stop.is_set():
            try:
                self._peak = max(self._peak, self._read())
                self._samples += 1
            except Exception:                     # noqa: BLE001 - counted, reported below
                self._errors += 1
            self._stop.wait(0.2)

    def __enter__(self):
        if self.enabled:
            self._peak = 0
            self._samples = 0
            self._errors = 0
            self._stop.clear()
            self._thread = threading.Thread(target=self._loop, daemon=True)
            self._thread.start()
        return self

    def __exit__(self, *exc):
        if self._thread:
            self._stop.set()
            self._thread.join()
        return False

    @property
    def peak_gb(self) -> float | None:
        """Null when sampling is off or no sample succeeded: a failed read must
        never be reported as zero memory."""
        if not self.enabled or self._samples == 0:
            return None
        return round(self._peak / 1e9, 2)


MEM = MemSampler(None, None)   # replaced in main()


def note_op(rec: dict, key: str, mem: MemSampler, nbytes: int) -> None:
    """Store the peak memory and the bytes sent back for one operation."""
    rec.setdefault("peak_mem_gb", {})[key] = mem.peak_gb
    rec.setdefault("bytes_out", {})[key] = int(nbytes)


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


class OpFailed(Exception):
    """One measured operation did not complete; the campaign continues."""


def attempt(rec: dict, label: str, fn):
    """Run one measured operation, recording n/a instead of aborting on failure.

    A campaign that dies on its first unsupported combination reports nothing
    about the cubes after it — and the failure most likely to happen here is not
    a defect in the software under test but a documented limit of the measuring
    path: /v1/cube/subvolume_bin answers with a single response body, so a
    multi-GB full-resolution fetch breaks the connection (IncompleteRead) where
    the desktop client would have fetched parallel slabs. That belongs in the
    table as n/a with its reason recorded, not as a missing 30 GB column.
    """
    try:
        return fn()
    except Exception as exc:                      # noqa: BLE001 - any failure is an n/a cell
        reason = f"{type(exc).__name__}: {exc}"
        rec.setdefault("failed_ops", {})[label] = reason
        log(f"    {label:<14} n/a  ({reason[:120]})")
        return None


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
        def _preview():
            ts = []
            with MEM:
                for _ in range(repeats):
                    t, r = timed(cli.post_json, "/v1/cube/preview",
                                 {"dataset_id": ds, "downsample": 4})
                    if not r.get("valid", True):
                        raise OpFailed(str(r.get("error")))
                    ts.append(t)
            note_op(rec, "preview", MEM, cli.last_bytes)
            rec["ops"]["preview"] = ts
            log(f"    preview        {med_mad(ts)[0]:.2f}s")
        attempt(rec, "preview", _preview)

    # -- 2. full resolution / camera ROI -----------------------------------
    if "fullres" not in skip:
        if size_gb <= roi_only_above_gb:
            def _fullres():
                ts = []
                with MEM:
                    for _ in range(repeats):
                        t, (n, _) = timed(cli.post_stream, "/v1/cube/subvolume_bin",
                                          dict(dataset_id=ds, x0=0, x1=w - 1,
                                               y0=0, y1=h - 1, z0=0, z1=d - 1))
                        ts.append(t)
                note_op(rec, "fullres", MEM, n)
                rec["ops"]["fullres"] = ts
                log(f"    full-res       {med_mad(ts)[0]:.2f}s  ({n/1e9:.1f} GB)")
            attempt(rec, "full-res", _fullres)
        # camera ROI: a CENTRED box bounded by the viewport, not a fixed fraction
        # of the cube — the desktop fetches only what the camera frames (and in
        # parallel slabs). Cap it to ROI_CAP_VOX (~1 GB f32) so the single-request
        # measurement is realistic and comparable across cube sizes; a 1/8 box of a
        # 30 GB cube (3.7 GB in one blob) is neither.
        ROI_CAP_VOX = 268_000_000
        side = int(round(ROI_CAP_VOX ** (1.0 / 3.0)))
        bx, by, bz = min(w, side), min(h, side), min(d, side)
        x0c, y0c, z0c = (w - bx) // 2, (h - by) // 2, (d - bz) // 2
        def _roi():
            ts = []
            with MEM:
                for _ in range(repeats):
                    t, (n, _) = timed(cli.post_stream, "/v1/cube/subvolume_bin",
                                      dict(dataset_id=ds,
                                           x0=x0c, x1=x0c + bx - 1,
                                           y0=y0c, y1=y0c + by - 1,
                                           z0=z0c, z1=z0c + bz - 1))
                    ts.append(t)
            note_op(rec, "roi", MEM, n)
            rec["ops"]["roi"] = ts
            rec["roi_gb"] = round(n / 1e9, 2)
            log(f"    camera ROI     {med_mad(ts)[0]:.2f}s  ({n/1e9:.2f} GB)")
        attempt(rec, "camera ROI", _roi)

    # -- seed for the lasso --------------------------------------------------
    if "lasso" not in skip:
        if seed is None:
            t_seed, seed = timed(find_seed, cli, ds, w, h, d)
            log(f"    seed auto      z,y,x = {seed}  (found in {t_seed:.1f}s)")
        rec["seed_zyx"] = list(seed)
        ts, counts = [], []
        MEM.__enter__()
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
        MEM.__exit__(None, None, None)
        note_op(rec, "lasso", MEM, len(resp2.content))
        rec["ops"]["lasso"] = ts
        rec["lasso_selected_voxels"] = counts[0]
        log(f"    lasso grow     {med_mad(ts)[0]:.2f}s  "
            f"({counts[0]} voxels selected)")

    # -- 4. masked M0 (auto 3-sigma threshold) -------------------------------
    if "m0" not in skip:
        def _m0():
            ts = []
            with MEM:
                for r_i in range(repeats):
                    t, r = timed(cli.post_json, "/v1/products/moment",
                                 dict(dataset_id=ds, moment_order=0,
                                      # vary the start channel to defeat the cache
                                      channel_start=r_i, channel_end=d - 1,
                                      mask_enabled=True, threshold_auto=True))
                    if not r.get("valid", True):
                        raise OpFailed(str(r.get("error")))
                    ts.append(t)
            note_op(rec, "m0", MEM, cli.last_bytes)
            rec["ops"]["m0"] = ts
            log(f"    masked M0      {med_mad(ts)[0]:.2f}s")
        attempt(rec, "masked M0", _m0)

    # -- 5. PV extraction ----------------------------------------------------
    if "pv" not in skip:
        def _pv():
            ts = []
            with MEM:
                for r_i in range(repeats):
                    verts = [[w // 8 + r_i, h // 8], [w // 2, h // 2],
                             [7 * w // 8, 7 * h // 8]]
                    t, r = timed(cli.post_json, "/v1/cube/pv",
                                 dict(dataset_id=ds, vertices=verts, width_pixels=3))
                    if not r.get("valid", True):
                        raise OpFailed(str(r.get("error")))
                    ts.append(t)
            note_op(rec, "pv", MEM, cli.last_bytes)
            rec["ops"]["pv"] = ts
            log(f"    PV extraction  {med_mad(ts)[0]:.2f}s")
        attempt(rec, "PV extraction", _pv)

    # -- 6. responsiveness under load ---------------------------------------
    if "load" not in skip:
        def _load():
            # Channels never requested before, so no slice comes from a cache.
            order = list(np.random.default_rng(7).permutation(d))
            def slice_once() -> float:
                z = int(order.pop())
                t, r = timed(cli.post_json, "/v1/cube/slice",
                             dict(dataset_id=ds, axis="z", index=z))
                if not r.get("valid", True):
                    raise OpFailed(str(r.get("error")))
                return t
            idle = [slice_once() for _ in range(min(10, len(order) // 3))]
            slice_bytes = cli.last_bytes
            # A masked M0 the cache has not seen (start channel past those of
            # the M0 row), run on its own client so it shares only the service.
            heavy = Client(cli.base, cli.s.headers["X-Visivo-Token"])
            heavy.session_id = cli.session_id
            box: dict = {}
            def run_m0():
                box["start"] = time.perf_counter()
                try:
                    r = heavy.post_json("/v1/products/moment",
                                        dict(dataset_id=ds, moment_order=0,
                                             channel_start=repeats + 3, channel_end=d - 1,
                                             mask_enabled=True, threshold_auto=True))
                    if not r.get("valid", True):
                        box["error"] = str(r.get("error"))
                except Exception as exc:          # noqa: BLE001 - reported below
                    box["error"] = f"{type(exc).__name__}: {exc}"
                finally:
                    box["end"] = time.perf_counter()
            th = threading.Thread(target=run_m0)
            th.start()
            spans = []                            # (start, end) of each slice request
            try:
                time.sleep(0.5)                   # let the job reach the workers
                while th.is_alive() and len(order) > 1:
                    t0 = time.perf_counter()
                    slice_once()
                    spans.append((t0, time.perf_counter()))
                    time.sleep(0.2)
            finally:
                th.join()
            if "error" in box:
                raise OpFailed(f"the M0 under the probe failed: {box['error']}")
            # Only slices that started AND finished while the M0 was running
            # count as served during it; one that outlived it is reported apart.
            busy = [e - s0 for s0, e in spans if s0 >= box["start"] and e <= box["end"]]
            straddling = [e - s0 for s0, e in spans if e > box["end"]]
            rec["load"] = {"slice_idle_s": idle, "slice_busy_s": busy,
                           "slice_straddling_s": straddling,
                           "m0_under_probe_s": box["end"] - box["start"],
                           "slice_bytes": slice_bytes}
            box["t"] = box["end"] - box["start"]
            log(f"    slice idle     {med_mad(idle)[0]:.3f}s   busy "
                f"{(med_mad(busy)[0] if busy else float('nan')):.3f}s "
                f"(n={len(busy)}, max {max(busy) if busy else float('nan'):.3f}s) "
                f"while M0 ran {box.get('t', float('nan')):.1f}s")
        attempt(rec, "load probe", _load)

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
    # Memory, network cost and responsiveness under load, per cube.
    md += ["", "| per cube | " + " | ".join(heads) + " |", "|---" * (len(records) + 1) + "|"]
    for key, label in ROWS:
        md.append(f"| peak memory, {label.replace('$M_0$', 'M0')} (GB) | " + " | ".join(
            str((r.get("peak_mem_gb") or {}).get(key, "n/a")) for r in records) + " |")
    for key, label in ROWS:
        md.append(f"| bytes sent, {label.replace('$M_0$', 'M0')} (MB) | " + " | ".join(
            (f"{(r.get('bytes_out') or {})[key] / 1e6:.1f}"
             if key in (r.get("bytes_out") or {}) else "n/a") for r in records) + " |")
    def _q(xs, q):
        return f"{float(np.quantile(xs, q)):.3f}" if xs else "n/a"
    md.append("| slice latency idle, median (s) | " + " | ".join(
        _q((r.get("load") or {}).get("slice_idle_s"), 0.5) for r in records) + " |")
    md.append("| slice latency during M0, median (s) | " + " | ".join(
        _q((r.get("load") or {}).get("slice_busy_s"), 0.5) for r in records) + " |")
    md.append("| slice latency during M0, max (s) | " + " | ".join(
        _q((r.get("load") or {}).get("slice_busy_s"), 1.0) for r in records) + " |")
    md.append("| slices served during M0 | " + " | ".join(
        str(len((r.get("load") or {}).get("slice_busy_s") or [])) for r in records) + " |")
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
    ap.add_argument("--skip", default="", help="comma list: preview,fullres,lasso,m0,pv,load")
    ap.add_argument("--backend-pid", type=int, default=None)
    ap.add_argument("--mem-cmd", default=None)
    ap.add_argument("--note", default="")
    ap.add_argument("--out", default="benchmark_results")
    args = ap.parse_args()

    token = args.token or (Path.home() / ".visivo_token").read_text().strip()
    seed = tuple(int(v) for v in args.seed.split(",")) if args.seed else None
    skip = set(filter(None, args.skip.split(",")))

    global MEM
    MEM = MemSampler(args.backend_pid, args.mem_cmd)
    cli = Client(args.base_url, token)
    env = {"timestamp": datetime.datetime.now().isoformat(timespec="seconds"),
           "base_url": args.base_url, "repeats": args.repeats,
           "platform": platform.platform(), "machine": platform.machine(),
           "python": platform.python_version(),
           "note": args.note or "add CPU/RAM/GPU/disk of the backend host here",
           "memory_source": ("pid tree RSS" if args.backend_pid else
                             args.mem_cmd or None)}

    records = [bench_cube(cli, c, args.repeats, seed,
                          args.roi_only_above, skip) for c in args.cube]
    write_outputs(records, Path(args.out), env)


if __name__ == "__main__":
    main()
