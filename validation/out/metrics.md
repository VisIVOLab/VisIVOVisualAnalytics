# Kinematic Lasso vs SoFiA-2 — HGC 44 (HI observation, VELO-HEL)

compact group: several galaxies of comparable brightness in one field.


Cube `hgc44_3axes.fits` (158, 700, 700), beam 30.7 px, Δv 8.24 km/s, rms 0.0002247 (1.4826·MAD), spectral correlation factor F = 3.45 (analytic column only). Lasso defaults reach=8.0, λv=1.0, nσ=2.5. Seed = each reference source's peak voxel.


Flux uncertainties are estimated empirically rather than modelled: the mask's own stencil (and, for Δ, the SIGNED stencil +1 lasso / −1 SoFiA) is dropped at positions that are FULLY outside a dilated SoFiA mask, and σ is the scatter of the resulting weighted sums. This avoids the analytic covariance model, at the price of assuming those off-source placements represent the noise and covariance at the source's own position and channel range. `σ_Δ analytic` is the rms·√(N·F/B)·Δv model shown for comparison only: wherever it can be checked it overestimates σ_Δ (the disagreeing voxels share a mask boundary, so the signed sum cancels much of their noise), and on the one stencil that spans the field it cannot be checked at all.


`n_σ` is a fixed-mask, noise-equivalent contrast, NOT a p-value: both masks were selected on the same noisy data they are integrated over, so boundary voxels carry selection bias that this does not model.

| SoFiA id | SoFiA vox | Lasso vox | Dice | IoU | Compl. | Relia. | F_SoFiA | F_Lasso | ratio | Δflux | σ_Δ | σ_Δ analytic | n_σ | clean Δ placements |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 5 | 11463 | 13216 | 0.844 | 0.731 | 0.909 | 0.788 | 5.80 ± 0.07 | 6.06 ± 0.07 | 1.046 | +0.27 | 0.02 | 0.04 | 15.7 | 200 |
| 7 | 8754 | 10675 | 0.826 | 0.704 | 0.917 | 0.752 | 2.88 ± 0.06 | 3.13 ± 0.07 | 1.088 | +0.25 | 0.02 | 0.04 | 15.2 | 200 |
| 12 | 4995 | 5470 | 0.737 | 0.584 | 0.772 | 0.705 | 0.97 ± 0.04 | 1.08 ± 0.04 | 1.120 | +0.12 | 0.02 | 0.03 | 7.1 | 200 |
| 14 | 4738 | 3697 | 0.660 | 0.492 | 0.587 | 0.753 | 0.85 ± 0.04 | 0.68 ± 0.03 | 0.790 | -0.18 | 0.02 | 0.03 | 8.8 | 200 |
| 8 | 4207 | 3187 | 0.687 | 0.523 | 0.604 | 0.797 | 0.56 ± 0.04 | 0.52 ± 0.03 | 0.938 | -0.03 | 0.02 | 0.03 | 1.8 | 200 |

**Median Dice 0.737, median flux ratio 1.046, median |Δflux| 8.8 noise-equivalent σ over the 5/5 sources whose scatter is measurable.**
