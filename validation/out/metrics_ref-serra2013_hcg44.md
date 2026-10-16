# Kinematic Lasso vs serra2013_hcg44 — HGC 44 (HI observation, VELO-HEL)

compact group: several galaxies of comparable brightness in one field.


Cube `hgc44_3axes.fits` (158, 700, 700), beam 30.7 px, Δv 8.24 km/s, rms 0.0002247 (1.4826·MAD), spectral correlation factor F = 3.45 (analytic column only). Lasso defaults reach=8.0, λv=1.0, nσ=2.5. Seed = each reference source's peak voxel.


Flux uncertainties are estimated empirically rather than modelled: the mask's own stencil (and, for Δ, the SIGNED stencil +1 lasso / −1 reference) is dropped at positions that are FULLY outside a dilated reference mask, and σ is the scatter of the resulting weighted sums. This avoids the analytic covariance model, at the price of assuming those off-source placements represent the noise and covariance at the source's own position and channel range. `σ_Δ analytic` is the rms·√(N·F/B)·Δv model shown for comparison only: wherever it can be checked it overestimates σ_Δ (the disagreeing voxels share a mask boundary, so the signed sum cancels much of their noise), and on the one stencil that spans the field it cannot be checked at all.


`n_σ` is a fixed-mask, noise-equivalent contrast, NOT a p-value: both masks were selected on the same noisy data they are integrated over, so boundary voxels carry selection bias that this does not model.

| reference id | reference vox | Lasso vox | Dice | IoU | Compl. | Relia. | F_reference | F_Lasso | ratio | Δflux | σ_Δ | σ_Δ analytic | n_σ | clean Δ placements |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 13 | 28563 | 13216 | 0.610 | 0.439 | 0.446 | 0.965 | 6.09 ± 0.13 | 6.06 ± 0.07 | 0.995 | -0.03 | 0.07 | 0.08 | 0.4 | 200 |
| 1 | 19256 | 10675 | 0.693 | 0.530 | 0.538 | 0.971 | 3.19 ± 0.10 | 3.13 ± 0.06 | 0.981 | -0.06 | 0.05 | 0.06 | 1.2 | 200 |
| 8 | 14563 | 5470 | 0.516 | 0.347 | 0.355 | 0.944 | 1.26 ± 0.08 | 1.08 ± 0.04 | 0.858 | -0.18 | 0.06 | 0.06 | 3.2 | 200 |
| 3 | 10964 | 3697 | 0.474 | 0.311 | 0.317 | 0.941 | 1.08 ± 0.07 | 0.68 ± 0.03 | 0.625 | -0.41 | 0.05 | 0.05 | 8.0 | 200 |
| 7 | 8396 | 3187 | 0.525 | 0.356 | 0.362 | 0.954 | 0.74 ± 0.06 | 0.52 ± 0.03 | 0.710 | -0.21 | 0.04 | 0.05 | 5.3 | 200 |

**Median Dice 0.525, median flux ratio 0.858, median |Δflux| 3.2 noise-equivalent σ over the 5/5 sources whose scatter is measurable.**
