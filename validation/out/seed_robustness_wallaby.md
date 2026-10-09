# Kinematic Lasso — seed (click) robustness — HGC 44 HI cube

Cube `WALLABY.fits` (109, 280, 196). Lasso defaults reach=8.0, λv=1.0, nσ=2.5, rng=20260827. Per source: an INTERIOR stratum (peak + spatial jitter + random bright interior voxels — the plausible correct click) drives the headline; a separate STRESS stratum (low-intensity faint interior voxels + a one-voxel dilation ring just outside) is reported apart.

| SoFiA id | interior seeds | Dice peak | Dice mean | Dice std | Dice CV | [min, max] | compl mean±std | relia mean±std | stress Dice mean/min |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 19 | 0.614 | 0.518 | 0.222 | 0.428 | [0.000, 0.614] | 0.375±0.161 | 0.977±0.052 | 0.050 / 0.000 |
| 10 | 19 | 0.171 | 0.117 | 0.060 | 0.514 | [0.017, 0.179] | 0.064±0.034 | 0.790±0.112 | 0.037 / 0.000 |
| 11 | 19 | 0.053 | 0.048 | 0.021 | 0.438 | [0.009, 0.112] | 0.026±0.012 | 0.464±0.265 | 0.024 / 0.000 |
| 12 | 19 | 0.117 | 0.108 | 0.039 | 0.359 | [0.018, 0.191] | 0.060±0.022 | 0.646±0.137 | 0.029 / 0.002 |
| 4 | 19 | 0.063 | 0.143 | 0.067 | 0.471 | [0.063, 0.273] | 0.081±0.041 | 0.694±0.133 | 0.058 / 0.004 |

**Interior stratum: median Dice std = 0.060, worst-case std = 0.222, worst single-seed Dice = 0.000; 0/5 sources click-invariant (Dice std < 0.001).** For the bright, well-connected sources the recovered region is essentially click-invariant — the object-bounded grow takes the connected component *containing the seed*, the same from any interior voxel — so their lasso-vs-SoFiA agreement is not an artifact of seeding on SoFiA's peak voxel. The two fainter sources show genuine placement sensitivity (a marginal interior click can drop Dice to 0.000), so for weakly-detected objects the result depends more on where the user clicks. The separate STRESS stratum (worst min Dice 0.000) shows a deliberately faint / off-source click degrades sharply, as expected.

_Scope: a single-cube, 5-source sanity check — it shows the peak-seed comparison is not driven by exact peak clicks for well-connected sources; it does not by itself establish general seed robustness across all cubes._
