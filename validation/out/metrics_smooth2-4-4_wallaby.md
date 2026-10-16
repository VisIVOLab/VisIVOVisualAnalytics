# Kinematic Lasso vs SoFiA-2 — WALLABY (ASKAP, RA---SIN / VOPT)

different instrument, projection and spectral convention; one large velocity-gradient source plus faint compact ones.


Cube `WALLABY.fits` (109, 280, 196), beam 28.5 px, Δv 3.94 km/s, rms 0.001969 (1.4826·MAD), spectral correlation factor F = 1.09 (analytic column only). Lasso defaults reach=8.0, λv=1.0, nσ=2.5; smooth-before-threshold σ(z,y,x) = (2, 4, 4). Seed = each reference source's peak voxel.


Flux uncertainties are estimated empirically rather than modelled: the mask's own stencil (and, for Δ, the SIGNED stencil +1 lasso / −1 SoFiA) is dropped at positions that are FULLY outside a dilated SoFiA mask, and σ is the scatter of the resulting weighted sums. This avoids the analytic covariance model, at the price of assuming those off-source placements represent the noise and covariance at the source's own position and channel range. `σ_Δ analytic` is the rms·√(N·F/B)·Δv model shown for comparison only: wherever it can be checked it overestimates σ_Δ (the disagreeing voxels share a mask boundary, so the signed sum cancels much of their noise), and on the one stencil that spans the field it cannot be checked at all.


`n_σ` is a fixed-mask, noise-equivalent contrast, NOT a p-value: both masks were selected on the same noisy data they are integrated over, so boundary voxels carry selection bias that this does not model.

| SoFiA id | SoFiA vox | Lasso vox | Dice | IoU | Compl. | Relia. | F_SoFiA | F_Lasso | ratio | Δflux | σ_Δ | σ_Δ analytic | n_σ | clean Δ placements |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 153227 | 238872 | 0.773 | 0.630 | 0.989 | 0.634 | 133.82 ± — | 139.97 ± — | 1.046 | +6.15 | — | 0.45 | — | 0 |
| 10 | 1189 | 3973 | 0.451 | 0.291 | 0.980 | 0.293 | 0.32 ± 0.05 | 0.64 ± 0.10 | 1.982 | +0.32 | 0.07 | 0.08 | 4.7 | 200 |
| 11 | 1117 | 3596 | 0.381 | 0.235 | 0.803 | 0.249 | 0.30 ± 0.04 | 0.71 ± 0.09 | 2.371 | +0.41 | 0.08 | 0.08 | 5.4 | 200 |
| 12 | 977 | 3592 | 0.417 | 0.264 | 0.975 | 0.265 | 0.27 ± 0.04 | 0.64 ± 0.10 | 2.367 | +0.37 | 0.07 | 0.08 | 5.6 | 200 |
| 4 | 539 | 1651 | 0.440 | 0.282 | 0.894 | 0.292 | 0.15 ± 0.03 | 0.32 ± 0.06 | 2.146 | +0.17 | 0.04 | 0.05 | 4.4 | 200 |

A dash means the stencil is too large to place anywhere fully source-free in this cube, so its empty-sky scatter is not measurable here — reported as such rather than substituted with the analytic model, which the other rows show overestimates σ_Δ by up to a factor of two and is untested in exactly this regime.

**Median Dice 0.440, median flux ratio 2.146, median |Δflux| 5.0 noise-equivalent σ over the 4/5 sources whose scatter is measurable.**
