#!/usr/bin/env python3
"""Tables and a plot from the VR page's measurement records (issue #224).

    python validation/vr_measure_report.py RECORD.jsonl [RECORD.jsonl ...] [--out DIR]

Reads one or more records written by POST /v1/vr/measurements (files
``vr-measure-<id>.jsonl``, by default in ``~/.visivo/vr-measurements``) and
writes, in ``--out`` (default: next to the first record):

- ``report.md``: per record, condition and phase, the frame figures, pose→frame,
  stop→refined, what was held equal; then loads and link events per record;
- ``per_phase.csv``: the same rows as a table;
- ``frame_time.png``: frame interval over time, one colour per condition.

The figures are pooled over the phase's measured samples (``proto.part ==
"measure"``; lead-ins are left out) from the raw values each sample carries,
with the page's own definitions (the record's header repeats them):

- frame interval mean, p50, p95 as volume.js FrameStats.stats: p = sorted[floor(q (n - 1))];
- FPS = 1000 / mean interval; missed % = intervals above 1.5 × 1000 / display Hz
  (none in a desktop browser, which has no display rate);
- pose→frame and stop→refined p50, p95 as refine.js Rolling.quantile: sorted[min(n - 1, floor(q n))].

Samples recorded outside a run are grouped as condition ``session:<local |
refine-jpeg | refine-webrtc>``, phase ``-``.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import sys
from collections import OrderedDict, defaultdict
from pathlib import Path

#: The run's conditions in their order (the plot's colours follow it, never the rank).
CONDITION_ORDER = ["local", "jpeg", "webrtc", "jpeg-small", "webrtc-small"]
PHASE_ORDER = ["still", "turn", "stops", "-"]
#: The transport each refined condition is about: its samples must all have come over it.
CONDITION_TRANSPORT = {"jpeg": "jpeg", "webrtc": "webrtc", "jpeg-small": "jpeg", "webrtc-small": "webrtc"}
#: Categorical slots 1-5 of the reference palette, in fixed order.
COLOURS = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#6250d6", "#008300", "#e34948"]


# ── The page's definitions ──────────────────────────────────────────────────

def frame_stats(values: list[float]) -> dict | None:
    """volume.js FrameStats.stats: mean, p50, p95 (sorted[floor(q (n - 1))])."""
    a = [v for v in values if v is not None and 0 < v < 1000]
    if not a:
        return None
    s = sorted(a)
    return {"n": len(a), "mean": sum(a) / len(a), "p50": s[math.floor(0.5 * (len(s) - 1))],
            "p95": s[math.floor(0.95 * (len(s) - 1))]}


def missed_pct(values: list[float], hz: float) -> float | None:
    """The share of intervals above 1.5 × the display's frame time, in %."""
    a = [v for v in values if v is not None and 0 < v < 1000]
    if not hz or not a:
        return None
    return 100.0 * sum(1 for v in a if v > 1.5 * 1000.0 / hz) / len(a)


def rolling_quantile(values: list[float], q: float) -> float | None:
    """refine.js Rolling.quantile: sorted[min(n - 1, floor(q n))]."""
    a = sorted(v for v in values if v is not None)
    if not a:
        return None
    return a[min(len(a) - 1, math.floor(q * len(a)))]


# ── Reading ─────────────────────────────────────────────────────────────────

def read_record(path: Path) -> dict:
    header, samples, events = None, [], []
    with open(path, encoding="utf-8") as f:
        for n, raw in enumerate(f, 1):
            raw = raw.strip()
            if not raw:
                continue
            try:
                line = json.loads(raw)
            except ValueError:
                print(f"{path.name}:{n}: not JSON, skipped", file=sys.stderr)
                continue
            k = line.get("k")
            if k == "h" and header is None:
                header = line
            elif k == "s":
                samples.append(line)
            elif k == "e":
                events.append(line)
    rid = (header or {}).get("record") or path.stem.replace("vr-measure-", "")
    return {"path": path, "id": rid, "header": header or {}, "samples": samples, "events": events}


def session_condition(s: dict) -> str:
    r = s.get("refine") or {}
    if not r.get("on"):
        return "session:local"
    return f"session:refine-{r.get('via') or 'none'}"


def group_key(line: dict) -> tuple[str, str] | None:
    """(condition, phase) of a sample or an event; None for a lead-in."""
    p = line.get("proto")
    if p:
        if p.get("part") != "measure":
            return None
        return p.get("condition") or "?", p.get("phase") or "?"
    if line.get("k") == "s":
        return session_condition(line), "-"
    return None


