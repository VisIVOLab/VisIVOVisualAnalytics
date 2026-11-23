# Desktop splat brightness against N (issue #457) - catalogue viewer - before

Plummer sphere a = 1, cut at 10 a, face on over +-4 a at 1000 px, splat size 0.75, render budget 1000000. Analytic Sigma(3a)/Sigma(0) of the cut sphere: 0.01007.

| N | stride | mean at the lowest intensity | mean ratio to first N | profile / analytic - 1 | mean ratio at the desktop's intensity 35 | pass |
|---:|---:|---:|---:|---:|---:|:---:|
| 100000 | 1 | 0.23684 | 1.000 | -0.716 | 1.000 | no |
| 1000000 | 1 | 0.35264 | 1.489 | -0.713 | 1.489 | no |
| 3660000 | 4 | 0.33648 | 1.421 | -0.738 | 1.421 | no |
| 10000000 | 10 | 0.35276 | 1.489 | -0.707 | 1.489 | no |
| 30000000 | 30 | 0.35278 | 1.490 | -0.707 | 1.490 | no |

## Every row drawn (render budget off)

Plummer sphere a = 1, cut at 10 a, face on over +-4 a at 1000 px, splat size 0.75, render budget 0. Analytic Sigma(3a)/Sigma(0) of the cut sphere: 0.01007.

| N | stride | mean at the lowest intensity | mean ratio to first N | profile / analytic - 1 | mean ratio at the desktop's intensity 35 | pass |
|---:|---:|---:|---:|---:|---:|:---:|
| 100000 | 1 | 0.23684 | 1.000 | -0.716 | 1.000 | no |
| 1000000 | 1 | 0.35264 | 1.489 | -0.713 | 1.489 | no |
| 3660000 | 1 | 0.43393 | 1.832 | -0.674 | 1.832 | no |
| 10000000 | 1 | 0.50487 | 2.132 | -0.545 | 2.132 | no |
| 30000000 | 1 | 0.58999 | 2.491 | -0.341 | 2.491 | no |

## VBT point viewer (vtkWindowVbt)

Plummer sphere a = 1, cut at 10 a, face on over +-4 a at 1000 px, splat size 3.00, render budget 0. Analytic Sigma(3a)/Sigma(0) of the cut sphere: 0.01007.

| N | stride | mean at the lowest intensity | mean ratio to first N | profile / analytic - 1 | mean ratio at the desktop's intensity 35 | pass |
|---:|---:|---:|---:|---:|---:|:---:|
| 100000 | 1 | 0.90088 | 1.000 | -0.007 | 1.000 | yes |
| 1000000 | 1 | 0.98226 | 1.090 | -0.007 | 0.999 | no |
| 3660000 | 1 | 0.99720 | 1.107 | -0.007 | 1.001 | no |
| 10000000 | 1 | 0.99987 | 1.110 | -0.007 | 1.003 | no |
| 30000000 | 1 | 1.00000 | 1.110 | -0.007 | 1.003 | no |
