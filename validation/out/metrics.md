# Kinematic Lasso vs SoFiA-2 — HGC 44 (HI observation, VELO-HEL)

compact group: several galaxies of comparable brightness in one field.


Cube `hgc44_3axes.fits` (158, 700, 700), beam 30.7 px, Δv 8.24 km/s, rms 0.0002247 (1.4826·MAD), spectral correlation factor F = 3.45. Lasso defaults reach=8.0, λv=1.0, nσ=2.5. Seed = each SoFiA source's peak voxel.


Flux errors are σ_F = rms·√(N·F/B)·Δv: N voxels inside a beam of B pixels carry only N/B independent spatial samples, and F is the measured spectral-correlation penalty (1 + 2·Σρ_k over emission-free voxels). The Δ column is the flux DIFFERENCE with the uncertainty of the SYMMETRIC DIFFERENCE alone — the shared intersection cancels exactly in the difference, so this is exact rather than an approximation.

| SoFiA id | SoFiA vox | Lasso vox | Dice | IoU | Compl. | Relia. | F_SoFiA | F_Lasso | ratio | Δflux | σ_Δ | n_σ |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 5 | 11463 | 13216 | 0.844 | 0.731 | 0.909 | 0.788 | 5.80 ± 0.07 | 6.06 ± 0.07 | 1.046 | +0.27 | 0.04 | 6.9 |
| 7 | 8754 | 10675 | 0.826 | 0.704 | 0.917 | 0.752 | 2.88 ± 0.06 | 3.13 ± 0.06 | 1.088 | +0.25 | 0.04 | 7.0 |
| 12 | 4995 | 5470 | 0.737 | 0.584 | 0.772 | 0.705 | 0.97 ± 0.04 | 1.08 ± 0.05 | 1.120 | +0.12 | 0.03 | 3.6 |
| 14 | 4738 | 3697 | 0.660 | 0.492 | 0.587 | 0.753 | 0.85 ± 0.04 | 0.68 ± 0.04 | 0.790 | -0.18 | 0.03 | 5.4 |
| 8 | 4207 | 3187 | 0.687 | 0.523 | 0.604 | 0.797 | 0.56 ± 0.04 | 0.52 ± 0.04 | 0.938 | -0.03 | 0.03 | 1.2 |

**Median Dice 0.737, median flux ratio 1.046, median |Δflux| 5.4σ.**