def _order(key: tuple[str, str]) -> tuple:
    c, p = key
    ci = CONDITION_ORDER.index(c) if c in CONDITION_ORDER else len(CONDITION_ORDER) + (0 if not c.startswith("session") else 1)
    pi = PHASE_ORDER.index(p) if p in PHASE_ORDER else len(PHASE_ORDER)
    return ci, c, pi, p


def _distinct(values) -> list:
    out = []
    for v in values:
        key = json.dumps(v, sort_keys=True)
        if key not in [json.dumps(x, sort_keys=True) for x in out]:
            out.append(v)
    return out


def summarise(rec: dict) -> list[dict]:
    groups: dict[tuple[str, str], list[dict]] = defaultdict(list)
    for s in rec["samples"]:
        k = group_key(s)
        if k:
            groups[k].append(s)
    ev_groups: dict[tuple[str, str], list[dict]] = defaultdict(list)
    for e in rec["events"]:
        p = e.get("proto")
        if p and p.get("part") == "measure":
            ev_groups[(p.get("condition"), p.get("phase"))].append(e)
    rows = []
    for key in sorted(groups, key=_order):
        ss = groups[key]
        iv = [v for s in ss for v in (s.get("iv") or [])]
        fs = frame_stats(iv)
        hz = _distinct([(s.get("frames") or {}).get("target_hz") or 0 for s in ss])
        hz0 = hz[0] if len(hz) == 1 else 0
        n_frames = [(s.get("frames") or {}).get("n") or 0 for s in ss]

        def weighted(field: str) -> float | None:
            # Each window's mean weighted by the values it is over (frames.cpu_n,
            # gpu_n: FrameStats keeps them apart from the frame intervals).
            num = den = 0.0
            for s, n_iv in zip(ss, n_frames):
                f = s.get("frames") or {}
                v = f.get(field)
                n = f.get(field.replace("_mean", "_n"), n_iv)
                if v is not None and n:
                    num += v * n
                    den += n
            return num / den if den else None

        pf = [v for s in ss for v in ((s.get("refine") or {}).get("pf") or [])]
        evs = ev_groups.get(key, [])
        first = [e["data"]["ms"] for e in evs if e.get("name") == "refined-first" and isinstance(e.get("data", {}).get("ms"), (int, float))]
        full = [e["data"]["ms"] for e in evs if e.get("name") == "refined-full" and isinstance(e.get("data", {}).get("ms"), (int, float))]
        via = [(s.get("refine") or {}).get("via") for s in ss]
        looks = _distinct([s.get("look") for s in ss])
        held: dict[str, bool] = {
            "look": len(looks) == 1,
            "binning": len(_distinct([(s.get("cube") or {}).get("factors") for s in ss])) == 1,
            "eye": len(_distinct([(s.get("display") or {}).get("eye") for s in ss])) == 1,
            "display_hz": len(hz) == 1,
            "spv": len(_distinct([(s.get("render") or {}).get("spv") for s in ss])) == 1,
        }
        cond_spec = CONDITION_TRANSPORT.get(key[0])
        if cond_spec:
            # Every sample over its transport, and no gap told between samples.
            held["transport"] = (all((s.get("refine") or {}).get("via") == cond_spec for s in ss)
                                 and not any(e.get("name") == "condition-degraded" for e in evs))
        rows.append(OrderedDict(
            record=rec["id"], condition=key[0], phase=key[1], samples=len(ss),
            seconds=round(sum(s.get("dur") or 0 for s in ss) / 1000.0, 2),
            frames=fs["n"] if fs else 0,
            fps=round(1000.0 / fs["mean"], 2) if fs else None,
            frame_mean_ms=round(fs["mean"], 2) if fs else None,
            frame_p50_ms=round(fs["p50"], 2) if fs else None,
            frame_p95_ms=round(fs["p95"], 2) if fs else None,
            missed_pct=None if missed_pct(iv, hz0) is None else round(missed_pct(iv, hz0), 2),
            display_hz=hz0 or None,
            cpu_mean_ms=None if weighted("cpu_mean") is None else round(weighted("cpu_mean"), 2),
            gpu_mean_ms=None if weighted("gpu_mean") is None else round(weighted("gpu_mean"), 2),
            pose_frame_n=len(pf),
            pose_frame_p50_ms=rolling_quantile(pf, 0.5), pose_frame_p95_ms=rolling_quantile(pf, 0.95),
            stop_first_n=len(first), stop_first_p50_ms=rolling_quantile(first, 0.5), stop_first_p95_ms=rolling_quantile(first, 0.95),
            stop_full_n=len(full), stop_full_p50_ms=rolling_quantile(full, 0.5), stop_full_p95_ms=rolling_quantile(full, 0.95),
            via_jpeg_pct=round(100.0 * via.count("jpeg") / len(via), 1) if via else None,
            via_webrtc_pct=round(100.0 * via.count("webrtc") / len(via), 1) if via else None,
            held_equal="yes" if all(held.values()) else "no: " + ", ".join(k for k, v in held.items() if not v),
            look=looks[0] if len(looks) == 1 else "/".join(str(x) for x in looks),
        ))
    return rows


