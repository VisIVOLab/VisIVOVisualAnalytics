#!/usr/bin/env python3
"""
moment_ooc_bench.py - out-of-core moment maps (M0, M1, M2) at SKA scale.

benchmark_visivo.py measures what a client sees through the REST API. This
script measures the moment computation itself, in the process that runs it,
so that time and memory belong to one computation and nothing else: the
backend's own functions are called, with the arguments the service passes
them, one measurement per process.

Three ways to compute a moment, all from backend/app/compute/moment.py:

  dask    _moment_map_dask on the local threaded scheduler: what the service
          runs for a channel range above VISIVO_DASK_MIN_BYTES (the default
          VISIVO_DASK_MODE=auto), VISIVO_MOMENT_THREADS threads
  stream  worker_moment with the slab streamer: what the service runs with
          VISIVO_DASK_MODE=off or without dask (single thread)
  inmem   worker_moment holding the whole range in memory (Numba / CuPy):
          what the service runs for small ranges; on a cube larger than the
          cap it must fail, which is how the cap is shown to bite

Subcommands

  measure   one moment, one process: wall and CPU time, peak memory
  check     the three ways on one sub-cube, compared map by map
  summarise the JSON lines of many measures as a Markdown table

Memory

  --cap-gb sets RLIMIT_DATA in the measuring process before anything is
  mapped. The kernel (Linux >= 4.7) then refuses any private writable memory
  beyond the cap: heap, numpy arrays, thread stacks, copy-on-write mappings.
  Read-only shared file mappings and the page cache are not counted, so a
  cube can be mapped whole while anything that materialises it fails with
  MemoryError. It is per process; every thread of the computation runs in
  this one. Whether the kernel enforces it is tested and recorded
  (cap_enforced): macOS, for one, does not.

  The resident set is sampled every 0.1 s from /proc/self/status and split
  into anonymous pages (RssAnon: what the computation materialised) and file
  pages (RssFile: the mapped cube, clean and reclaimable, which is why RSS
  over-counts a streaming computation). Both peaks are reported, with VmHWM.

Cold reads

  --evict drops the cube's pages from the page cache before the measurement
  (posix_fadvise DONTNEED, no privileges needed; pages mapped by a live
  process stay). One measurement per process makes it effective.

Examples

  python3 moment_ooc_bench.py measure --cube /data/cube.fits --order 2 \\
      --path dask --cap-gb 16 --evict --out results.jsonl
  python3 moment_ooc_bench.py check --cube /data/cube.fits --channels 3000:3127 \\
      --mask 8.6e-5 --criterion moment_criterion_ulp.py --out check.json
  python3 moment_ooc_bench.py summarise results.jsonl > table.md
"""
from __future__ import annotations

import argparse
import base64
import datetime
import importlib.util
import json
import os
import platform
import resource
import socket
import statistics
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
GB = 1e9


# ── memory -------------------------------------------------------------------

def set_data_cap(cap_gb: float | None) -> dict:
    """Cap private writable memory at ``cap_gb`` and test that the kernel
    enforces it: an anonymous mapping just above the cap must be refused."""
    if not cap_gb:
        return {"cap_gb": None, "cap_enforced": None}
    import mmap  # noqa: PLC0415

    cap = int(cap_gb * GB)
    # The soft limit is what the kernel enforces; the hard one is left alone
    # (lowering it is refused on some systems, macOS among them).
    _soft, hard = resource.getrlimit(resource.RLIMIT_DATA)
    if hard != resource.RLIM_INFINITY and hard < cap:
        raise SystemExit(f"--cap-gb {cap_gb} is above this process's hard data limit")
    try:
        resource.setrlimit(resource.RLIMIT_DATA, (cap, hard))
    except (ValueError, OSError) as exc:
        return {"cap_gb": cap_gb, "cap_enforced": False, "cap_error": str(exc)}
    try:
        # Anonymous, private and writable (what RLIMIT_DATA counts), never touched.
        probe = mmap.mmap(-1, cap + (1 << 30), flags=mmap.MAP_PRIVATE,
                          prot=mmap.PROT_READ | mmap.PROT_WRITE)
        probe.close()
        enforced = False
    except (OSError, MemoryError):
        enforced = True
    return {"cap_gb": cap_gb, "cap_enforced": enforced}


