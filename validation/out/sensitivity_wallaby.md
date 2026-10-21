# Lasso parameter sensitivity — WALLABY (ASKAP, RA---SIN / FREQ)

Completeness (∩/SoFiA), reliability (∩/lasso) and Dice at every setting of the two interactive controls, for the largest and the 4th-largest SoFiA source. Defaults are nσ=2.5, reach=8; the other control is held at its default while one is swept.


## Sweeping nσ

| source | nσ | Dice | completeness | reliability | lasso vox |
|---|---|---|---|---|---|
| #1 (largest) | 1.5 | 0.747 | 0.630 | 0.916 | 105450 |
| #1 (largest) | 2 | 0.676 | 0.518 | 0.973 | 81554 |
| #1 (largest) | 2.5 | 0.614 | 0.445 | 0.990 | 68909 |
| #1 (largest) | 3 | 0.562 | 0.392 | 0.997 | 60219 |
| #1 (largest) | 3.5 | 0.519 | 0.351 | 0.999 | 53757 |
| #12 (4th-largest) | 1.5 | 0.295 | 0.277 | 0.314 | 863 |
| #12 (4th-largest) | 2 | 0.172 | 0.113 | 0.361 | 305 |
| #12 (4th-largest) | 2.5 | 0.117 | 0.066 | 0.529 | 121 |
| #12 (4th-largest) | 3 | 0.058 | 0.031 | 0.566 | 53 |
| #12 (4th-largest) | 3.5 | 0.034 | 0.017 | 0.607 | 28 |

## Sweeping reach

| source | reach | Dice | completeness | reliability | lasso vox |
|---|---|---|---|---|---|
| #1 (largest) | 4 | 0.545 | 0.375 | 0.998 | 57559 |
| #1 (largest) | 8 | 0.614 | 0.445 | 0.990 | 68909 |
| #1 (largest) | 16 | 0.707 | 0.560 | 0.958 | 89510 |
| #1 (largest) | 32 | 0.763 | 0.724 | 0.807 | 137452 |
| #1 (largest) | 64 | 0.596 | 0.810 | 0.472 | 263097 |
| #12 (4th-largest) | 4 | 0.045 | 0.024 | 0.561 | 41 |
| #12 (4th-largest) | 8 | 0.117 | 0.066 | 0.529 | 121 |
| #12 (4th-largest) | 16 | 0.233 | 0.173 | 0.358 | 472 |
| #12 (4th-largest) | 32 | 0.285 | 0.418 | 0.216 | 1886 |
| #12 (4th-largest) | 64 | 0.176 | 0.985 | 0.097 | 9963 |

**Best Dice per source and axis**

| source | axis | best value | Dice | completeness | reliability |
|---|---|---|---|---|---|
| #1 | nsigma | 1.5 | 0.747 | 0.630 | 0.916 |
| #1 | reach | 32 | 0.763 | 0.724 | 0.807 |
| #12 | nsigma | 1.5 | 0.295 | 0.277 | 0.314 |
| #12 | reach | 32 | 0.285 | 0.418 | 0.216 |
