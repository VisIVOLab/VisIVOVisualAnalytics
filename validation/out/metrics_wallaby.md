# Kinematic Lasso vs SoFiA-2 — WALLABY (ASKAP, RA---SIN / VOPT)

different instrument, projection and spectral convention; one large velocity-gradient source plus faint compact ones.


Cube `WALLABY.fits` (109, 280, 196), beam 28.5 px, Δv 3.94 km/s, rms 0.002004 (1.4826·MAD), spectral correlation factor F = 1.09. Lasso defaults reach=8.0, λv=1.0, nσ=2.5. Seed = each SoFiA source's peak voxel.


Flux errors are σ_F = rms·√(N·F/B)·Δv: N voxels inside a beam of B pixels carry only N/B independent spatial samples, and F is the measured spectral-correlation penalty (1 + 2·Σρ_k over emission-free voxels). The Δ column is the flux DIFFERENCE with the uncertainty of the SYMMETRIC DIFFERENCE alone — the shared intersection cancels exactly in the difference, so this is exact rather than an approximation.

| SoFiA id | SoFiA vox | Lasso vox | Dice | IoU | Compl. | Relia. | F_SoFiA | F_Lasso | ratio | Δflux | σ_Δ | n_σ |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 153227 | 68909 | 0.614 | 0.443 | 0.445 | 0.990 | 133.82 ± 0.61 | 116.25 ± 0.41 | 0.869 | -17.57 | 0.45 | 38.8 |
| 10 | 1189 | 136 | 0.171 | 0.093 | 0.095 | 0.831 | 0.32 ± 0.05 | 0.07 ± 0.02 | 0.226 | -0.25 | 0.05 | 4.9 |
| 11 | 1117 | 159 | 0.053 | 0.027 | 0.030 | 0.214 | 0.30 ± 0.05 | 0.09 ± 0.02 | 0.307 | -0.21 | 0.05 | 3.8 |
| 12 | 977 | 121 | 0.117 | 0.062 | 0.066 | 0.529 | 0.27 ± 0.05 | 0.07 ± 0.02 | 0.264 | -0.20 | 0.05 | 4.1 |
| 4 | 539 | 32 | 0.063 | 0.033 | 0.033 | 0.562 | 0.15 ± 0.04 | 0.02 ± 0.01 | 0.111 | -0.13 | 0.04 | 3.7 |

**Median Dice 0.117, median flux ratio 0.264, median |Δflux| 4.1σ.**
