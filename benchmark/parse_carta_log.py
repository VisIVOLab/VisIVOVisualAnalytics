#!/usr/bin/env python
"""Extract CARTA operation timings from ~/.carta/log/carta.log (--log_performance).

Fills the CARTA column of Table 2 from a short GUI session on the 8 GB cube.
CARTA has no headless compute path (its scripting proxies actions to a connected
frontend), so the three comparable operations are run once in the browser GUI and
their durations read from CARTA's own performance log.

Run AFTER the GUI session:
    backend/.venv/bin/python benchmark/parse_carta_log.py
"""
from __future__ import annotations

import re
from pathlib import Path

LOG = Path.home() / ".carta" / "log" / "carta.log"
# CARTA logs perf lines like "... <something> in 1234 ms" / "... took 1.23 s".
PATTERNS = {
    "open / load": r"(load|open|render|raster|fill image cache|contour)",
    "moment": r"moment",
    "pv": r"(pv|position[- ]velocity)",
    "tile": r"tile",
}
TIME_RE = re.compile(r"([\d.]+)\s*(ms|s)\b", re.I)


def to_seconds(val: str, unit: str) -> float:
    return float(val) / 1000.0 if unit.lower() == "ms" else float(val)


def main() -> None:
    if not LOG.exists():
        raise SystemExit(f"no log at {LOG} — run the CARTA GUI session first")
    lines = LOG.read_text(errors="ignore").splitlines()
    timed = [(l, TIME_RE.search(l)) for l in lines]
    timed = [(l, m) for l, m in timed if m]
    if not timed:
        print("No timing lines found. Was the backend started with "
              "--log_performance and were operations run in the GUI?")
        # still dump candidate lines with 'ms'/'s' for manual inspection
    print(f"# CARTA timing lines in {LOG} ({len(timed)} with a duration)\n")
    for label, pat in PATTERNS.items():
        rx = re.compile(pat, re.I)
        hits = [(to_seconds(m.group(1), m.group(2)), l.strip())
                for l, m in timed if rx.search(l)]
        if hits:
            hits.sort(reverse=True)
            print(f"## {label}  (max {hits[0][0]:.2f} s)")
            for sec, l in hits[:6]:
                print(f"  {sec:8.3f} s  | {l[-140:]}")
            print()
    # everything else with a duration, for completeness
    known = re.compile("|".join(PATTERNS.values()), re.I)
    other = [(to_seconds(m.group(1), m.group(2)), l.strip())
             for l, m in timed if not known.search(l)]
    other.sort(reverse=True)
    if other:
        print("## other timed lines (top 10)")
        for sec, l in other[:10]:
            print(f"  {sec:8.3f} s  | {l[-140:]}")


if __name__ == "__main__":
    main()
