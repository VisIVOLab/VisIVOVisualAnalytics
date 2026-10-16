# Kinematic Lasso vs SoFiA-2 — HGC 44 (HI observation, VELO-HEL)

compact group: several galaxies of comparable brightness in one field.


Cube `hgc44_3axes.fits` (158, 700, 700), beam 30.7 px, Δv 8.24 km/s, rms 0.0002247 (1.4826·MAD), spectral correlation factor F = 3.45 (analytic column only). Lasso defaults reach=8.0, λv=1.0, nσ=2.5; smooth-before-threshold σ(z,y,x) = (1, 3, 3). Seed = each reference source's peak voxel.


Flux uncertainties are estimated empirically rather than modelled: the mask's own stencil (and, for Δ, the SIGNED stencil +1 lasso / −1 SoFiA) is dropped at positions that are FULLY outside a dilated SoFiA mask, and σ is the scatter of the resulting weighted sums. This avoids the analytic covariance model, at the price of assuming those off-source placements represent the noise and covariance at the source's own position and channel range. `σ_Δ analytic` is the rms·√(N·F/B)·Δv model shown for comparison only: wherever it can be checked it overestimates σ_Δ (the disagreeing voxels share a mask boundary, so the signed sum cancels much of their noise), and on the one stencil that spans the field it cannot be checked at all.


`n_σ` is a fixed-mask, noise-equivalent contrast, NOT a p-value: both masks were selected on the same noisy data they are integrated over, so boundary voxels carry selection bias that this does not model.

| SoFiA id | SoFiA vox | Lasso vox | Dice | IoU | Compl. | Relia. | F_SoFiA | F_Lasso | ratio | Δflux | σ_Δ | σ_Δ analytic | n_σ | clean Δ placements |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 5 | 11463 | 27177 | 0.593 | 0.422 | 1.000 | 0.422 | 5.80 ± 0.07 | 6.44 ± 0.11 | 1.111 | +0.65 | 0.06 | 0.08 | 10.0 | 200 |
| 7 | 8754 | 24161 | 0.532 | 0.362 | 1.000 | 0.362 | 2.88 ± 0.06 | 3.52 ± 0.11 | 1.222 | +0.64 | 0.07 | 0.08 | 8.6 | 200 |
| 12 | 4995 | 15140 | 0.493 | 0.327 | 0.993 | 0.328 | 0.97 ± 0.04 | 1.57 ± 0.07 | 1.619 | +0.60 | 0.05 | 0.06 | 12.3 | 200 |
| 14 | 4738 | 12964 | 0.534 | 0.365 | 0.998 | 0.365 | 0.85 ± 0.04 | 1.34 ± 0.07 | 1.567 | +0.48 | 0.05 | 0.06 | 10.0 | 200 |
| 8 | 4207 | 10293 | 0.572 | 0.401 | 0.986 | 0.403 | 0.56 ± 0.04 | 0.94 ± 0.06 | 1.688 | +0.38 | 0.04 | 0.05 | 9.2 | 200 |

**Median Dice 0.534, median flux ratio 1.567, median |Δflux| 10.0 noise-equivalent σ over the 5/5 sources whose scatter is measurable.**
