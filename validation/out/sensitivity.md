# Lasso parameter sensitivity — HGC 44 (HI observation, VELO-HEL)

Completeness (∩/SoFiA), reliability (∩/lasso) and Dice at every setting of the two interactive controls, for the largest and the 4th-largest SoFiA source. Defaults are nσ=2.5, reach=8; the other control is held at its default while one is swept.


## Sweeping nσ

| source | nσ | Dice | completeness | reliability | lasso vox |
|---|---|---|---|---|---|
| #5 (largest) | 1.5 | 0.661 | 0.987 | 0.498 | 22730 |
| #5 (largest) | 2 | 0.785 | 0.955 | 0.666 | 16442 |
| #5 (largest) | 2.5 | 0.844 | 0.909 | 0.788 | 13216 |
| #5 (largest) | 3 | 0.860 | 0.852 | 0.868 | 11249 |
| #5 (largest) | 3.5 | 0.856 | 0.796 | 0.925 | 9860 |
| #14 (4th-largest) | 1.5 | 0.581 | 0.971 | 0.415 | 11095 |
| #14 (4th-largest) | 2 | 0.720 | 0.893 | 0.603 | 7015 |
| #14 (4th-largest) | 2.5 | 0.660 | 0.587 | 0.753 | 3697 |
| #14 (4th-largest) | 3 | 0.616 | 0.480 | 0.861 | 2643 |
| #14 (4th-largest) | 3.5 | 0.559 | 0.399 | 0.933 | 2028 |

## Sweeping reach

| source | reach | Dice | completeness | reliability | lasso vox |
|---|---|---|---|---|---|
| #5 (largest) | 4 | 0.862 | 0.832 | 0.893 | 10684 |
| #5 (largest) | 8 | 0.844 | 0.909 | 0.788 | 13216 |
| #5 (largest) | 16 | 0.747 | 0.971 | 0.607 | 18336 |
| #5 (largest) | 32 | 0.526 | 0.998 | 0.358 | 31973 |
| #5 (largest) | 64 | 0.238 | 1.000 | 0.135 | 84804 |
| #14 (4th-largest) | 4 | 0.598 | 0.449 | 0.897 | 2372 |
| #14 (4th-largest) | 8 | 0.660 | 0.587 | 0.753 | 3697 |
| #14 (4th-largest) | 16 | 0.692 | 0.876 | 0.571 | 7267 |
| #14 (4th-largest) | 32 | 0.430 | 0.995 | 0.274 | 17184 |
| #14 (4th-largest) | 64 | 0.174 | 1.000 | 0.095 | 49648 |

**Best Dice per source and axis**

| source | axis | best value | Dice | completeness | reliability |
|---|---|---|---|---|---|
| #5 | nsigma | 3 | 0.860 | 0.852 | 0.868 |
| #5 | reach | 4 | 0.862 | 0.832 | 0.893 |
| #14 | nsigma | 2 | 0.720 | 0.893 | 0.603 |
| #14 | reach | 16 | 0.692 | 0.876 | 0.571 |
