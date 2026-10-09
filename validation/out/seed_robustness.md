# Kinematic Lasso — seed (click) robustness — HGC 44 HI cube

Cube `hgc44_3axes.fits` (158, 700, 700). Lasso defaults reach=8.0, λv=1.0, nσ=2.5, rng=20260827. Per source: an INTERIOR stratum (peak + spatial jitter + random bright interior voxels — the plausible correct click) drives the headline; a separate STRESS stratum (low-intensity faint interior voxels + a one-voxel dilation ring just outside) is reported apart.

| SoFiA id | interior seeds | Dice peak | Dice mean | Dice std | Dice CV | [min, max] | compl mean±std | relia mean±std | stress Dice mean/min |
|---|---|---|---|---|---|---|---|---|---|
| 5 | 19 | 0.844 | 0.844 | 0.000 | 0.000 | [0.844, 0.844] | 0.909±0.000 | 0.788±0.000 | 0.311 / 0.000 |
| 7 | 19 | 0.826 | 0.826 | 0.000 | 0.000 | [0.826, 0.826] | 0.917±0.000 | 0.752±0.000 | 0.384 / 0.000 |
| 12 | 19 | 0.737 | 0.734 | 0.006 | 0.009 | [0.720, 0.739] | 0.755±0.029 | 0.715±0.019 | 0.174 / 0.000 |
| 14 | 19 | 0.660 | 0.606 | 0.108 | 0.178 | [0.377, 0.670] | 0.524±0.117 | 0.727±0.066 | 0.222 / 0.000 |
| 8 | 19 | 0.687 | 0.659 | 0.042 | 0.064 | [0.503, 0.688] | 0.557±0.060 | 0.817±0.029 | 0.142 / 0.016 |

**Interior stratum: median Dice std = 0.006, worst-case std = 0.108, worst single-seed Dice = 0.377; 2/5 sources click-invariant (Dice std < 0.001).** For the bright, well-connected sources the recovered region is essentially click-invariant — the object-bounded grow takes the connected component *containing the seed*, the same from any interior voxel — so their lasso-vs-SoFiA agreement is not an artifact of seeding on SoFiA's peak voxel. The two fainter sources show genuine placement sensitivity (a marginal interior click can drop Dice to 0.377), so for weakly-detected objects the result depends more on where the user clicks. The separate STRESS stratum (worst min Dice 0.000) shows a deliberately faint / off-source click degrades sharply, as expected.

_Scope: a single-cube, 5-source sanity check — it shows the peak-seed comparison is not driven by exact peak clicks for well-connected sources; it does not by itself establish general seed robustness across all cubes._