def record_events(rec: dict) -> dict:
    ev = rec["events"]
    loads = [e["data"] for e in ev if e.get("name") == "load-end" and (e.get("data") or {}).get("ok")]
    failed = [e for e in ev if e.get("name") == "load-end" and not (e.get("data") or {}).get("ok")]
    ms = [d.get("ms") for d in loads if isinstance(d.get("ms"), (int, float))]
    link = [(e.get("data") or {}).get("state") for e in ev if e.get("name") == "link"]
    logs = [(e.get("data") or {}).get("event") for e in ev if e.get("name") == "log"]
    return {
        "loads": len(loads), "loads_failed": len(failed),
        "load_p50_ms": rolling_quantile(ms, 0.5), "load_max_ms": max(ms) if ms else None,
        "load_mb": [d.get("mb") for d in loads],
        "link_down": link.count("down"), "link_up": link.count("up"), "link_lost": link.count("lost"),
        "refine_reconnects": logs.count("refine-reconnected"), "refine_closed": logs.count("refine-ws-closed"),
        "backend_unreachable": logs.count("backend-unreachable"), "backend_restarted": logs.count("backend-restarted"),
        "xr_starts": sum(1 for e in ev if e.get("name") == "xr-start"),
        "skipped": [(e.get("data") or {}).get("condition") for e in ev if e.get("name") == "condition-skipped"],
        "cancelled": any((e.get("data") or {}).get("cancelled") for e in ev if e.get("name") == "protocol-end"),
    }


# ── Writing ─────────────────────────────────────────────────────────────────

def _fmt(v) -> str:
    if v is None:
        return "n/a"
    if isinstance(v, float):
        return f"{v:.2f}".rstrip("0").rstrip(".") if abs(v) < 1e5 else f"{v:.3g}"
    return str(v)


def write_markdown(records: list[dict], rows: list[dict], path: Path) -> None:
    out = ["# VR measurement report", ""]
    for rec in records:
        h = rec["header"]
        dev = h.get("device") or {}
        link = h.get("link") or {}
        cube = h.get("cube") or {}
        out += [f"## Record {rec['id']}", "",
                f"- started {h.get('started', 'n/a')}, page build {(h.get('page') or {}).get('build', 'n/a')}, "
                f"backend {(h.get('backend') or {}).get('version') or 'n/a'}",
                f"- device: {dev.get('user_agent', 'n/a')}; GPU {dev.get('gpu') or 'n/a'}",
                f"- link: {link.get('kind', 'n/a')} ({link.get('host', 'n/a')})",
                f"- cube: {cube.get('dims')} {cube.get('encoding')} binned {cube.get('factors')} ({cube.get('binning')}) of "
                f"{cube.get('full_shape')}, dataset {cube.get('dataset')}" if cube else "- cube: none at the start",
                f"- {len(rec['samples'])} samples, {len(rec['events'])} events", ""]
        head = ["condition", "phase", "s", "frames", "FPS", "mean ms", "p50 ms", "p95 ms", "missed %", "Hz", "CPU ms", "GPU ms",
                "pose→frame p50/p95 ms (n)", "stop→refined first p50/p95 ms (n)", "full p50/p95 ms (n)", "via JPEG/WebRTC %", "held equal"]
        out += ["| " + " | ".join(head) + " |", "|" + "---|" * len(head)]
        for r in (r for r in rows if r["record"] == rec["id"]):
            out.append("| " + " | ".join([
                r["condition"], r["phase"], _fmt(r["seconds"]), _fmt(r["frames"]), _fmt(r["fps"]), _fmt(r["frame_mean_ms"]),
                _fmt(r["frame_p50_ms"]), _fmt(r["frame_p95_ms"]), _fmt(r["missed_pct"]), _fmt(r["display_hz"]),
                _fmt(r["cpu_mean_ms"]), _fmt(r["gpu_mean_ms"]),
                f"{_fmt(r['pose_frame_p50_ms'])} / {_fmt(r['pose_frame_p95_ms'])} ({r['pose_frame_n']})",
                f"{_fmt(r['stop_first_p50_ms'])} / {_fmt(r['stop_first_p95_ms'])} ({r['stop_first_n']})",
                f"{_fmt(r['stop_full_p50_ms'])} / {_fmt(r['stop_full_p95_ms'])} ({r['stop_full_n']})",
                f"{_fmt(r['via_jpeg_pct'])} / {_fmt(r['via_webrtc_pct'])}", r["held_equal"]]) + " |")
        ev = record_events(rec)
        out += ["", f"Loads: {ev['loads']} (failed {ev['loads_failed']}), p50 {_fmt(ev['load_p50_ms'])} ms, "
                f"max {_fmt(ev['load_max_ms'])} ms, MB {ev['load_mb']}. "
                f"Pairing link: down {ev['link_down']}, up {ev['link_up']}, lost {ev['link_lost']}. "
                f"Refinement: socket closed {ev['refine_closed']}, reconnected {ev['refine_reconnects']}. "
                f"Backend unreachable {ev['backend_unreachable']}, restarted {ev['backend_restarted']}. "
                f"VR entries {ev['xr_starts']}."
                + (f" Conditions skipped: {', '.join(ev['skipped'])}." if ev["skipped"] else "")
                + (" The run was cancelled." if ev["cancelled"] else ""), ""]
    out += ["Frame figures pooled over each phase's measured samples with volume.js FrameStats' definitions; "
            "pose→frame and stop→refined with refine.js Rolling.quantile's. Lead-ins are left out.", ""]
    path.write_text("\n".join(out), encoding="utf-8")


