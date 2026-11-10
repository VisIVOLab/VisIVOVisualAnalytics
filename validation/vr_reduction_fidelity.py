#!/usr/bin/env python
"""Fidelity of the reduced cube the headset renders (issue #167).

The VR page draws a reduced copy of the cube (``/v1/cube/reduced_bin``: integer
binning, mean or max, 8-bit or 16-bit) while the analysis runs on the full
cube. This script measures what the reduction hides: for each reduction factor
and rule, the share of a reference mask's voxels and flux that is still drawn,
i.e. that lies in a reduced voxel at or above the display threshold, and the
reduced voxels drawn outside the mask (noise drawn as fog).

It calls the app's own code: ``worker_cube_reduced`` (the route's worker, whose
bytes are what the headset receives, decoded with ``cube_reduce.dequantise``)
and ``worker_cube_reduced_info`` (the cube's default threshold). Two display
thresholds are measured, the two the page offers:

  - "cube":    the full cube's default threshold (issue #93), as the desktop's
               3-D view starts from;
  - "binning": the page's default, X-Visivo-Threshold: for mean binning the
               same noise rule applied to the reduced cube (its noise falls as
               1/sqrt(block)); for max binning the full cube's threshold (a block
               maximum is above it exactly when one of its voxels is).

References: HCG 44 against the published mask of Serra et al. (2013) and our
SoFiA-2 run; WALLABY NGC 3137 against our SoFiA-2 run.

Run:  backend/.venv/bin/python validation/vr_reduction_fidelity.py \
          [--sofia-out DIR] [--encodings uint8,float16]
"""
from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

import numpy as np
from astropy.io import fits

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "backend"))
sys.path.insert(0, str(HERE))
from app import cube_reduce  # noqa: E402
from app.compute.reduced import worker_cube_reduced, worker_cube_reduced_info  # noqa: E402
from datasets import DATASETS, OUT  # noqa: E402

SERRA_DEMO = Path("/Users/fvitello/Downloads/DemoData/hcg44_serra2013_binmask.fits")

#: Table titles (datasets.py keeps the historical "HGC 44" spelling of the file).
TITLES = {"hgc44": "HCG 44", "wallaby": "WALLABY NGC 3137"}

#: (spatial factor, spectral factor) pairs measured.
FACTORS = [(1, 1), (2, 1), (3, 1), (4, 1), (6, 1), (8, 1), (2, 2), (3, 2), (4, 2)]


def load(path) -> np.ndarray:
    with fits.open(path, memmap=True) as hdul:
        hdu = next(h for h in hdul if h.data is not None and h.data.ndim >= 3)
        data = hdu.data
        while data.ndim > 3:
            data = data[0]
        return np.asarray(data, dtype=np.float32)


def references(sofia_out: Path):
    serra = SERRA_DEMO if SERRA_DEMO.is_file() else sofia_out / "serra2013_hcg44_mask.fits"
    return [
        ("hgc44", "Serra+2013", serra),
        ("hgc44", "SoFiA-2", sofia_out / DATASETS["hgc44"].mask),
        ("wallaby", "SoFiA-2", sofia_out / DATASETS["wallaby"].mask),
    ]


def block_sizes(full_shape, factors) -> np.ndarray:
    """Full-resolution voxels in each reduced voxel (partial blocks at the edges)."""
    fx, fy, fz = factors
    lens = []
    for n, f in zip(full_shape, (fz, fy, fx)):
        starts = np.arange(0, n, f)
        lens.append(np.minimum(f, n - starts))
    return lens[0][:, None, None] * lens[1][None, :, None] * lens[2][None, None, :]


