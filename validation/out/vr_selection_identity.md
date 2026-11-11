# VR selection = desktop selection — HCG 44, NGC 3187 (issue #171)

The VR path: the reduced cube of `reduced_bin` (auto binning, mean, 8 bits, the page's default threshold), the page's own ray pick and seed (`selection.js`, run by node), the request the page builds (`recipe.js`). The desktop: the Reach slider's level field (`kinematic_levels_bin`, reach_max 64) thresholded at reach 8, and its recipe's `kinematic_select_bin` request. Lasso defaults reach 8, λv 1, nσ 2.5, σ measured by the service. Paper ranges for the default click: Dice 0.47–0.69 against Serra et al. (2013), 0.66–0.84 against SoFiA-2.

| case | seed (x, y, ch) | voxels | Dice VR / desktop shown | Dice VR / desktop recipe | spectrum = desktop | spectrum = cube | Dice vs Serra+2013 | Dice vs SoFiA-2 | flux (Jy km/s) | velocity (km/s) |
|---|---|---|---|---|---|---|---|---|---|---|
| VR pick, ray along the spectral axis | (465, 299, 74) | 13216 | 1.0000 | 1.0000 | yes | yes | 0.610 (component 13) | 0.844 | 6.061 | 1391.5 to 1729.5 |
| VR pick, ray from the default view | (467, 298, 85) | 13216 | 1.0000 | 1.0000 | yes | yes | 0.610 (component 13) | 0.844 | 6.061 | 1391.5 to 1729.5 |
| VR pick, default view, seed at the first drawn sample | (461, 300, 91) | 13216 | 1.0000 | 1.0000 | yes | yes | 0.610 (component 13) | 0.844 | 6.061 | 1391.5 to 1729.5 |
| the paper's seed (SoFiA-2 source peak) | (460, 299, 75) | 13216 | 1.0000 | 1.0000 | yes | yes | 0.610 (component 13) | 0.844 | 6.061 | 1391.5 to 1729.5 |

- VR pick, ray along the spectral axis: the ray's first drawn sample is in pixel (465, 298, 98); the seed's reduced voxel is (155, 99, 74) (from pixel 465, 298, 74), the seed its block's brightest voxel.
- VR pick, ray from the default view: the ray's first drawn sample is in pixel (461, 301, 91); the seed's reduced voxel is (155, 99, 85) (from pixel 465, 298, 85), the seed its block's brightest voxel.
- VR pick, default view, seed at the first drawn sample: the ray's first drawn sample is in pixel (461, 301, 91); the seed's reduced voxel is (153, 100, 91) (from pixel 461, 301, 91), the seed its block's brightest voxel.