def write_csv(rows: list[dict], path: Path) -> None:
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        for r in rows:
            w.writerow({k: "" if v is None else v for k, v in r.items()})


def write_plot(records: list[dict], path: Path) -> bool:
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib is not installed: no plot", file=sys.stderr)
        return False
    fig, axes = plt.subplots(len(records), 1, figsize=(10, 3.2 * len(records)), squeeze=False)
    seen_conditions: list[str] = []
    for ax, rec in zip(axes[:, 0], records):
        series: dict[str, tuple[list, list]] = OrderedDict()
        for s in rec["samples"]:
            k = group_key(s)
            cond = k[0] if k else "lead-in"
            t_end = (s.get("t") or 0) / 1000.0
            iv = s.get("iv") or []
            t = t_end - sum(iv) / 1000.0
            xs, ys = series.setdefault(cond, ([], []))
            for v in iv:
                t += v / 1000.0
                xs.append(t)
                ys.append(v)
            xs.append(float("nan"))      # no line across a gap between samples of one condition
            ys.append(float("nan"))
        for cond in sorted(series, key=lambda c: _order((c, "-"))):
            if cond not in seen_conditions:
                seen_conditions.append(cond)
            xs, ys = series[cond]
            # A condition keeps its slot whatever else is in the record (identity, never rank).
            slot = CONDITION_ORDER.index(cond) if cond in CONDITION_ORDER else len(CONDITION_ORDER) + seen_conditions.index(cond) % 3
            colour = "#9a9a96" if cond == "lead-in" else COLOURS[slot]
            ax.plot(xs, ys, lw=1.2 if cond != "lead-in" else 0.8, color=colour, label=cond)
        hz = (rec["header"].get("xr") or {}).get("display_hz")
        if hz:
            ax.axhline(1000.0 / hz, color="#52514e", lw=0.8, ls="--")
            ax.axhline(1.5 * 1000.0 / hz, color="#52514e", lw=0.8, ls=":")
        ax.set_title(f"Record {rec['id']}: frame interval", fontsize=10, loc="left")
        ax.set_xlabel("time since the record started (s)")
        ax.set_ylabel("frame interval (ms)")
        ax.grid(True, color="#e6e5e0", lw=0.6)
        for side in ("top", "right"):
            ax.spines[side].set_visible(False)
        ax.legend(fontsize=8, frameon=False, ncol=min(6, len(series)), loc="upper right")
    fig.tight_layout()
    fig.savefig(path, dpi=140)
    plt.close(fig)
    return True


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("records", nargs="+", type=Path)
    ap.add_argument("--out", type=Path, default=None)
    ap.add_argument("--no-plot", action="store_true")
    a = ap.parse_args(argv)
    records = [read_record(p) for p in a.records]
    out = a.out or a.records[0].resolve().parent
    out.mkdir(parents=True, exist_ok=True)
    rows = [r for rec in records for r in summarise(rec)]
    write_markdown(records, rows, out / "report.md")
    write_csv(rows, out / "per_phase.csv")
    plotted = False if a.no_plot else write_plot(records, out / "frame_time.png")
    print(f"{len(rows)} rows from {len(records)} record(s): {out / 'report.md'}, {out / 'per_phase.csv'}"
          + (f", {out / 'frame_time.png'}" if plotted else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
