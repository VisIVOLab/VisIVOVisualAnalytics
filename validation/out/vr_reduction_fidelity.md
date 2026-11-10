# Fidelity of the headset's reduced cube (issue #167)

Share of each reference mask still drawn above the display threshold, by
reduction (spatial x spatial x spectral factor) and rule. 'cube' = the full
cube's default threshold (as on the desktop); 'binning' = the page's default:
the noise rule on the reduced cube for mean binning, the full cube's threshold
for max binning. Fog = reduced voxels drawn that
hold no mask voxel. Spill = full-resolution voxels outside the mask covered
by a drawn reduced voxel, as a share of the mask's size: the blur of the
reduction ('voxels kept' rises with the factor partly because of it).
Encoding as sent to the headset.

## uint8

| Cube | Reference | Factors | Rule | Threshold from | Threshold | Voxels kept | Flux kept | Fog voxels (of drawn) | Spill |
|---|---|---|---|---|---|---|---|---|---|
| HCG 44 | Serra+2013 | 1x1x1 | mean | cube | 0.00108 | 13.1 % | 56.7 % | 62 (0 %) | 0 % |
| HCG 44 | Serra+2013 | 1x1x1 | mean | binning | 0.00108 | 13.1 % | 56.7 % | 62 (0 %) | 0 % |
| HCG 44 | Serra+2013 | 1x1x1 | max | cube | 0.00108 | 13.1 % | 56.7 % | 62 (0 %) | 0 % |
| HCG 44 | Serra+2013 | 1x1x1 | max | binning | 0.00108 | 13.1 % | 56.7 % | 62 (0 %) | 0 % |
| HCG 44 | Serra+2013 | 2x2x1 | mean | cube | 0.00108 | 13.3 % | 56.3 % | 10 (0 %) | 0 % |
| HCG 44 | Serra+2013 | 2x2x1 | mean | binning | 0.000946 | 15.4 % | 60.0 % | 127 (3 %) | 1 % |
| HCG 44 | Serra+2013 | 2x2x1 | max | cube | 0.00108 | 17.0 % | 62.4 % | 45 (1 %) | 0 % |
| HCG 44 | Serra+2013 | 2x2x1 | max | binning | 0.00108 | 17.0 % | 62.4 % | 45 (1 %) | 0 % |
| HCG 44 | Serra+2013 | 3x3x1 | mean | cube | 0.00108 | 13.0 % | 54.7 % | 0 (0 %) | 0 % |
| HCG 44 | Serra+2013 | 3x3x1 | mean | binning | 0.000847 | 17.0 % | 61.5 % | 97 (5 %) | 1 % |
| HCG 44 | Serra+2013 | 3x3x1 | max | cube | 0.00108 | 21.8 % | 67.1 % | 66 (3 %) | 1 % |
| HCG 44 | Serra+2013 | 3x3x1 | max | binning | 0.00108 | 21.8 % | 67.1 % | 66 (3 %) | 1 % |
| HCG 44 | Serra+2013 | 4x4x1 | mean | cube | 0.00108 | 13.2 % | 53.5 % | 0 (0 %) | 0 % |
| HCG 44 | Serra+2013 | 4x4x1 | mean | binning | 0.000757 | 19.5 % | 63.6 % | 63 (5 %) | 1 % |
| HCG 44 | Serra+2013 | 4x4x1 | max | cube | 0.00108 | 25.5 % | 69.3 % | 32 (2 %) | 1 % |
| HCG 44 | Serra+2013 | 4x4x1 | max | binning | 0.00108 | 25.5 % | 69.3 % | 32 (2 %) | 1 % |
| HCG 44 | Serra+2013 | 6x6x1 | mean | cube | 0.00108 | 12.1 % | 48.1 % | 0 (0 %) | 0 % |
| HCG 44 | Serra+2013 | 6x6x1 | mean | binning | 0.000584 | 25.0 % | 66.7 % | 84 (10 %) | 4 % |
| HCG 44 | Serra+2013 | 6x6x1 | max | cube | 0.00108 | 34.0 % | 73.6 % | 30 (3 %) | 4 % |
| HCG 44 | Serra+2013 | 6x6x1 | max | binning | 0.00108 | 34.0 % | 73.6 % | 30 (3 %) | 4 % |
| HCG 44 | Serra+2013 | 8x8x1 | mean | cube | 0.00108 | 11.3 % | 43.2 % | 0 (0 %) | 0 % |
| HCG 44 | Serra+2013 | 8x8x1 | mean | binning | 0.000471 | 30.4 % | 68.8 % | 107 (17 %) | 9 % |
| HCG 44 | Serra+2013 | 8x8x1 | max | cube | 0.00108 | 40.0 % | 76.2 % | 48 (6 %) | 14 % |
| HCG 44 | Serra+2013 | 8x8x1 | max | binning | 0.00108 | 40.0 % | 76.2 % | 48 (6 %) | 14 % |
| HCG 44 | Serra+2013 | 2x2x2 | mean | cube | 0.00108 | 13.0 % | 55.3 % | 0 (0 %) | 0 % |
| HCG 44 | Serra+2013 | 2x2x2 | mean | binning | 0.000858 | 16.8 % | 61.8 % | 103 (5 %) | 1 % |
| HCG 44 | Serra+2013 | 2x2x2 | max | cube | 0.00108 | 19.3 % | 65.0 % | 33 (1 %) | 0 % |
| HCG 44 | Serra+2013 | 2x2x2 | max | binning | 0.00108 | 19.3 % | 65.0 % | 33 (1 %) | 0 % |
| HCG 44 | Serra+2013 | 3x3x2 | mean | cube | 0.00108 | 13.2 % | 54.7 % | 0 (0 %) | 0 % |
| HCG 44 | Serra+2013 | 3x3x2 | mean | binning | 0.00076 | 19.5 % | 64.6 % | 159 (13 %) | 3 % |
| HCG 44 | Serra+2013 | 3x3x2 | max | cube | 0.00108 | 24.8 % | 70.0 % | 63 (4 %) | 1 % |
| HCG 44 | Serra+2013 | 3x3x2 | max | binning | 0.00108 | 24.8 % | 70.0 % | 63 (4 %) | 1 % |
| HCG 44 | Serra+2013 | 4x4x2 | mean | cube | 0.00108 | 13.0 % | 52.7 % | 0 (0 %) | 0 % |
| HCG 44 | Serra+2013 | 4x4x2 | mean | binning | 0.000675 | 21.8 % | 66.0 % | 79 (10 %) | 3 % |
| HCG 44 | Serra+2013 | 4x4x2 | max | cube | 0.00108 | 28.9 % | 72.2 % | 47 (5 %) | 2 % |
| HCG 44 | Serra+2013 | 4x4x2 | max | binning | 0.00108 | 28.9 % | 72.2 % | 47 (5 %) | 2 % |
| HCG 44 | SoFiA-2 | 1x1x1 | mean | cube | 0.00108 | 31.6 % | 68.7 % | 67 (1 %) | 0 % |
| HCG 44 | SoFiA-2 | 1x1x1 | mean | binning | 0.00108 | 31.6 % | 68.7 % | 67 (1 %) | 0 % |
| HCG 44 | SoFiA-2 | 1x1x1 | max | cube | 0.00108 | 31.6 % | 68.7 % | 67 (1 %) | 0 % |
| HCG 44 | SoFiA-2 | 1x1x1 | max | binning | 0.00108 | 31.6 % | 68.7 % | 67 (1 %) | 0 % |
| HCG 44 | SoFiA-2 | 2x2x1 | mean | cube | 0.00108 | 32.0 % | 68.2 % | 13 (0 %) | 0 % |
| HCG 44 | SoFiA-2 | 2x2x1 | mean | binning | 0.000946 | 36.9 % | 72.6 % | 138 (3 %) | 2 % |
| HCG 44 | SoFiA-2 | 2x2x1 | max | cube | 0.00108 | 40.4 % | 75.2 % | 47 (1 %) | 1 % |
| HCG 44 | SoFiA-2 | 2x2x1 | max | binning | 0.00108 | 40.4 % | 75.2 % | 47 (1 %) | 1 % |
| HCG 44 | SoFiA-2 | 3x3x1 | mean | cube | 0.00108 | 31.0 % | 66.1 % | 2 (0 %) | 0 % |
| HCG 44 | SoFiA-2 | 3x3x1 | mean | binning | 0.000847 | 40.0 % | 74.0 % | 106 (5 %) | 3 % |
| HCG 44 | SoFiA-2 | 3x3x1 | max | cube | 0.00108 | 48.7 % | 79.9 % | 71 (3 %) | 6 % |
| HCG 44 | SoFiA-2 | 3x3x1 | max | binning | 0.00108 | 48.7 % | 79.9 % | 71 (3 %) | 6 % |
| HCG 44 | SoFiA-2 | 4x4x1 | mean | cube | 0.00108 | 31.0 % | 64.5 % | 0 (0 %) | 1 % |
| HCG 44 | SoFiA-2 | 4x4x1 | mean | binning | 0.000757 | 43.9 % | 75.7 % | 72 (5 %) | 6 % |
| HCG 44 | SoFiA-2 | 4x4x1 | max | cube | 0.00108 | 53.0 % | 81.7 % | 35 (2 %) | 11 % |
| HCG 44 | SoFiA-2 | 4x4x1 | max | binning | 0.00108 | 53.0 % | 81.7 % | 35 (2 %) | 11 % |
| HCG 44 | SoFiA-2 | 6x6x1 | mean | cube | 0.00108 | 27.5 % | 57.9 % | 0 (0 %) | 2 % |
| HCG 44 | SoFiA-2 | 6x6x1 | mean | binning | 0.000584 | 50.7 % | 78.1 % | 96 (12 %) | 19 % |
| HCG 44 | SoFiA-2 | 6x6x1 | max | cube | 0.00108 | 61.7 % | 85.3 % | 31 (3 %) | 31 % |
| HCG 44 | SoFiA-2 | 6x6x1 | max | binning | 0.00108 | 61.7 % | 85.3 % | 31 (3 %) | 31 % |
| HCG 44 | SoFiA-2 | 8x8x1 | mean | cube | 0.00108 | 24.4 % | 51.8 % | 0 (0 %) | 3 % |
| HCG 44 | SoFiA-2 | 8x8x1 | mean | binning | 0.000471 | 55.5 % | 79.2 % | 124 (19 %) | 41 % |
| HCG 44 | SoFiA-2 | 8x8x1 | max | cube | 0.00108 | 65.9 % | 87.0 % | 51 (6 %) | 64 % |
| HCG 44 | SoFiA-2 | 8x8x1 | max | binning | 0.00108 | 65.9 % | 87.0 % | 51 (6 %) | 64 % |
| HCG 44 | SoFiA-2 | 2x2x2 | mean | cube | 0.00108 | 31.1 % | 67.0 % | 0 (0 %) | 0 % |
| HCG 44 | SoFiA-2 | 2x2x2 | mean | binning | 0.000858 | 39.7 % | 74.4 % | 114 (5 %) | 3 % |
| HCG 44 | SoFiA-2 | 2x2x2 | max | cube | 0.00108 | 44.7 % | 77.9 % | 36 (1 %) | 3 % |
| HCG 44 | SoFiA-2 | 2x2x2 | max | binning | 0.00108 | 44.7 % | 77.9 % | 36 (1 %) | 3 % |
| HCG 44 | SoFiA-2 | 3x3x2 | mean | cube | 0.00108 | 31.4 % | 66.1 % | 0 (0 %) | 1 % |
| HCG 44 | SoFiA-2 | 3x3x2 | mean | binning | 0.00076 | 45.1 % | 77.3 % | 167 (13 %) | 9 % |
| HCG 44 | SoFiA-2 | 3x3x2 | max | cube | 0.00108 | 53.6 % | 82.6 % | 68 (5 %) | 10 % |
| HCG 44 | SoFiA-2 | 3x3x2 | max | binning | 0.00108 | 53.6 % | 82.6 % | 68 (5 %) | 10 % |
| HCG 44 | SoFiA-2 | 4x4x2 | mean | cube | 0.00108 | 30.5 % | 63.6 % | 0 (0 %) | 1 % |
| HCG 44 | SoFiA-2 | 4x4x2 | mean | binning | 0.000675 | 48.0 % | 78.2 % | 85 (11 %) | 11 % |
| HCG 44 | SoFiA-2 | 4x4x2 | max | cube | 0.00108 | 57.9 % | 84.3 % | 51 (5 %) | 18 % |
| HCG 44 | SoFiA-2 | 4x4x2 | max | binning | 0.00108 | 57.9 % | 84.3 % | 51 (5 %) | 18 % |
| WALLABY NGC 3137 | SoFiA-2 | 1x1x1 | mean | cube | 0.00835 | 23.1 % | 69.7 % | 109 (0 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 1x1x1 | mean | binning | 0.00835 | 23.1 % | 69.7 % | 109 (0 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 1x1x1 | max | cube | 0.00835 | 23.1 % | 69.7 % | 109 (0 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 1x1x1 | max | binning | 0.00835 | 23.1 % | 69.7 % | 109 (0 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 2x2x1 | mean | cube | 0.00835 | 23.0 % | 69.2 % | 8 (0 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 2x2x1 | mean | binning | 0.00737 | 25.3 % | 72.0 % | 80 (1 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 2x2x1 | max | cube | 0.00835 | 26.3 % | 73.0 % | 45 (0 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 2x2x1 | max | binning | 0.00835 | 26.3 % | 73.0 % | 45 (0 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 3x3x1 | mean | cube | 0.00835 | 23.3 % | 69.0 % | 1 (0 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 3x3x1 | mean | binning | 0.00672 | 27.3 % | 73.6 % | 59 (1 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 3x3x1 | max | cube | 0.00835 | 30.0 % | 75.9 % | 40 (1 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 3x3x1 | max | binning | 0.00835 | 30.0 % | 75.9 % | 40 (1 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 4x4x1 | mean | cube | 0.00835 | 22.6 % | 67.3 % | 0 (0 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 4x4x1 | mean | binning | 0.00614 | 30.0 % | 75.6 % | 34 (1 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 4x4x1 | max | cube | 0.00835 | 35.0 % | 79.0 % | 65 (2 %) | 1 % |
| WALLABY NGC 3137 | SoFiA-2 | 4x4x1 | max | binning | 0.00835 | 35.0 % | 79.0 % | 65 (2 %) | 1 % |
| WALLABY NGC 3137 | SoFiA-2 | 6x6x1 | mean | cube | 0.00835 | 23.6 % | 66.4 % | 0 (0 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 6x6x1 | mean | binning | 0.0051 | 33.7 % | 76.9 % | 8 (1 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 6x6x1 | max | cube | 0.00835 | 42.6 % | 81.7 % | 58 (3 %) | 3 % |
| WALLABY NGC 3137 | SoFiA-2 | 6x6x1 | max | binning | 0.00835 | 42.6 % | 81.7 % | 58 (3 %) | 3 % |
| WALLABY NGC 3137 | SoFiA-2 | 8x8x1 | mean | cube | 0.00835 | 23.5 % | 64.0 % | 0 (0 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 8x8x1 | mean | binning | 0.00412 | 40.0 % | 80.0 % | 16 (2 %) | 1 % |
| WALLABY NGC 3137 | SoFiA-2 | 8x8x1 | max | cube | 0.00835 | 47.7 % | 83.5 % | 32 (2 %) | 4 % |
| WALLABY NGC 3137 | SoFiA-2 | 8x8x1 | max | binning | 0.00835 | 47.7 % | 83.5 % | 32 (2 %) | 4 % |
| WALLABY NGC 3137 | SoFiA-2 | 2x2x2 | mean | cube | 0.00835 | 22.3 % | 67.6 % | 0 (0 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 2x2x2 | mean | binning | 0.00558 | 30.5 % | 76.5 % | 63 (1 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 2x2x2 | max | cube | 0.00835 | 30.6 % | 76.2 % | 64 (1 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 2x2x2 | max | binning | 0.00835 | 30.6 % | 76.2 % | 64 (1 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 3x3x2 | mean | cube | 0.00835 | 22.8 % | 67.7 % | 0 (0 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 3x3x2 | mean | binning | 0.00528 | 31.8 % | 77.1 % | 25 (1 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 3x3x2 | max | cube | 0.00835 | 34.7 % | 78.8 % | 64 (2 %) | 1 % |
| WALLABY NGC 3137 | SoFiA-2 | 3x3x2 | max | binning | 0.00835 | 34.7 % | 78.8 % | 64 (2 %) | 1 % |
| WALLABY NGC 3137 | SoFiA-2 | 4x4x2 | mean | cube | 0.00835 | 22.7 % | 66.8 % | 0 (0 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 4x4x2 | mean | binning | 0.00469 | 34.4 % | 78.6 % | 13 (1 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 4x4x2 | max | cube | 0.00835 | 38.7 % | 80.8 % | 41 (2 %) | 1 % |
| WALLABY NGC 3137 | SoFiA-2 | 4x4x2 | max | binning | 0.00835 | 38.7 % | 80.8 % | 41 (2 %) | 1 % |

## float16

| Cube | Reference | Factors | Rule | Threshold from | Threshold | Voxels kept | Flux kept | Fog voxels (of drawn) | Spill |
|---|---|---|---|---|---|---|---|---|---|
| HCG 44 | Serra+2013 | 1x1x1 | mean | cube | 0.00108 | 13.3 % | 57.1 % | 85 (1 %) | 0 % |
| HCG 44 | Serra+2013 | 1x1x1 | mean | binning | 0.00108 | 13.3 % | 57.1 % | 85 (1 %) | 0 % |
| HCG 44 | Serra+2013 | 1x1x1 | max | cube | 0.00108 | 13.3 % | 57.1 % | 85 (1 %) | 0 % |
| HCG 44 | Serra+2013 | 1x1x1 | max | binning | 0.00108 | 13.3 % | 57.1 % | 85 (1 %) | 0 % |
| HCG 44 | Serra+2013 | 2x2x1 | mean | cube | 0.00108 | 13.2 % | 56.1 % | 9 (0 %) | 0 % |
| HCG 44 | Serra+2013 | 2x2x1 | mean | binning | 0.000946 | 15.4 % | 60.0 % | 127 (3 %) | 1 % |
| HCG 44 | Serra+2013 | 2x2x1 | max | cube | 0.00108 | 17.2 % | 62.6 % | 56 (1 %) | 0 % |
| HCG 44 | Serra+2013 | 2x2x1 | max | binning | 0.00108 | 17.2 % | 62.6 % | 56 (1 %) | 0 % |
| HCG 44 | Serra+2013 | 3x3x1 | mean | cube | 0.00108 | 13.1 % | 54.8 % | 0 (0 %) | 0 % |
| HCG 44 | Serra+2013 | 3x3x1 | mean | binning | 0.000847 | 17.3 % | 61.9 % | 121 (6 %) | 1 % |
| HCG 44 | Serra+2013 | 3x3x1 | max | cube | 0.00108 | 21.5 % | 66.7 % | 43 (2 %) | 1 % |
| HCG 44 | Serra+2013 | 3x3x1 | max | binning | 0.00108 | 21.5 % | 66.7 % | 43 (2 %) | 1 % |
| HCG 44 | Serra+2013 | 4x4x1 | mean | cube | 0.00108 | 13.1 % | 53.4 % | 0 (0 %) | 0 % |
| HCG 44 | Serra+2013 | 4x4x1 | mean | binning | 0.000757 | 19.8 % | 64.0 % | 92 (7 %) | 2 % |
| HCG 44 | Serra+2013 | 4x4x1 | max | cube | 0.00108 | 25.7 % | 69.5 % | 42 (2 %) | 1 % |
| HCG 44 | Serra+2013 | 4x4x1 | max | binning | 0.00108 | 25.7 % | 69.5 % | 42 (2 %) | 1 % |
| HCG 44 | Serra+2013 | 6x6x1 | mean | cube | 0.00108 | 11.9 % | 47.9 % | 0 (0 %) | 0 % |
| HCG 44 | Serra+2013 | 6x6x1 | mean | binning | 0.000584 | 25.3 % | 67.0 % | 116 (14 %) | 5 % |
| HCG 44 | Serra+2013 | 6x6x1 | max | cube | 0.00108 | 34.2 % | 73.8 % | 40 (4 %) | 5 % |
| HCG 44 | Serra+2013 | 6x6x1 | max | binning | 0.00108 | 34.2 % | 73.8 % | 40 (4 %) | 5 % |
| HCG 44 | Serra+2013 | 8x8x1 | mean | cube | 0.00108 | 11.3 % | 43.0 % | 0 (0 %) | 0 % |
| HCG 44 | Serra+2013 | 8x8x1 | mean | binning | 0.000471 | 30.7 % | 69.1 % | 117 (18 %) | 10 % |
| HCG 44 | Serra+2013 | 8x8x1 | max | cube | 0.00108 | 39.8 % | 76.0 % | 38 (5 %) | 13 % |
| HCG 44 | Serra+2013 | 8x8x1 | max | binning | 0.00108 | 39.8 % | 76.0 % | 38 (5 %) | 13 % |
| HCG 44 | Serra+2013 | 2x2x2 | mean | cube | 0.00108 | 13.1 % | 55.6 % | 0 (0 %) | 0 % |
| HCG 44 | Serra+2013 | 2x2x2 | mean | binning | 0.000858 | 17.0 % | 62.1 % | 108 (5 %) | 1 % |
| HCG 44 | Serra+2013 | 2x2x2 | max | cube | 0.00108 | 19.7 % | 65.5 % | 49 (2 %) | 0 % |
| HCG 44 | Serra+2013 | 2x2x2 | max | binning | 0.00108 | 19.7 % | 65.5 % | 49 (2 %) | 0 % |
| HCG 44 | Serra+2013 | 3x3x2 | mean | cube | 0.00108 | 13.1 % | 54.5 % | 0 (0 %) | 0 % |
| HCG 44 | Serra+2013 | 3x3x2 | mean | binning | 0.00076 | 19.3 % | 64.2 % | 120 (10 %) | 2 % |
| HCG 44 | Serra+2013 | 3x3x2 | max | cube | 0.00108 | 24.5 % | 69.6 % | 43 (3 %) | 1 % |
| HCG 44 | Serra+2013 | 3x3x2 | max | binning | 0.00108 | 24.5 % | 69.6 % | 43 (3 %) | 1 % |
| HCG 44 | Serra+2013 | 4x4x2 | mean | cube | 0.00108 | 12.9 % | 52.6 % | 0 (0 %) | 0 % |
| HCG 44 | Serra+2013 | 4x4x2 | mean | binning | 0.000675 | 22.2 % | 66.6 % | 99 (12 %) | 3 % |
| HCG 44 | Serra+2013 | 4x4x2 | max | cube | 0.00108 | 28.6 % | 72.0 % | 39 (4 %) | 2 % |
| HCG 44 | Serra+2013 | 4x4x2 | max | binning | 0.00108 | 28.6 % | 72.0 % | 39 (4 %) | 2 % |
| HCG 44 | SoFiA-2 | 1x1x1 | mean | cube | 0.00108 | 32.1 % | 69.2 % | 91 (1 %) | 0 % |
| HCG 44 | SoFiA-2 | 1x1x1 | mean | binning | 0.00108 | 32.1 % | 69.2 % | 91 (1 %) | 0 % |
| HCG 44 | SoFiA-2 | 1x1x1 | max | cube | 0.00108 | 32.1 % | 69.2 % | 91 (1 %) | 0 % |
| HCG 44 | SoFiA-2 | 1x1x1 | max | binning | 0.00108 | 32.1 % | 69.2 % | 91 (1 %) | 0 % |
| HCG 44 | SoFiA-2 | 2x2x1 | mean | cube | 0.00108 | 31.7 % | 67.9 % | 11 (0 %) | 0 % |
| HCG 44 | SoFiA-2 | 2x2x1 | mean | binning | 0.000946 | 36.8 % | 72.6 % | 138 (3 %) | 2 % |
| HCG 44 | SoFiA-2 | 2x2x1 | max | cube | 0.00108 | 40.8 % | 75.5 % | 57 (1 %) | 1 % |
| HCG 44 | SoFiA-2 | 2x2x1 | max | binning | 0.00108 | 40.8 % | 75.5 % | 57 (1 %) | 1 % |
| HCG 44 | SoFiA-2 | 3x3x1 | mean | cube | 0.00108 | 31.2 % | 66.3 % | 2 (0 %) | 0 % |
| HCG 44 | SoFiA-2 | 3x3x1 | mean | binning | 0.000847 | 40.5 % | 74.4 % | 131 (6 %) | 4 % |
| HCG 44 | SoFiA-2 | 3x3x1 | max | cube | 0.00108 | 48.2 % | 79.6 % | 45 (2 %) | 5 % |
| HCG 44 | SoFiA-2 | 3x3x1 | max | binning | 0.00108 | 48.2 % | 79.6 % | 45 (2 %) | 5 % |
| HCG 44 | SoFiA-2 | 4x4x1 | mean | cube | 0.00108 | 30.9 % | 64.4 % | 0 (0 %) | 1 % |
| HCG 44 | SoFiA-2 | 4x4x1 | mean | binning | 0.000757 | 44.4 % | 76.1 % | 102 (8 %) | 7 % |
| HCG 44 | SoFiA-2 | 4x4x1 | max | cube | 0.00108 | 53.3 % | 81.9 % | 45 (3 %) | 11 % |
| HCG 44 | SoFiA-2 | 4x4x1 | max | binning | 0.00108 | 53.3 % | 81.9 % | 45 (3 %) | 11 % |
| HCG 44 | SoFiA-2 | 6x6x1 | mean | cube | 0.00108 | 27.3 % | 57.7 % | 0 (0 %) | 2 % |
| HCG 44 | SoFiA-2 | 6x6x1 | mean | binning | 0.000584 | 51.3 % | 78.5 % | 130 (15 %) | 22 % |
| HCG 44 | SoFiA-2 | 6x6x1 | max | cube | 0.00108 | 61.9 % | 85.4 % | 42 (4 %) | 32 % |
| HCG 44 | SoFiA-2 | 6x6x1 | max | binning | 0.00108 | 61.9 % | 85.4 % | 42 (4 %) | 32 % |
| HCG 44 | SoFiA-2 | 8x8x1 | mean | cube | 0.00108 | 24.3 % | 51.7 % | 0 (0 %) | 3 % |
| HCG 44 | SoFiA-2 | 8x8x1 | mean | binning | 0.000471 | 55.9 % | 79.5 % | 134 (20 %) | 43 % |
| HCG 44 | SoFiA-2 | 8x8x1 | max | cube | 0.00108 | 65.6 % | 86.9 % | 40 (5 %) | 62 % |
| HCG 44 | SoFiA-2 | 8x8x1 | max | binning | 0.00108 | 65.6 % | 86.9 % | 40 (5 %) | 62 % |
| HCG 44 | SoFiA-2 | 2x2x2 | mean | cube | 0.00108 | 31.4 % | 67.3 % | 0 (0 %) | 0 % |
| HCG 44 | SoFiA-2 | 2x2x2 | mean | binning | 0.000858 | 40.2 % | 74.8 % | 119 (5 %) | 3 % |
| HCG 44 | SoFiA-2 | 2x2x2 | max | cube | 0.00108 | 45.5 % | 78.4 % | 53 (2 %) | 3 % |
| HCG 44 | SoFiA-2 | 2x2x2 | max | binning | 0.00108 | 45.5 % | 78.4 % | 53 (2 %) | 3 % |
| HCG 44 | SoFiA-2 | 3x3x2 | mean | cube | 0.00108 | 31.1 % | 65.8 % | 0 (0 %) | 1 % |
| HCG 44 | SoFiA-2 | 3x3x2 | mean | binning | 0.00076 | 44.5 % | 76.9 % | 127 (10 %) | 8 % |
| HCG 44 | SoFiA-2 | 3x3x2 | max | cube | 0.00108 | 53.2 % | 82.4 % | 46 (3 %) | 9 % |
| HCG 44 | SoFiA-2 | 3x3x2 | max | binning | 0.00108 | 53.2 % | 82.4 % | 46 (3 %) | 9 % |
| HCG 44 | SoFiA-2 | 4x4x2 | mean | cube | 0.00108 | 30.4 % | 63.5 % | 0 (0 %) | 1 % |
| HCG 44 | SoFiA-2 | 4x4x2 | mean | binning | 0.000675 | 48.7 % | 78.8 % | 105 (13 %) | 13 % |
| HCG 44 | SoFiA-2 | 4x4x2 | max | cube | 0.00108 | 57.6 % | 84.2 % | 41 (4 %) | 17 % |
| HCG 44 | SoFiA-2 | 4x4x2 | max | binning | 0.00108 | 57.6 % | 84.2 % | 41 (4 %) | 17 % |
| WALLABY NGC 3137 | SoFiA-2 | 1x1x1 | mean | cube | 0.00835 | 22.8 % | 69.3 % | 81 (0 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 1x1x1 | mean | binning | 0.00835 | 22.8 % | 69.3 % | 81 (0 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 1x1x1 | max | cube | 0.00835 | 22.8 % | 69.3 % | 81 (0 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 1x1x1 | max | binning | 0.00835 | 22.8 % | 69.3 % | 81 (0 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 2x2x1 | mean | cube | 0.00835 | 22.8 % | 68.9 % | 7 (0 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 2x2x1 | mean | binning | 0.00737 | 25.3 % | 72.1 % | 81 (1 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 2x2x1 | max | cube | 0.00835 | 26.7 % | 73.4 % | 58 (1 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 2x2x1 | max | binning | 0.00835 | 26.7 % | 73.4 % | 58 (1 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 3x3x1 | mean | cube | 0.00835 | 22.9 % | 68.5 % | 1 (0 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 3x3x1 | mean | binning | 0.00672 | 27.3 % | 73.7 % | 61 (1 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 3x3x1 | max | cube | 0.00835 | 30.5 % | 76.3 % | 53 (1 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 3x3x1 | max | binning | 0.00835 | 30.5 % | 76.3 % | 53 (1 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 4x4x1 | mean | cube | 0.00835 | 23.0 % | 67.7 % | 0 (0 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 4x4x1 | mean | binning | 0.00614 | 29.5 % | 75.1 % | 24 (1 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 4x4x1 | max | cube | 0.00835 | 34.5 % | 78.6 % | 48 (1 %) | 1 % |
| WALLABY NGC 3137 | SoFiA-2 | 4x4x1 | max | binning | 0.00835 | 34.5 % | 78.6 % | 48 (1 %) | 1 % |
| WALLABY NGC 3137 | SoFiA-2 | 6x6x1 | mean | cube | 0.00835 | 23.4 % | 66.0 % | 0 (0 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 6x6x1 | mean | binning | 0.0051 | 33.8 % | 76.9 % | 8 (1 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 6x6x1 | max | cube | 0.00835 | 42.4 % | 81.6 % | 48 (2 %) | 2 % |
| WALLABY NGC 3137 | SoFiA-2 | 6x6x1 | max | binning | 0.00835 | 42.4 % | 81.6 % | 48 (2 %) | 2 % |
| WALLABY NGC 3137 | SoFiA-2 | 8x8x1 | mean | cube | 0.00835 | 23.9 % | 64.5 % | 0 (0 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 8x8x1 | mean | binning | 0.00412 | 39.6 % | 79.7 % | 11 (1 %) | 1 % |
| WALLABY NGC 3137 | SoFiA-2 | 8x8x1 | max | cube | 0.00835 | 48.0 % | 83.6 % | 42 (3 %) | 5 % |
| WALLABY NGC 3137 | SoFiA-2 | 8x8x1 | max | binning | 0.00835 | 48.0 % | 83.6 % | 42 (3 %) | 5 % |
| WALLABY NGC 3137 | SoFiA-2 | 2x2x2 | mean | cube | 0.00835 | 22.4 % | 67.8 % | 0 (0 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 2x2x2 | mean | binning | 0.00558 | 30.5 % | 76.5 % | 63 (1 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 2x2x2 | max | cube | 0.00835 | 30.4 % | 76.1 % | 58 (1 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 2x2x2 | max | binning | 0.00835 | 30.4 % | 76.1 % | 58 (1 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 3x3x2 | mean | cube | 0.00835 | 22.6 % | 67.5 % | 0 (0 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 3x3x2 | mean | binning | 0.00528 | 31.7 % | 77.0 % | 25 (1 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 3x3x2 | max | cube | 0.00835 | 34.6 % | 78.7 % | 52 (2 %) | 1 % |
| WALLABY NGC 3137 | SoFiA-2 | 3x3x2 | max | binning | 0.00835 | 34.6 % | 78.7 % | 52 (2 %) | 1 % |
| WALLABY NGC 3137 | SoFiA-2 | 4x4x2 | mean | cube | 0.00835 | 22.9 % | 67.2 % | 0 (0 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 4x4x2 | mean | binning | 0.00469 | 34.6 % | 78.7 % | 13 (1 %) | 0 % |
| WALLABY NGC 3137 | SoFiA-2 | 4x4x2 | max | cube | 0.00835 | 38.9 % | 80.9 % | 49 (2 %) | 1 % |
| WALLABY NGC 3137 | SoFiA-2 | 4x4x2 | max | binning | 0.00835 | 38.9 % | 80.9 % | 49 (2 %) | 1 % |