def _proc_status() -> dict[str, int]:
    out: dict[str, int] = {}
    with open("/proc/self/status") as fh:
        for line in fh:
            key, _, rest = line.partition(":")
            if key in ("RssAnon", "RssFile", "RssShmem", "VmHWM"):
                out[key] = int(rest.split()[0]) * 1024
    return out


class Sampler:
    """Peak anonymous and file-backed resident memory of this process."""

    def __init__(self, period: float = 0.1) -> None:
        self.period = period
        self.available = Path("/proc/self/status").exists()
        self.peak = {"RssAnon": 0, "RssFile": 0, "RssShmem": 0}
        self.samples = 0
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._loop, daemon=True)

    def _take(self) -> None:
        st = _proc_status()
        for k in self.peak:
            self.peak[k] = max(self.peak[k], st.get(k, 0))
        self.samples += 1

    def _loop(self) -> None:
        while not self._stop.wait(self.period):
            self._take()

    def __enter__(self):
        if self.available:
            self._take()
            self._thread.start()
        return self

    def __exit__(self, *exc):
        if self.available:
            self._stop.set()
            self._thread.join()
            self._take()
        return False

    def record(self) -> dict:
        ru_kb = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        # ru_maxrss is in KiB on Linux and in bytes on macOS.
        maxrss = ru_kb if sys.platform == "darwin" else ru_kb * 1024
        rec = {"maxrss_gb": round(maxrss / GB, 3)}
        if self.available:
            rec.update(anon_peak_gb=round(self.peak["RssAnon"] / GB, 3),
                       file_peak_gb=round(self.peak["RssFile"] / GB, 3),
                       shmem_peak_gb=round(self.peak["RssShmem"] / GB, 3),
                       hwm_gb=round(_proc_status().get("VmHWM", 0) / GB, 3),
                       mem_samples=self.samples)
        return rec


def evict(path: str) -> str:
    """Drop the file's clean pages from the page cache."""
    if not hasattr(os, "posix_fadvise"):
        return "unsupported"
    fd = os.open(path, os.O_RDONLY)
    try:
        os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
    finally:
        os.close(fd)
    return "posix_fadvise DONTNEED"


# ── backend ------------------------------------------------------------------

def import_backend(backend_dir: str | None):
    """The backend's moment module, from ``backend_dir`` or the repository."""
    root = Path(backend_dir) if backend_dir else HERE.parent / "backend"
    sys.path.insert(0, str(root))
    os.environ.setdefault("VISIVO_TOKEN", "bench")   # no token file written
    from app.compute import moment  # noqa: PLC0415
    from app.fits_dataset import ScientificFitsDataset  # noqa: PLC0415
    return moment, ScientificFitsDataset


def _parse_range(text: str | None) -> tuple[int, int] | None:
    if not text:
        return None
    a, b = text.split(":")
    return int(a), int(b)


def run_moment(moment, path: str, way: str, order: int, z0: int, z1: int,
               mask_enabled: bool, threshold: float, threshold_auto: bool) -> dict:
    """One moment map, computed as the service would along ``way``."""
    if way == "dask":
        return moment._moment_map_dask(path, order, z0, z1, threshold, mask_enabled,
                                       64, None, threshold_auto, False)
    # worker_moment streams when the range exceeds _moment_stream_bytes();
    # replace that threshold for this call only to force one way or the other.
    original = moment._moment_stream_bytes
    moment._moment_stream_bytes = (lambda: 0) if way == "stream" else (lambda: 1 << 62)
    try:
        return moment.worker_moment(path, order, z0, z1, mask_enabled, threshold,
                                    threshold_auto, False, None)
    finally:
        moment._moment_stream_bytes = original


def decode_map(result: dict):
    import numpy as np  # noqa: PLC0415

    raw = base64.b64decode(result["data_base64"])
    return np.frombuffer(raw, dtype="<f4").reshape(int(result["height"]),
                                                    int(result["width"]))


def _mask_args(mask: str) -> tuple[bool, float, bool]:
    """'auto' (median + 3 sigma, the client's default), 'none', or a number."""
    if mask == "auto":
        return True, 0.0, True
    if mask == "none":
        return False, 0.0, False
    return True, float(mask), False


