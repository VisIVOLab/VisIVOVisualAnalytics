# Kinematic Lasso vs serra2013_hcg44 — HGC 44 (HI observation, VELO-HEL)

compact group: several galaxies of comparable brightness in one field.


Cube `hgc44_3axes.fits` (158, 700, 700), beam 30.7 px, Δv 8.24 km/s, rms 0.0002247 (1.4826·MAD), spectral correlation factor F = 3.45 (analytic column only). Lasso defaults reach=8.0, λv=1.0, nσ=2.5; smooth-before-threshold σ(z,y,x) = (1, 3, 3). Seed = each reference source's peak voxel.


Flux uncertainties are estimated empirically rather than modelled: the mask's own stencil (and, for Δ, the SIGNED stencil +1 lasso / −1 reference) is dropped at positions that are FULLY outside a dilated reference mask, and σ is the scatter of the resulting weighted sums. This avoids the analytic covariance model, at the price of assuming those off-source placements represent the noise and covariance at the source's own position and channel range. `σ_Δ analytic` is the rms·√(N·F/B)·Δv model shown for comparison only: wherever it can be checked it overestimates σ_Δ (the disagreeing voxels share a mask boundary, so the signed sum cancels much of their noise), and on the one stencil that spans the field it cannot be checked at all.


`n_σ` is a fixed-mask, noise-equivalent contrast, NOT a p-value: both masks were selected on the same noisy data they are integrated over, so boundary voxels carry selection bias that this does not model.

| reference id | reference vox | Lasso vox | Dice | IoU | Compl. | Relia. | F_reference | F_Lasso | ratio | Δflux | σ_Δ | σ_Δ analytic | n_σ | clean Δ placements |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 13 | 28563 | 27177 | 0.728 | 0.572 | 0.710 | 0.746 | 6.09 ± 0.13 | 6.44 ± 0.11 | 1.057 | +0.35 | 0.07 | 0.08 | 5.3 | 200 |
| 1 | 19256 | 24161 | 0.751 | 0.601 | 0.846 | 0.674 | 3.19 ± 0.10 | 3.52 ± 0.10 | 1.103 | +0.33 | 0.04 | 0.06 | 7.5 | 200 |
| 8 | 14563 | 15140 | 0.645 | 0.477 | 0.658 | 0.633 | 1.26 ± 0.08 | 1.57 ± 0.06 | 1.240 | +0.30 | 0.05 | 0.06 | 6.6 | 200 |
| 3 | 10964 | 12964 | 0.733 | 0.579 | 0.800 | 0.677 | 1.08 ± 0.07 | 1.34 ± 0.07 | 1.239 | +0.26 | 0.03 | 0.05 | 7.7 | 200 |
| 7 | 8396 | 10293 | 0.708 | 0.548 | 0.788 | 0.642 | 0.74 ± 0.06 | 0.94 ± 0.06 | 1.277 | +0.20 | 0.03 | 0.05 | 6.4 | 200 |

**Median Dice 0.728, median flux ratio 1.239, median |Δflux| 6.6 noise-equivalent σ over the 5/5 sources whose scatter is measurable.**
