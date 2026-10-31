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
