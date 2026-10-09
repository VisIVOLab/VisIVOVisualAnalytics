# Velocity-field use case (F1) — cosmic flows

Demonstration of VisIVO's peculiar-velocity-field pipeline on a real
**Cosmicflows-4 (CF4)** reconstructed field (`CF4_new_64-z008_velocity.fits`,
3-component field on a 64³ supergalactic grid, |v| up to ~50 km/s). Uses the
actual backend compute (`app.velocity_field.load_velocity_field`,
`app.velocity_basins.compute_basins`).

```bash
backend/.venv/bin/python validation/velocity_field_demo.py   # → out/velocity_field.png
```

`out/velocity_field.png`:

- **Left — flow streamlines coloured by speed.** The reconstructed peculiar
  velocities integrated into streamlines, revealing the convergent flow toward
  the local overdensities (higher |v| along the infall filaments).
- **Right — gravitational basins of attraction.** The watershed segmentation of
  the flow into **15 basins** — the method behind the Laniakea-style partition of
  the local Universe (Dupuy et al. / Courtois): each cell is integrated along the
  flow to its attractor and labelled by basin.

This is the F1 top-priority feature demonstrated end-to-end on real data — one of
the use cases that no competing desktop tool (CARTA / SlicerAstro / iDaVIE) covers.
