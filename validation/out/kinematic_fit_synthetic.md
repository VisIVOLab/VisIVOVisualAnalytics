# The comparator's fit on discs with known kinematics

Two discs built with the application's tilted-ring model on a 96×96×64 grid (7.9 km/s channels, line σ 12 km/s), Gaussian noise at two peak signal-to-noise levels, fitted by `app.compute.kinematic_fit` — what the viewer's *Fit from the lasso selection* runs — on the footprint of the noiseless model. The discs carry an exponential surface brightness (scale length 30 px, in the local ring geometry), so their isophotes mean something. Inclination comes from the moment-0 isophotes; the velocity field fixes V_rot sin i, so V_rot is only as good as that inclination.

## Flat disc

Input: PA 40°, i 60°, V_sys 1050 km/s, V_rot 150 km/s (V_rot sin i = 129.9), centre (47.5, 49).

| noise | centre Δx, Δy (px) | PA (°) | i (°) | V_sys (km/s) | V_rot sin i (km/s) | V_rot (km/s) | median residual flat / rings (km/s) | ring PA spread (°) |
|---|---|---|---|---|---|---|---|---|
| S/N 25 | -0.0, -0.0 | 40.0 (+0.0) | 59.9 (-0.1) | 1050.0 (+0.0) | 129.9 (-0.1) | 150.0 (+0.0) | 0.7 / 0.9 | 0.0 |
| S/N 8 | -0.0, -0.0 | 40.0 (+0.0) | 60.1 (+0.1) | 1050.2 (+0.2) | 130.0 (+0.1) | 150.0 (-0.0) | 2.0 / 2.4 | 0.0 |

## Warped disc

Input, at the ring radii (interpolated between them):

| r (px) | PA (°) | i (°) | V_rot (km/s) | V_rot sin i (km/s) |
|---|---|---|---|---|
| 6 | 40 | 60 | 90 | 77.9 |
| 12 | 40 | 60 | 140 | 121.2 |
| 18 | 45 | 58 | 150 | 127.2 |
| 24 | 55 | 55 | 150 | 122.9 |
| 30 | 70 | 50 | 150 | 114.9 |

### S/N 25

Flat disc through it: PA 52.0°, V_rot sin i 113.9 km/s, median residual 10.0 km/s; the ring solution brings the residual to 2.7 km/s.

| ring r (px) | pixels | PA fit / true (°) | i fit / true (°) | V_rot sin i fit / true (km/s) |
|---|---|---|---|---|
| 6.6 | 114 | 42.0 / 40.0 (+2.0) | 58.3 / 60.0 (-1.7) | 86.6 / 82.1 (+4.5) |
| 11.0 | 194 | 42.0 / 40.0 (+2.0) | 58.7 / 60.0 (-1.3) | 114.8 / 113.7 (+1.1) |
| 15.3 | 266 | 44.0 / 42.8 (+1.2) | 59.1 / 58.9 (+0.2) | 126.2 / 124.6 (+1.6) |
| 19.7 | 354 | 48.0 / 47.9 (+0.1) | 57.5 / 57.1 (+0.3) | 124.6 / 126.0 (-1.4) |
| 24.1 | 420 | 56.0 / 55.3 (+0.7) | 55.1 / 54.9 (+0.2) | 116.5 / 122.7 (-6.3) |
| 28.5 | 301 | 62.0 / 66.2 (-4.2) | 52.3 / 51.3 (+1.1) | 113.6 / 117.0 (-3.4) |
| 32.9 | 129 | 66.0 / 70.0 (-4.0) | 52.3 / 50.0 (+2.3) | 117.9 / 114.9 (+3.0) |

### S/N 8

Flat disc through it: PA 52.0°, V_rot sin i 113.5 km/s, median residual 10.3 km/s; the ring solution brings the residual to 3.3 km/s.

| ring r (px) | pixels | PA fit / true (°) | i fit / true (°) | V_rot sin i fit / true (km/s) |
|---|---|---|---|---|
| 6.6 | 114 | 42.0 / 40.0 (+2.0) | 57.6 / 60.0 (-2.4) | 86.3 / 82.1 (+4.2) |
| 11.0 | 197 | 42.0 / 40.0 (+2.0) | 57.6 / 60.0 (-2.4) | 114.3 / 113.7 (+0.6) |
| 15.3 | 263 | 44.0 / 42.8 (+1.2) | 55.8 / 58.9 (-3.1) | 123.1 / 124.6 (-1.5) |
| 19.7 | 351 | 48.0 / 47.9 (+0.1) | 54.5 / 57.1 (-2.6) | 121.8 / 126.0 (-4.2) |
| 24.1 | 402 | 56.0 / 55.3 (+0.7) | 52.9 / 54.9 (-2.0) | 114.8 / 122.7 (-8.0) |
| 28.5 | 278 | 62.0 / 66.2 (-4.2) | 50.3 / 51.3 (-0.9) | 112.9 / 117.0 (-4.1) |
| 32.9 | 110 | 66.0 / 70.0 (-4.0) | 50.3 / 50.0 (+0.3) | 116.0 / 114.9 (+1.1) |

