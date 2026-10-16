# Kinematic Lasso vs SoFiA-2 — WALLABY (ASKAP, RA---SIN / VOPT)

different instrument, projection and spectral convention; one large velocity-gradient source plus faint compact ones.


Cube `WALLABY.fits` (109, 280, 196), beam 28.5 px, Δv 3.94 km/s, rms 0.001969 (1.4826·MAD), spectral correlation factor F = 1.09 (analytic column only). Lasso defaults reach=8.0, λv=1.0, nσ=2.5. Seed = each reference source's peak voxel.


Flux uncertainties are estimated empirically rather than modelled: the mask's own stencil (and, for Δ, the SIGNED stencil +1 lasso / −1 SoFiA) is dropped at positions that are FULLY outside a dilated SoFiA mask, and σ is the scatter of the resulting weighted sums. This avoids the analytic covariance model, at the price of assuming those off-source placements represent the noise and covariance at the source's own position and channel range. `σ_Δ analytic` is the rms·√(N·F/B)·Δv model shown for comparison only: wherever it can be checked it overestimates σ_Δ (the disagreeing voxels share a mask boundary, so the signed sum cancels much of their noise), and on the one stencil that spans the field it cannot be checked at all.


`n_σ` is a fixed-mask, noise-equivalent contrast, NOT a p-value: both masks were selected on the same noisy data they are integrated over, so boundary voxels carry selection bias that this does not model.

| SoFiA id | SoFiA vox | Lasso vox | Dice | IoU | Compl. | Relia. | F_SoFiA | F_Lasso | ratio | Δflux | σ_Δ | σ_Δ analytic | n_σ | clean Δ placements |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 153227 | 68909 | 0.614 | 0.443 | 0.445 | 0.990 | 133.82 ± — | 116.25 ± 0.69 | 0.869 | -17.57 | — | 0.44 | — | 0 |
| 10 | 1189 | 136 | 0.171 | 0.093 | 0.095 | 0.831 | 0.32 ± 0.05 | 0.07 ± 0.01 | 0.226 | -0.25 | 0.04 | 0.05 | 6.3 | 200 |
| 11 | 1117 | 159 | 0.053 | 0.027 | 0.030 | 0.214 | 0.30 ± 0.05 | 0.09 ± 0.01 | 0.307 | -0.21 | 0.04 | 0.05 | 5.0 | 200 |
| 12 | 977 | 121 | 0.117 | 0.062 | 0.066 | 0.529 | 0.27 ± 0.04 | 0.07 ± 0.01 | 0.264 | -0.20 | 0.04 | 0.05 | 5.2 | 200 |
| 4 | 539 | 32 | 0.063 | 0.033 | 0.033 | 0.562 | 0.15 ± 0.03 | 0.02 ± 0.01 | 0.111 | -0.13 | 0.03 | 0.04 | 4.8 | 200 |

A dash means the stencil is too large to place anywhere fully source-free in this cube, so its empty-sky scatter is not measurable here — reported as such rather than substituted with the analytic model, which the other rows show overestimates σ_Δ by up to a factor of two and is untested in exactly this regime.

**Median Dice 0.117, median flux ratio 0.264, median |Δflux| 5.1 noise-equivalent σ over the 4/5 sources whose scatter is measurable.**
