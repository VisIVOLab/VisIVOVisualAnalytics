# VR analysis = desktop analysis — HCG 44, NGC 3187 (issue #191)

Everything runs the app's code: the routes in-process, the page's modules under node (`tools-lasso.js`, `tools-moment.js`, `tools-pv.js`, `selection.js`). Lasso defaults reach 8, λv 1, nσ 2.5, σ measured by the service; the desktop's displayed selection is the Reach slider's level field (reach_max 64) thresholded at the reach.

## 1. Lasso seeds (new, add, carve)

| press | seed (x, y, ch) | voxels | request = desktop recipe | Dice / desktop shown | Dice / desktop recipe | flux (Jy km/s) |
|---|---|---|---|---|---|---|
| new | (465, 299, 74) | 13216 | yes | 1.0000 | 1.0000 | 6.061 |
| add | (442, 284, 74) | 23891 | yes | 1.0000 | 1.0000 | 9.191 |
| carve | (471, 300, 81) | 10675 | yes | 1.0000 | 1.0000 | 3.131 |

## 2. Isolate and Remove

| selection | mode | cube = desktop's | finite voxels | expected | kept = selection |
|---|---|---|---|---|---|
| new | blank_outside | yes | 13216 | 13216 | yes |
| new | blank_inside | yes | 77406784 | 77406784 | yes |
| carve | blank_outside | yes | 10675 | 10675 | yes |
| carve | blank_inside | yes | 77409325 | 77409325 | yes |

## 3. Moments of the selection

*request = desktop*: the page's request has exactly the keys `BackendClient::requestMoment` sends (read from its C++ source) and the desktop dialog's default channel range; the threshold mask is OFF (the dialog's default is an automatic ~3σ mask: on an isolated selection the selection is the mask). *map = desktop's*: the page's decoded map equals the backend's answer to that request on the desktop's own isolated cube, bit for bit (it checks the page's decoding and that both isolated cubes are the same).

| selection | order | request = desktop | mask | map = desktop's | unit (backend) | shown as | colour range (backend unit) | pixels |
|---|---|---|---|---|---|---|---|---|
| new | M0 | yes | off | yes | JY/BEAM m/s | JY/BEAM km/s | 1.841 to 1064 | 995 |
| new | M1 | yes | off | yes | m/s | km/s | 1.191e+06 to 1.99e+06 | 995 |
| new | M2 | yes | off | yes | m/s | km/s | 0 to 7.367e+04 | 995 |
| carve | M0 | yes | off | yes | JY/BEAM m/s | JY/BEAM km/s | 1.594 to 702.9 | 613 |
| carve | M1 | yes | off | yes | m/s | km/s | 2.848e+05 to 2.272e+06 | 613 |
| carve | M2 | yes | off | yes | m/s | km/s | 0 to 1.195e+05 | 613 |

Physics checks, independent of the request: the map's flux and mean velocity from the page's units (its displayUnit scale) against the selection's own statistics (`mask_stats_bin`).

| selection | Σ M0 / beam (Jy km/s) | flux of mask_stats (Jy km/s) | M0-weighted M1 (km/s) | spectrum-weighted velocity (km/s) | beam (pixels) |
|---|---|---|---|---|---|
| new | 6.0607 | 6.0607 | 1572.44 | 1572.44 | 30.66 |
| carve | 3.1306 | 3.1306 | 1300.06 | 1300.06 | 30.66 |

## 4. PV across NGC 3187

Vertices placed by the page's pick (fractional pixels): (443.5, 287.6), (465.5, 297.6), (487.5, 307.6); width 1 px.

*request = desktop*: the page's request equals `BackendClient::requestPv`'s keys (read from its C++ source) with the same vertices and vtkWindowCube's default width (read from its header). *rows = desktop's*: the page's rows equal a line-for-line Python port of the desktop's `rowsOnUniformSpectralGrid` applied to the backend's answer; *drawn*: the page's own `colourise` output (canvas pixels) equals those desktop rows coloured with the page's table and range, bottom row at the bottom.

| cube | CDELT3 | request = desktop | rows = desktop's rows | drawn = desktop rows | rows ascending | velocity bottom → top (km/s) | peak at (km/s) | samples × channels |
|---|---|---|---|---|---|---|---|---|
| HCG 44 | 8244 | yes | yes | yes | yes | 888.6 → 2182.9 | 1515.1 | 49 × 158 |
| HCG 44, spectral axis reversed | -8244 | yes | yes | yes | yes | 888.6 → 2182.9 | 1515.1 | 49 × 158 |

Reversed spectral axis: the page's picture is the same row for row: yes (largest difference 0).

PV recipe parameters: {"Path": "3 points", "Width": "1 px", "From": "(443.5, 287.6)", "To": "(487.5, 307.6)", "Measured on": "a 3-point PV path, 1 px wide"}; origin {"kind": "pvpath", "points": [[443.45142816281293, 287.5672040405115], [465.45142816281293, 297.5672040405115], [487.45142816281293, 307.5672040405115]], "width": 1}.
