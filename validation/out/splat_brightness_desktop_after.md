# Desktop splat brightness against N (issue #457) - catalogue viewer

Plummer sphere a = 1, cut at 10 a, face on over +-4 a at 1000 px, splat size 0.75, render budget 1000000. Analytic Sigma(3a)/Sigma(0) of the cut sphere: 0.01007.

| N | stride | mean at the lowest intensity | mean ratio to first N | profile / analytic - 1 | mean ratio at the desktop's intensity 35 | pass |
|---:|---:|---:|---:|---:|---:|:---:|
| 100000 | 1 | 0.01397 | 1.000 | -0.056 | 1.000 | yes |
| 1000000 | 1 | 0.01395 | 0.999 | -0.025 | 0.996 | yes |
| 3660000 | 4 | 0.01395 | 0.998 | -0.029 | 0.994 | yes |
| 10000000 | 10 | 0.01395 | 0.998 | -0.015 | 0.996 | yes |
| 30000000 | 30 | 0.01395 | 0.999 | -0.023 | 0.996 | yes |

## Every row drawn (render budget off)

Plummer sphere a = 1, cut at 10 a, face on over +-4 a at 1000 px, splat size 0.75, render budget 0. Analytic Sigma(3a)/Sigma(0) of the cut sphere: 0.01007.

| N | stride | mean at the lowest intensity | mean ratio to first N | profile / analytic - 1 | mean ratio at the desktop's intensity 35 | pass |
|---:|---:|---:|---:|---:|---:|:---:|
| 100000 | 1 | 0.01397 | 1.000 | -0.056 | 1.000 | yes |
| 1000000 | 1 | 0.01395 | 0.999 | -0.025 | 0.996 | yes |
| 3660000 | 1 | 0.01393 | 0.997 | -0.001 | 0.997 | yes |
| 10000000 | 1 | 0.01392 | 0.996 | -0.012 | 0.997 | yes |
| 30000000 | 1 | 0.01391 | 0.996 | +0.001 | 0.998 | yes |

## VBT point viewer (vtkWindowVbt)

Plummer sphere a = 1, cut at 10 a, face on over +-4 a at 1000 px, splat size 3.00, render budget 0. Analytic Sigma(3a)/Sigma(0) of the cut sphere: 0.01007.

| N | stride | mean at the lowest intensity | mean ratio to first N | profile / analytic - 1 | mean ratio at the desktop's intensity 35 | pass |
|---:|---:|---:|---:|---:|---:|:---:|
| 100000 | 1 | 0.01393 | 1.000 | +0.100 | 1.000 | yes |
| 1000000 | 1 | 0.01391 | 0.999 | +0.002 | 1.025 | yes |
| 3660000 | 1 | 0.01390 | 0.998 | +0.003 | 1.032 | yes |
| 10000000 | 1 | 0.01390 | 0.998 | -0.007 | 1.031 | yes |
| 30000000 | 1 | 0.01390 | 0.998 | +0.003 | 1.032 | yes |
