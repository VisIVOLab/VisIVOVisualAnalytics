# Refinement in the VR page: quality and times (issue #170)

Cube `hgc44_3axes.fits` (HCG 44), headless Chrome on ANGLE (Apple, ANGLE Metal Renderer: Apple M5 Max, Unspecified Version), canvas 1600 x about 1000 px, local backend on the same Mac.

## Quality at a still pose

Against the backend's reference (full cube, full canvas size, JPEG 100, quarter-voxel rays). Emission: the pixels of the emission, grown by 8 px.

| cube on the page | image | PSNR emission (dB) | SSIM emission | PSNR frame (dB) | SSIM frame |
|---|---|---|---|---|---|
| 234×234×158 uint8, binned 3×3×1 (mean) from 700×700×158 | page alone (reduced cube) | 33.01 | 0.9650 | 49.13 | 0.9982 |
| 234×234×158 uint8, binned 3×3×1 (mean) from 700×700×158 | refined, 0.6 x size, JPEG 85 | 37.80 | 0.9571 | 56.01 | 0.9993 |
| 234×234×158 uint8, binned 3×3×1 (mean) from 700×700×158 | refined, 1 x size, JPEG 95 | 45.95 | 0.9876 | 63.88 | 0.9998 |
| 350×350×158 uint8, binned 2×2×1 (mean) from 700×700×158 | page alone (reduced cube) | 36.69 | 0.9820 | 51.31 | 0.9986 |
| 350×350×158 uint8, binned 2×2×1 (mean) from 700×700×158 | refined, 0.6 x size, JPEG 85 | 36.57 | 0.9502 | 56.04 | 0.9994 |
| 350×350×158 uint8, binned 2×2×1 (mean) from 700×700×158 | refined, 1 x size, JPEG 95 | 44.95 | 0.9858 | 64.34 | 0.9998 |

## Reprojection

A frame for pose A drawn by the page at pose B, against the backend's frame for B (full size, JPEG 95: the page's defaults); PSNR (dB) on the emission. *Orbit*: the eye turned about the cube's centre at 1 m (translation 1.7 cm per degree); *turn*: the head turned in place.

| cube | motion | none (A as is) | plane through the centre | depth (first hit) | page alone at B |
|---|---|---|---|---|---|
| auto | orbit 0.5 deg | 37.60 | 36.59 | 42.58 | 31.50 |
| auto | orbit 1 deg | 32.42 | 31.35 | 38.80 | 31.43 |
| auto | orbit 2 deg | 27.76 | 26.74 | 34.79 | 31.65 |
| auto | orbit 4 deg | 24.28 | 23.27 | 30.40 | 32.01 |
| auto | turn 2 deg | 19.38 | 45.97 | 45.92 | 33.66 |
| auto | turn 5 deg | 19.86 | 46.05 | 45.99 | 33.96 |
| bin2 | orbit 0.5 deg | 36.76 | 35.84 | 43.28 | 35.44 |
| bin2 | orbit 1 deg | 31.64 | 30.62 | 40.97 | 35.44 |
| bin2 | orbit 2 deg | 26.86 | 25.94 | 37.66 | 35.48 |
| bin2 | orbit 4 deg | 23.33 | 22.38 | 33.15 | 35.70 |
| bin2 | turn 2 deg | 18.71 | 45.54 | 45.50 | 37.61 |
| bin2 | turn 5 deg | 19.21 | 45.58 | 45.55 | 37.88 |

## Times

**auto** (234×234×158 uint8, binned 3×3×1 (mean) from 700×700×158)

- Camera stopped to refined frame first shown: 268 ms median, 270 p95 (n=15); fully faded in (120 ms fade): 383 ms median, 385 p95 (n=15); timeouts 0.
- The same from the moment the page decided the view had stopped (what the panel shows): 83 ms median, 83 p95 (n=15) first shown, 200 ms median, 200 p95 (n=15) fully in.
- Pose sent to frame decoded: 72 ms median, 83 p95; to frame received 67 ms; JPEG and depth decode 4.4 ms; texture upload 1.30 ms median, 1.70 p95.
- Last frame: 65 KB; server {"wait": 2.193, "render": 36.834, "depth": 5.092, "encode": 17.667, "handoff": 0.377, "queue": 0.008, "total": 62.171}.
- Poses sent 48, frames received 46, dropped as stale 0, merged by the backend 0.
- Local frame, page alone: GPU 1.307 ms, CPU 0.099 ms (mean of 2 runs of 3 s).
- Local frame, with the refined frame composited: GPU 1.471 ms, CPU 0.101 ms (mean of 2 runs of 3 s).

**bin2** (350×350×158 uint8, binned 2×2×1 (mean) from 700×700×158)

- Camera stopped to refined frame first shown: 408 ms median, 472 p95 (n=15); fully faded in (120 ms fade): 516 ms median, 584 p95 (n=15); timeouts 0.
- The same from the moment the page decided the view had stopped (what the panel shows): 217 ms median, 283 p95 (n=15) first shown, 333 ms median, 400 p95 (n=15) fully in.
- Pose sent to frame decoded: 74 ms median, 233 p95; to frame received 69 ms; JPEG and depth decode 4.6 ms; texture upload 1.30 ms median, 6.00 p95.
- Last frame: 56 KB; server {"wait": 3.681, "render": 107.719, "depth": 17.835, "encode": 64.194, "handoff": 1.149, "queue": 0.017, "total": 194.595}.
- Poses sent 95, frames received 84, dropped as stale 0, merged by the backend 0.
- Local frame, page alone: GPU 1.354 ms, CPU 0.098 ms (mean of 2 runs of 3 s).
- Local frame, with the refined frame composited: GPU 1.525 ms, CPU 0.124 ms (mean of 2 runs of 3 s).

