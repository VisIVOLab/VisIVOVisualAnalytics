# The kinematic lasso on synthetic discs

Discs built with the application's tilted-ring model (160×160×64 voxels, 7.9 km/s channels, PA 40°, i 60°, V_rot 120 km/s, exponential light of scale length = radius) in Gaussian noise. The shipped defaults are reach 8, λv 1, nσ 2.5; the noise scale is estimated from the data, as the application does. '2.5σ truth' is the noiseless disc above 2.5 σ of the injected noise, '1σ envelope' above 1 σ. Each cell is the median over 5 clicks (the peak and 4 random voxels above half the peak), with the range in brackets.

## Signal-to-noise and extent

| S/N | radius (px) | 2.5σ truth (vox) | Dice, defaults | completeness | reliability | best reach | Dice at best reach | completeness vs 1σ envelope (defaults) |
|---|---|---|---|---|---|---|---|---|
| 3 | 8 | 2 | 0.00 [0.00–0.29] | 0.00 [0.00–1.00] | 0.00 [0.00–0.17] | 32 | 0.01 [0.01–0.01] | 0.04 [0.02–0.09] |
| 5 | 8 | 92 | 0.26 [0.24–0.31] | 0.24 [0.24–0.32] | 0.25 [0.23–0.39] | 8 | 0.26 [0.24–0.31] | 0.14 [0.10–0.24] |
| 10 | 8 | 348 | 0.70 [0.50–0.71] | 0.99 [0.41–0.99] | 0.55 [0.54–0.64] | 8 | 0.70 [0.50–0.71] | 0.90 [0.33–0.91] |
| 20 | 8 | 492 | 0.69 [0.69–0.70] | 1.00 [1.00–1.00] | 0.53 [0.53–0.54] | 4 | 0.83 [0.83–0.84] | 0.99 [0.99–0.99] |
| 3 | 16 | 16 | 0.06 [0.00–0.25] | 0.12 [0.00–0.19] | 0.04 [0.00–0.38] | 8 | 0.06 [0.00–0.25] | 0.03 [0.00–0.04] |
| 5 | 16 | 362 | 0.35 [0.28–0.36] | 0.31 [0.20–0.36] | 0.35 [0.27–0.47] | 8 | 0.35 [0.28–0.36] | 0.19 [0.08–0.20] |
| 10 | 16 | 1434 | 0.72 [0.34–0.72] | 1.00 [0.23–1.00] | 0.56 [0.56–0.64] | 4 | 0.84 [0.12–0.84] | 0.94 [0.20–0.94] |
| 20 | 16 | 2040 | 0.77 [0.77–0.77] | 1.00 [1.00–1.00] | 0.62 [0.62–0.63] | 4 | 0.86 [0.86–0.87] | 0.97 [0.97–0.97] |
| 3 | 32 | 54 | 0.00 [0.00–0.13] | 0.00 [0.00–0.09] | 0.00 [0.00–0.23] | 16 | 0.05 [0.01–0.08] | 0.01 [0.00–0.03] |
| 5 | 32 | 1474 | 0.45 [0.15–0.52] | 0.42 [0.09–0.48] | 0.50 [0.44–0.66] | 8 | 0.45 [0.15–0.52] | 0.17 [0.03–0.19] |
| 10 | 32 | 5716 | 0.74 [0.74–0.78] | 1.00 [0.99–1.00] | 0.59 [0.59–0.65] | 4 | 0.85 [0.84–0.87] | 0.94 [0.90–0.94] |
| 20 | 32 | 8122 | 0.80 [0.80–0.81] | 1.00 [1.00–1.00] | 0.67 [0.67–0.68] | 4 | 0.88 [0.88–0.88] | 0.97 [0.97–0.97] |

## A neighbour of the same brightness

Two discs of radius 16 px at peak S/N 10, centres separated along the apparent minor axis; the click is the left disc's brightest voxel, reach 8. 'Leak' is the fraction of the selection inside the neighbour's 2.5σ truth. Below 2 radii the two discs overlap on the sky.

| Δv_sys (km/s) | λv | separation (radii) | Dice | completeness | reliability | leak |
|---|---|---|---|---|---|---|
| 0 | 1 | 1.25 | 0.71 | 1.00 | 0.56 | 0.00 |
| 0 | 1 | 1.5 | 0.72 | 1.00 | 0.56 | 0.00 |
| 0 | 1 | 2 | 0.72 | 1.00 | 0.57 | 0.00 |
| 0 | 1 | 3 | 0.72 | 1.00 | 0.56 | 0.00 |
| 0 | 3 | 1.25 | 0.75 | 1.00 | 0.61 | 0.00 |
| 0 | 3 | 1.5 | 0.78 | 1.00 | 0.64 | 0.00 |
| 0 | 3 | 2 | 0.78 | 0.99 | 0.64 | 0.00 |
| 0 | 3 | 3 | 0.75 | 1.00 | 0.60 | 0.00 |
| 150 | 1 | 1.25 | 0.46 | 1.00 | 0.30 | 0.29 |
| 150 | 1 | 1.5 | 0.47 | 1.00 | 0.31 | 0.29 |
| 150 | 1 | 2 | 0.73 | 1.00 | 0.58 | 0.00 |
| 150 | 1 | 3 | 0.72 | 1.00 | 0.56 | 0.00 |
| 150 | 3 | 1.25 | 0.50 | 1.00 | 0.33 | 0.32 |
| 150 | 3 | 1.5 | 0.52 | 1.00 | 0.35 | 0.32 |
| 150 | 3 | 2 | 0.78 | 0.99 | 0.64 | 0.00 |
| 150 | 3 | 3 | 0.76 | 0.99 | 0.62 | 0.00 |

