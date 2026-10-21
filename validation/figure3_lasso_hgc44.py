#!/usr/bin/env python
"""Figure 3 (data-driven panels) — the kinematic lasso on a real HI galaxy.

Produces the science panels of the paper's Figure 3 on an HCG 44 galaxy, headless,
through the application's own code — every panel is something the viewer shows:
  (a) the selection outlined on its channel map, as the 2-D view draws it;
  (b) the lasso panel's per-channel profile (selected pixels per channel) at three
      values of the velocity coupling λv;
  (c) Isolate → new cube, then Compute Moment 0 on it (mask off);
  (d) Remove → new cube, the same.
The seed double-click and the per-channel polygon refinement are gestures in the
app and are described in the text rather than drawn here.

Run:  backend/.venv/bin/python validation/figure3_lasso_hgc44.py
"""
from __future__ import annotations

import base64
import sys
import tempfile
from pathlib import Path

import numpy as np
from astropy.io import fits

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "backend"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from datasets import rank_sources  # noqa: E402
from app.compute.moment import worker_moment  # noqa: E402
from app.compute.selection import cube_kinematic_select, worker_cube_mask_apply  # noqa: E402

import os  # noqa: E402

# Out-of-repo HI cube; point at it via the env var so the figure is reproducible anywhere.
CUBE = os.environ.get(
    "VISIVO_VALIDATION_CUBE", "/Users/fvitello/downloads/DemoData/hgc44_3axes.fits")
MASK = str(Path(__file__).resolve().parent / "sofia_out" / "sofia_hgc44_mask.fits")
OUT = Path(__file__).resolve().parent / "out"


def app_moment0(mask: np.ndarray, mode: str, z0: int, z1: int, workdir: Path) -> np.ndarray:
    """Isolate / Remove (POST /v1/cube/mask_apply_bin) and Compute Moment 0 with the
    mask off (POST /v1/moment) — the same workers, the same two gestures."""
    nz, ny, nx = mask.shape
    out = workdir / f"lasso_{mode}.fits"
    worker_cube_mask_apply(CUBE, str(out), mask.astype("<f4").tobytes(), 0, 0, 0, nx, ny, nz,
                           mode, "figure 3")
    m = worker_moment(str(out), 0, z0, z1 - 1, False, 0.0, False)
    return np.frombuffer(base64.b64decode(m["data_base64"]), dtype=np.float32).reshape(
        m["height"], m["width"])


def main():
    for label, path in (("cube (set $VISIVO_VALIDATION_CUBE)", CUBE), ("SoFiA mask", MASK)):
        if not Path(path).is_file():
            sys.exit(f"error: {label} not found: {path}")
    OUT.mkdir(exist_ok=True)
    data = np.squeeze(fits.getdata(CUBE)).astype(np.float32)
    sofia = np.squeeze(fits.getdata(MASK)).astype(np.int32)
    labels, counts, order = rank_sources(sofia, 1)
    lab = int(labels[order[0]])                                # the biggest galaxy
    src = sofia == lab
    zz, yy, xx = np.where(src)
    k = int(np.argmax(data[zz, yy, xx]))
    seed = (int(zz[k]), int(yy[k]), int(xx[k]))

    # crop to the galaxy for display
    pad = 25
    y0, y1 = max(0, yy.min() - pad), min(data.shape[1], yy.max() + pad)
    x0, x1 = max(0, xx.min() - pad), min(data.shape[2], xx.max() + pad)
    zc0, zc1 = max(0, zz.min() - 5), min(data.shape[0], zz.max() + 5)
    crop = lambda m: m[y0:y1, x0:x1]

    sel = cube_kinematic_select(data, seed, reach=8.0, lambda_v=1.0, nsigma=2.5)
    lo = cube_kinematic_select(data, seed, reach=8.0, lambda_v=0.35, nsigma=2.5)
    hi = cube_kinematic_select(data, seed, reach=8.0, lambda_v=1.6, nsigma=2.5)
    print(f"galaxy #{lab} seed {seed}: sel={int(sel.sum())} "
          f"loλv={int(lo.sum())} hiλv={int(hi.sum())} vox")

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(2, 2, figsize=(10, 8.6))

    # (a) the selection outlined on its channel map — the seed's channel — as the
    # viewer's 2-D view draws it on whichever channel is shown
    zs = seed[0]
    ax[0, 0].imshow(crop(data[zs]), origin="lower", cmap="gray_r")
    ax[0, 0].contour(crop(sel[zs]), [0.5], colors="tab:green", linewidths=1.4)
    ax[0, 0].set_title(f"(a) selection on its channel map (channel {zs + 1})")

    # (c) kinematic knob λv: per-channel selected area
    zc = np.arange(data.shape[0]) + 1          # channels from 1, as the viewer numbers them
    area = lambda m: m.sum((1, 2))
    ax[0, 1].plot(zc, area(lo), color="tab:blue", label="low λv = 0.35")
    ax[0, 1].plot(zc, area(sel), color="gray", ls=":", label="λv = 1.0")
    ax[0, 1].plot(zc, area(hi), color="tab:red", label="high λv = 1.6")
    ax[0, 1].set_xlim(zz.min() - 7, zz.max() + 9)
    ax[0, 1].set_xlabel("channel"); ax[0, 1].set_ylabel("selected pixels")
    ax[0, 1].set_title("(b) the lasso panel's profile at three λv"); ax[0, 1].legend()

    # (c) Isolate, then Moment 0; (d) Remove, then Moment 0 — two full-size cubes,
    # written where they are removed again whatever happens
    with tempfile.TemporaryDirectory(prefix="figure3_") as tmp:
        work = Path(tmp)
        iso = app_moment0(sel, "blank_outside", zc0, zc1, work)
        res = app_moment0(sel, "blank_inside", zc0, zc1, work)
    im = ax[1, 0].imshow(crop(iso), origin="lower", cmap="magma")
    ax[1, 0].set_title("(c) isolate → galaxy on blank background")
    fig.colorbar(im, ax=ax[1, 0], fraction=0.046)

    im2 = ax[1, 1].imshow(crop(res), origin="lower", cmap="magma")
    ax[1, 1].set_title("(d) remove → residual")
    fig.colorbar(im2, ax=ax[1, 1], fraction=0.046)
    for a in (ax[0, 0], ax[1, 0], ax[1, 1]):
        a.set_xticks([]); a.set_yticks([])

    fig.suptitle(f"Kinematic lasso on an HCG 44 HI galaxy "
                 f"(one double-click, {int(sel.sum())} voxels)", y=0.995)
    fig.tight_layout(rect=(0, 0, 1, 0.98))
    fig.savefig(OUT / "figure3_lasso.png", dpi=130)
    print("Wrote", OUT / "figure3_lasso.png")


if __name__ == "__main__":
    main()
