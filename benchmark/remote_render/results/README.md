# Results on PLEIADI-GPU (INAF), 2026-10-30

Backend code of VisIVO Visual Analytics commit `1337188`, image
`backend-egl.sif` (VTK with EGL), Apertif DR1 cube
`3D_Ape-DR1_200426041_AP_B021_HI_image_cube0.fits` (661 × 661 × 1218, 2 GB),
2540 × 1560 frames, the backend's own threshold. Nodes: POWER9, 4 × V100
16 GB.

| File | Script | Setting |
|---|---|---|
| `push_stream_pleiadi.json`, `push_stream_pleiadi_2.json` | `push_stream_fps.py` | two sessions, three rotations each, from a laptop through the SSH tunnel to the backend's node (r33c1s02, job of 4 GPUs) |
| `multi_gpu_pleiadi.jsonl` | `multi_gpu_sessions.py` | on node r33c1s01, one job of 4 GPUs: 1 view; 2 views on one GPU, then spread; 4 views on one GPU, then spread |

Summary: through the tunnel 25.7-26.4 frames per second once the drag
level has settled (9.1 and 15.4 on the first rotation of each session,
while it adapts), about 42 kB a frame. On the node: one view 29.5; two
views 16.9 each on one GPU, 28.2 spread; four views 6.8 on one GPU,
20.0 spread (16.1-22.9).

# Out-of-core views on PLEIADI-GPU, 2026-11-01

Backend code of the commit that adds these files (issue #70), image
`backend-egl.sif` (VTK 9.3.1 with EGL), node r33c1s01, one V100 16 GB,
`out_of_core_views.py` with its defaults (1280 × 800 frames, the backend's own
threshold). Pyramids built beforehand in `VISIVO_PYRAMID_DIR`.

| File | Cube | Setting |
|---|---|---|
| `out_of_core_adp_pleiadi.json` | `ADP.2025-09-05T10_57_51.916.fits` (MUSE, 772 × 392 × 1604, 1.8 GB as float32) | `--mode both`: the same views in core and out of core |
| `out_of_core_synthetic_pleiadi.json` | `make_synthetic_cube.py` default (2048 × 2048 × 1500, 23.4 GiB, larger than the GPU) | `--mode ooc` |

Summary. On the 1.8 GB cube, out of core against in core: the session opens
in 1.9 s instead of loading for 45 s; the views stop changing within 0.7 s
(3.2 s for MIP, which draws the whole of level 0); the mean absolute frame
difference is 1.4 to 2.1 (of 255) for home, MIP and MIP zoomed in, 6.7 from
the side and 11.7 zoomed in, where coarser blocks thin the noise in front of
the cutting plane; dragging runs at 68 frames per second against 216; the
render process peaks at 4.8 GB against 4.7 GB. On the 23.4 GiB cube: opens in
2.6 s, views settle within 1.3 s, dragging at 54 frames per second, the render
process peaks at 2.5 GB.
