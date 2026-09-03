# Kinematic Lasso vs SoFiA-2 — WALLABY (ASKAP, RA---SIN / VOPT)

different instrument, projection and spectral convention; one large velocity-gradient source plus faint compact ones.


Cube `WALLABY.fits` (109, 280, 196), beam 28.5 px, Δv 3.94 km/s. Lasso defaults reach=8.0, λv=1.0, nσ=2.5. Seed = each SoFiA source's peak voxel.

| SoFiA id | SoFiA vox | Lasso vox | Dice | IoU | Compl. | Relia. | F_SoFiA | F_Lasso | ratio |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 153227 | 68909 | 0.614 | 0.443 | 0.445 | 0.990 | 133.82 | 116.25 | 0.869 |
| 10 | 1189 | 136 | 0.171 | 0.093 | 0.095 | 0.831 | 0.32 | 0.07 | 0.226 |
| 11 | 1117 | 159 | 0.053 | 0.027 | 0.030 | 0.214 | 0.30 | 0.09 | 0.307 |
| 12 | 977 | 121 | 0.117 | 0.062 | 0.066 | 0.529 | 0.27 | 0.07 | 0.264 |
| 4 | 539 | 32 | 0.063 | 0.033 | 0.033 | 0.562 | 0.15 | 0.02 | 0.111 |

**Median Dice 0.117, median flux ratio 0.264.**