def _channels(cube_shape, text: str | None) -> tuple[int, int]:
    nz = int(cube_shape[0])
    rng = _parse_range(text)
    if rng is None:
        return 0, nz - 1
    z0, z1 = rng
    if not 0 <= z0 <= z1 < nz:
        raise SystemExit(f"--channels {text}: need 0 <= A <= B < {nz}")
    return z0, z1


def environment(args) -> dict:
    return {"host": socket.gethostname(), "platform": platform.platform(),
            "machine": platform.machine(), "python": platform.python_version(),
            "cpus": os.cpu_count(), "slurm_job": os.environ.get("SLURM_JOB_ID"),
            "moment_threads_env": os.environ.get("VISIVO_MOMENT_THREADS"),
            "cuda_visible": os.environ.get("CUDA_VISIBLE_DEVICES"),
            "mem_total_gb": _mem_total_gb(),
            "commit": os.environ.get("VISIVO_BENCH_COMMIT"),
            "image": os.environ.get("VISIVO_BENCH_IMAGE")}


def _mem_total_gb() -> float | None:
    try:
        with open("/proc/meminfo") as fh:
            for line in fh:
                if line.startswith("MemTotal:"):
                    return round(int(line.split()[1]) * 1024 / GB, 1)
    except OSError:
        pass
    return None


# What each way runs in the backend, recorded with every measurement.
FUNCTIONS = {"dask": "compute.moment._moment_map_dask (local threaded scheduler)",
             "stream": "compute.moment.worker_moment -> _moment_map_streaming",
             "inmem": "compute.moment.worker_moment -> _moment_map_from_array"}


# ── measure ------------------------------------------------------------------

def cmd_measure(args) -> None:
    if args.threads:
        os.environ["VISIVO_MOMENT_THREADS"] = str(args.threads)
    rec: dict = {"timestamp": datetime.datetime.now().isoformat(timespec="seconds"),
                 "label": args.label, "cube": args.cube, "path": args.path,
                 "order": args.order, "mask": args.mask, "repeat": args.repeat}
    rec["evicted"] = evict(args.cube) if args.evict else "no"
    rec["function"] = FUNCTIONS[args.path]
    rec["cap_method"] = "RLIMIT_DATA" if args.cap_gb else None
    rec.update(set_data_cap(args.cap_gb))
    import numpy as np  # noqa: PLC0415

    moment, dataset_cls = import_backend(args.backend)
    dataset = dataset_cls(args.cube)
    hdul, raw = dataset.open_memmap()
    try:
        cube = dataset.squeeze_to_3d(raw)
        z0, z1 = _channels(cube.shape, args.channels)
        ny, nx = int(cube.shape[1]), int(cube.shape[2])
    finally:
        hdul.close()
    nch = z1 - z0 + 1
    rec.update(channels=[z0, z1], shape=[nch, ny, nx],
               range_gb=round(nch * ny * nx * 4 / GB, 2),
               file_gb=round(os.path.getsize(args.cube) / GB, 2),
               moment_threads=moment._n_moment_threads(),
               chunk_bytes=moment._MOMENT_CHUNK_BYTES,
               channels_per_block=moment._moment_chunk_z(ny * nx * 4, 64),
               env=environment(args))
    mask_enabled, threshold, auto = _mask_args(args.mask)
    cpu0 = os.times()
    t0 = time.perf_counter()
    try:
        with Sampler() as sampler:
            result = run_moment(moment, args.cube, args.path, args.order, z0, z1,
                                mask_enabled, threshold, auto)
        rec["status"] = "ok"
    except MemoryError as exc:
        rec["status"] = f"MemoryError: {exc}"[:300]
        result = None
    except Exception as exc:                        # noqa: BLE001 - recorded, not hidden
        rec["status"] = f"{type(exc).__name__}: {exc}"[:300]
        result = None
    wall = time.perf_counter() - t0
    cpu1 = os.times()
    # Full precision: rounded only when the table is written.
    rec["wall_s"] = wall
    rec["cpu_s"] = (cpu1.user - cpu0.user) + (cpu1.system - cpu0.system)
    rec.update(sampler.record())
    if result is not None:
        img = decode_map(result)
        finite = np.isfinite(img)
        rec.update(threshold_used=result.get("threshold_used"),
                   moment_unit=result.get("moment_unit"),
                   finite_pixels=int(finite.sum()),
                   map_sum=float(np.sum(img[finite], dtype=np.float64)))
        # Bytes the reduction read from the cube (M2 sweeps it twice, on both
        # ways), per second of the whole call.
        passes = 2 if args.order == 2 else 1
        rec["read_gb_per_s"] = passes * nch * ny * nx * 4 / GB / wall
    line = json.dumps(rec)
    print(line, flush=True)
    if args.out:
        with open(args.out, "a") as fh:
            fh.write(line + "\n")
    if rec["status"] != "ok" and not args.allow_fail:
        sys.exit(1)