def measure(cube, mask_idx, mask_flux_total, mask_count, sizes, visible, factors):
    fx, fy, fz = factors
    z, y, x = mask_idx
    seen = visible[z // fz, y // fy, x // fx]
    vals = cube[z, y, x]
    flux = float(vals[seen].sum()) / mask_flux_total if mask_flux_total else float("nan")
    drawn = int(visible.sum())
    fog = int((visible & (mask_count == 0)).sum())
    # Full-resolution voxels outside the mask that a drawn reduced voxel covers:
    # the blur of the reduction, as a share of the mask's own size.
    spill = int((sizes - mask_count)[visible].sum())
    return {"voxels_kept": float(seen.mean()), "flux_kept": flux,
            "drawn": drawn, "fog": fog, "fog_fraction": fog / drawn if drawn else 0.0,
            "spill": spill / max(1, z.size)}


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--sofia-out", type=Path, default=HERE / "sofia_out",
                    help="directory with the SoFiA-2 masks (default validation/sofia_out)")
    ap.add_argument("--encodings", default="uint8,float16")
    args = ap.parse_args()
    encodings = args.encodings.split(",")

    rows = []
    cubes: dict[str, np.ndarray] = {}
    for name, ref, mask_path in references(args.sofia_out):
        cube_path = DATASETS[name].cube
        if not Path(cube_path).is_file() or not Path(mask_path).is_file():
            print(f"skip {name}/{ref}: missing {cube_path if not Path(cube_path).is_file() else mask_path}")
            continue
        if name not in cubes:
            cubes[name] = load(cube_path)
        cube = cubes[name]
        finite = np.isfinite(cube)
        mask = (load(mask_path) > 0) & finite
        mask_idx = np.nonzero(mask)
        total = float(cube[mask & (cube > 0)].sum())
        cube_thr = worker_cube_reduced_info(cube_path, 1, 1, 4096, limit=False)["default_threshold"]
        for fxy, fz in FACTORS:
            factors = (fxy, fxy, fz)
            rshape = cube_reduce.reduced_shape(cube.shape, factors)
            lin = np.ravel_multi_index((mask_idx[0] // fz, mask_idx[1] // fxy, mask_idx[2] // fxy), rshape)
            mask_count = np.bincount(lin, minlength=int(np.prod(rshape))).reshape(rshape)
            sizes = block_sizes(cube.shape, factors)
            for mode in ("mean", "max"):
                for enc in encodings:
                    res = worker_cube_reduced(cube_path, fxy, fz, 4096, mode, enc, limit=False)
                    shape = (res["depth"], res["height"], res["width"])
                    shown = cube_reduce.dequantise(res["raw_bytes"], shape, res["range_min"],
                                                   res["range_max"], enc)
                    for source, thr in (("cube", cube_thr), ("binning", res["threshold"])):
                        if thr is None:
                            continue
                        # The page compares the normalised value with the
                        # normalised threshold; the same comparison in data units.
                        visible = np.isfinite(shown) & (shown >= thr)
                        m = measure(cube, mask_idx, total, mask_count, sizes, visible, factors)
                        rows.append({"cube": TITLES.get(name, DATASETS[name].title), "reference": ref,
                                     "factors": f"{fxy}x{fxy}x{fz}", "mode": mode,
                                     "encoding": enc, "threshold_source": source,
                                     "threshold": thr,
                                     "reduced_shape": "x".join(str(v) for v in shape[::-1]),
                                     **m})
                        print(rows[-1]["cube"], ref, rows[-1]["factors"], mode, enc, source,
                              f"thr={thr:.3g} vox={m['voxels_kept']:.3f} flux={m['flux_kept']:.3f} "
                              f"fog={m['fog']}", flush=True)

    OUT.mkdir(exist_ok=True)
    fields = ["cube", "reference", "factors", "reduced_shape", "mode", "encoding",
              "threshold_source", "threshold", "voxels_kept", "flux_kept", "drawn", "fog",
              "fog_fraction", "spill"]
    with open(OUT / "vr_reduction_fidelity.csv", "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=fields)
        w.writeheader()
        w.writerows(rows)
    lines = ["# Fidelity of the headset's reduced cube (issue #167)", "",
             "Share of each reference mask still drawn above the display threshold, by",
             "reduction (spatial x spatial x spectral factor) and rule. 'cube' = the full",
             "cube's default threshold (as on the desktop); 'binning' = the page's default:",
             "the noise rule on the reduced cube for mean binning, the full cube's threshold",
             "for max binning. Fog = reduced voxels drawn that",
             "hold no mask voxel. Spill = full-resolution voxels outside the mask covered",
             "by a drawn reduced voxel, as a share of the mask's size: the blur of the",
             "reduction ('voxels kept' rises with the factor partly because of it).",
             "Encoding as sent to the headset.", ""]
    for enc in encodings:
        lines += [f"## {enc}", "",
                  "| Cube | Reference | Factors | Rule | Threshold from | Threshold | Voxels kept | Flux kept | Fog voxels (of drawn) | Spill |",
                  "|---|---|---|---|---|---|---|---|---|---|"]
        for r in rows:
            if r["encoding"] != enc:
                continue
            lines.append(f"| {r['cube']} | {r['reference']} | {r['factors']} | {r['mode']} "
                         f"| {r['threshold_source']} | {r['threshold']:.3g} "
                         f"| {100 * r['voxels_kept']:.1f} % | {100 * r['flux_kept']:.1f} % "
                         f"| {r['fog']:,} ({100 * r['fog_fraction']:.0f} %) | {100 * r['spill']:.0f} % |")
        lines.append("")
    (OUT / "vr_reduction_fidelity.md").write_text("\n".join(lines))
    print("\n".join(lines))


if __name__ == "__main__":
    main()
