#!/usr/bin/env python
"""Velocity-field use case (F1) — cosmic-flows peculiar-velocity field.

Demonstrates VisIVO's velocity-field pipeline on a real Cosmicflows-4 (CF4)
reconstructed peculiar-velocity field: streamlines of the flow coloured by speed,
and the gravitational **basins of attraction** (the watershed segmentation behind
the Laniakea-style partition of the local Universe). Uses the actual backend
compute (`load_velocity_field`, `compute_basins`).

Run:  backend/.venv/bin/python validation/velocity_field_demo.py
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "backend"))
from app.velocity_field import load_velocity_field   # noqa: E402
from app.velocity_basins import compute_basins        # noqa: E402

FIELD = "/Users/fvitello/Downloads/CF4_new_64-z008_velocity.fits"
OUT = Path(__file__).resolve().parent / "out"


def main():
    OUT.mkdir(exist_ok=True)
    field = load_velocity_field(FIELD)
    nz, ny, nx = field.shape
    speed = field.magnitude
    labels, n_basins = compute_basins(field, direction=1)
    print(f"CF4 field {field.shape}, |v| max {speed.max():.1f} km/s, "
          f"{n_basins} basins of attraction")

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    zc = nz // 2
    x, y = np.arange(nx), np.arange(ny)

    fig, ax = plt.subplots(1, 2, figsize=(11, 4.6))

    # ── flow streamlines coloured by speed ──
    sp = speed[zc]
    U, V = field.vx[zc], field.vy[zc]
    im = ax[0].imshow(sp, origin="lower", cmap="magma")
    lw = 0.6 + 2.0 * (sp / sp.max())
    ax[0].streamplot(x, y, U, V, color="white", density=1.3, linewidth=lw,
                     arrowsize=0.7)
    ax[0].set_title(f"CF4 peculiar-velocity flow (slice z={zc})")
    ax[0].set_xticks([]); ax[0].set_yticks([])
    cb = fig.colorbar(im, ax=ax[0], fraction=0.046); cb.set_label("|v| (km/s)")

    # ── gravitational basins of attraction (watershed) ──
    from matplotlib.colors import ListedColormap
    rng = np.random.default_rng(0)
    cols = rng.random((max(n_basins + 1, 2), 3)) * 0.75 + 0.2
    cols[0] = (0.1, 0.1, 0.1)
    ax[1].imshow(labels[zc], origin="lower", cmap=ListedColormap(cols),
                 interpolation="nearest")
    ax[1].set_title(f"Gravitational basins of attraction ({n_basins} basins)")
    ax[1].set_xticks([]); ax[1].set_yticks([])

    fig.tight_layout()
    fig.savefig(OUT / "velocity_field.png", dpi=130)
    print("Wrote", OUT / "velocity_field.png")


if __name__ == "__main__":
    main()