# ── check --------------------------------------------------------------------

def default_compare(order: int, ref, test, reference: str = "inmem",
                    rtol: float = 1e-5, atol: float = 0.0) -> dict:
    """Same blanked pixels, and |test - ref| <= atol + rtol*|ref| on the rest."""
    import numpy as np  # noqa: PLC0415

    if np.isinf(ref).any() or np.isinf(test).any():   # an overflow, never a blank
        return {"ok": False, "infinite_ref": int(np.isinf(ref).sum()),
                "infinite_test": int(np.isinf(test).sum())}
    fr, ft = ~np.isnan(ref), ~np.isnan(test)
    both = fr & ft
    diff = np.abs(test[both].astype(np.float64) - ref[both].astype(np.float64))
    scale = np.abs(ref[both].astype(np.float64))
    bad = diff > atol + rtol * scale
    with np.errstate(divide="ignore", invalid="ignore"):
        rel = np.where(scale > 0, diff / scale, np.where(diff > 0, np.inf, 0.0))
    return {"ok": bool(np.array_equal(fr, ft) and not bad.any()),
            "nan_mismatch": int((fr != ft).sum()),
            "finite_ref": int(fr.sum()), "finite_test": int(ft.sum()),
            "max_abs_diff": float(diff.max()) if diff.size else 0.0,
            "max_rel_diff": float(rel.max()) if rel.size else 0.0,
            "n_out_of_tolerance": int(bad.sum()), "rtol": rtol, "atol": atol}


