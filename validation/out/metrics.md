# Kinematic Lasso vs SoFiA-2 — HGC 44 (HI observation, VELO-HEL)

compact group: several galaxies of comparable brightness in one field.


Cube `hgc44_3axes.fits` (158, 700, 700), beam 30.7 px, Δv 8.24 km/s. Lasso defaults reach=8.0, λv=1.0, nσ=2.5. Seed = each SoFiA source's peak voxel.

| SoFiA id | SoFiA vox | Lasso vox | Dice | IoU | Compl. | Relia. | F_SoFiA | F_Lasso | ratio |
|---|---|---|---|---|---|---|---|---|---|
| 5 | 11463 | 13216 | 0.844 | 0.731 | 0.909 | 0.788 | 5.80 | 6.06 | 1.046 |
| 7 | 8754 | 10675 | 0.826 | 0.704 | 0.917 | 0.752 | 2.88 | 3.13 | 1.088 |
| 12 | 4995 | 5470 | 0.737 | 0.584 | 0.772 | 0.705 | 0.97 | 1.08 | 1.120 |
| 14 | 4738 | 3697 | 0.660 | 0.492 | 0.587 | 0.753 | 0.85 | 0.68 | 0.790 |
| 8 | 4207 | 3187 | 0.687 | 0.523 | 0.604 | 0.797 | 0.56 | 0.52 | 0.938 |

**Median Dice 0.737, median flux ratio 1.046.**