def load_criterion(path: str | None):
    """A plug-in comparison: a Python file defining
    ``compare(order, ref, test, reference) -> dict`` with at least the key
    ``ok``; ``reference`` names what ``ref`` is: "inmem" (the backend's
    in-memory map) or "float64" (reference64 below)."""
    if not path:
        return None
    spec = importlib.util.spec_from_file_location("moment_criterion", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.compare


def reference64(subcube, coords, order: int, mask_enabled: bool, threshold: float):
    """M0, M1 or M2 (as a dispersion, the backend's default) in float64 numpy,
    from the definitions and sharing no code with the backend; the channel
    width is |dν| from the gradient of the channel coordinates."""
    import numpy as np  # noqa: PLC0415

    d = np.asarray(subcube, dtype=np.float64)
    nu = np.asarray(coords, dtype=np.float64)
    valid = np.isfinite(d)
    if mask_enabled:
        valid &= np.where(valid, d, -np.inf) >= threshold
    width = np.abs(np.gradient(nu)) if nu.size > 1 else np.ones(1)
    w = np.where(valid, d, 0.0) * width[:, None, None]
    s0 = w.sum(axis=0)
    with np.errstate(divide="ignore", invalid="ignore"):
        if order == 0:
            return np.where(valid.any(axis=0), s0, np.nan)
        m1 = np.where(s0 != 0.0, (w * nu[:, None, None]).sum(axis=0) / s0, np.nan)
        # A weighted mean outside the sampled axis, or a variance outside
        # [0, span²], is not a moment (mixed-sign weights without a mask);
        # such pixels have none, as in the backend. The bounds allow for
        # rounding: a line of sight whose only valid voxel is in the first or
        # last channel has M1 on the bound, and summing can land it a few
        # float64 ULP outside (541 such pixels in a 3-sigma SDC2 band).
        lo, hi = float(nu.min()), float(nu.max())
        if order == 1:
            slack = 1e-9 * max(abs(lo), abs(hi))   # rounding at the axis offset
            return np.where((m1 >= lo - slack) & (m1 <= hi + slack), m1, np.nan)
        # The variance does not depend on the axis offset, nor does its bound.
        span2 = (hi - lo) ** 2
        var = (w * (nu[:, None, None] - m1[None]) ** 2).sum(axis=0) / s0
        var = np.where((var >= 0.0) & (var <= span2 * (1.0 + 1e-9)), var, np.nan)
        return np.sqrt(var)


def cmd_check(args) -> None:
    if args.threads:
        os.environ["VISIVO_MOMENT_THREADS"] = str(args.threads)
    import numpy as np  # noqa: PLC0415

    moment, dataset_cls = import_backend(args.backend)
    dataset = dataset_cls(args.cube)
    hdul, raw = dataset.open_memmap()
    try:
        cube = dataset.squeeze_to_3d(raw)
        z0, z1 = _channels(cube.shape, args.channels)
        subcube = np.array(cube[z0:z1 + 1], dtype=np.float32)
    finally:
        hdul.close()
    coords = dataset.spectral_axis_metadata(z0, z1)["coordinates"]
    mask_enabled, threshold, auto = _mask_args(args.mask)
    if auto:
        raise SystemExit("check needs an explicit --mask threshold (or none): the "
                         "automatic threshold depends on the channel range")
    criterion = load_criterion(args.criterion)
    report: dict = {"cube": args.cube, "channels": [z0, z1], "mask": args.mask,
                    "criterion": args.criterion
                    or f"default (rtol {args.rtol}, atol {args.atol})",
                    "env": environment(args), "orders": {}}
    all_ok = True
    pairs = [("inmem", "stream"), ("inmem", "dask"),
             ("float64", "inmem"), ("float64", "stream"), ("float64", "dask")]
    for order in args.orders:
        maps, meta = {}, {}
        for way in ("inmem", "stream", "dask"):
            t0 = time.perf_counter()
            res = run_moment(moment, args.cube, way, order, z0, z1, mask_enabled,
                             threshold, auto)
            maps[way] = decode_map(res)
            meta[way] = {"wall_s": round(time.perf_counter() - t0, 2),
                         "moment_unit": res.get("moment_unit")}
        maps["float64"] = reference64(subcube, coords, order, mask_enabled, threshold)
        if args.save_maps:
            for way, img in maps.items():
                np.save(f"{args.save_maps}_m{order}_{way}.npy", img)
        entry = {"timing": meta}
        for ref, test in pairs:
            if criterion:
                cmp = criterion(order, maps[ref], maps[test], ref)
            else:
                cmp = default_compare(order, maps[ref], maps[test], ref,
                                      args.rtol, args.atol)
            entry[f"{test}_vs_{ref}"] = cmp
            all_ok &= bool(cmp["ok"])
        report["orders"][str(order)] = entry
        print(f"M{order}: " + "  ".join(
            f"{t}/{r} ok={entry[f'{t}_vs_{r}']['ok']}" for r, t in pairs), flush=True)
    report["ok"] = all_ok
    text = json.dumps(report, indent=2)
    if args.out:
        Path(args.out).write_text(text + "\n")
    else:
        print(text)
    sys.exit(0 if all_ok else 1)


# ── summarise ----------------------------------------------------------------

def med_mad(xs: list[float]) -> tuple[float, float]:
    m = statistics.median(xs)
    mad = statistics.median([abs(x - m) for x in xs]) if len(xs) > 1 else 0.0
    return m, 1.4826 * mad


def cmd_summarise(args) -> None:
    rows: dict[tuple, list[dict]] = {}
    for f in args.files:
        for line in Path(f).read_text().splitlines():
            if line.strip():
                r = json.loads(line)
                # One row per configuration: runs that differ in anything that
                # changes the work are never pooled into one median.
                key = (r.get("label") or Path(r["cube"]).name, r["path"], r["order"],
                       r["cube"], tuple(r.get("channels") or ()), str(r.get("mask")),
                       str(r.get("cap_gb")), str(r.get("moment_threads")))
                rows.setdefault(key, []).append(r)
    print("| cube | way | moment | threads | range GB | n | wall s (median ± MAD) "
          "| first run s | GB/s | CPU/wall | anon peak GB | file peak GB | cap GB | status |")
    print("|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|")

    def peak(rs, key):
        vals = [r[key] for r in rs if r.get(key) is not None]
        return f"{max(vals):.2f}" if vals else "n/a"

    for (label, way, order, *_), rs in sorted(rows.items(), key=lambda kv: kv[0]):
        ok = [r for r in rs if r["status"] == "ok"]
        # Repeat 0 is the first run, reported on its own; the median is over
        # the repeats after it (over every run when there is no repeat 0).
        first = min(rs, key=lambda r: r.get("repeat", 0))
        rep = [r for r in ok if r.get("repeat", 0) >= 1] or ok
        if rep:
            m, s = med_mad([r["wall_s"] for r in rep])
            wall = f"{m:.0f} ± {s:.0f}" if m >= 100 else f"{m:.1f} ± {s:.1f}"
            gbps = f"{statistics.median([r['read_gb_per_s'] for r in rep]):.2f}"
            cpuw = f"{statistics.median([r['cpu_s'] / max(r['wall_s'], 1e-9) for r in rep]):.1f}"
        else:
            wall = gbps = cpuw = "n/a"
        first_s = (f"{first['wall_s']:.1f}" if first.get("repeat", 0) == 0
                   and first["status"] == "ok" else "-")
        cap = first.get("cap_gb")
        enforced = "" if first.get("cap_enforced") in (True, None) else " (not enforced)"
        status = "ok" if len(ok) == len(rs) else "; ".join(
            sorted({r["status"][:60] for r in rs if r["status"] != "ok"}))
        # Only the dask way is multi-threaded; the streamer runs on one thread.
        threads = first.get("moment_threads", "?") if way == "dask" else 1
        print(f"| {label} | {way} | M{order} | {threads} "
              f"| {first.get('range_gb', '?')} | {len(rep)} "
              f"| {wall} | {first_s} | {gbps} | {cpuw} | {peak(rs, 'anon_peak_gb')} "
              f"| {peak(rs, 'file_peak_gb')} | {cap}{enforced} | {status} |")


# ── main ---------------------------------------------------------------------

def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p):
        p.add_argument("--cube", required=True, help="FITS cube on this host")
        p.add_argument("--channels", default=None, metavar="A:B",
                       help="channel range, inclusive (default: all)")
        p.add_argument("--mask", default="auto",
                       help="auto (median + 3 sigma), none, or a threshold value")
        p.add_argument("--threads", type=int, default=0,
                       help="VISIVO_MOMENT_THREADS for the dask way (0: backend default)")
        p.add_argument("--backend", default=None,
                       help="backend directory holding app/ (default: the repository's)")
        p.add_argument("--out", default=None)

    m = sub.add_parser("measure", help="one moment, one process")
    common(m)
    m.add_argument("--order", type=int, choices=(0, 1, 2), required=True)
    m.add_argument("--path", choices=("dask", "stream", "inmem"), default="dask")
    m.add_argument("--cap-gb", type=float, default=None,
                   help="RLIMIT_DATA cap for this process, in GB")
    m.add_argument("--evict", action="store_true",
                   help="drop the cube from the page cache first")
    m.add_argument("--label", default=None, help="name of the cube in the table")
    m.add_argument("--repeat", type=int, default=0)
    m.add_argument("--allow-fail", action="store_true",
                   help="exit 0 when the computation fails (an expected MemoryError)")
    m.set_defaults(func=cmd_measure)

    c = sub.add_parser("check", help="in-memory vs streaming vs dask on a sub-cube")
    common(c)
    c.add_argument("--orders", type=int, nargs="+", default=[0, 1, 2])
    c.add_argument("--rtol", type=float, default=1e-5)
    c.add_argument("--atol", type=float, default=0.0)
    c.add_argument("--criterion", default=None,
                   help="Python file defining compare(order, ref, test) -> dict with 'ok'")
    c.add_argument("--save-maps", default=None, metavar="PREFIX",
                   help="save every map as PREFIX_m<order>_<way>.npy")
    c.set_defaults(func=cmd_check)

    s = sub.add_parser("summarise", help="Markdown table from measure output")
    s.add_argument("files", nargs="+")
    s.set_defaults(func=cmd_summarise)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
