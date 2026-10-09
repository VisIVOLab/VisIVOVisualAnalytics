# VisIVO backend performance benchmark

Machine: macOS-26.5.2-arm64-arm-64bit. Wall time (s) and PEAK RESIDENT memory (MB), each op in its own process.

**Interactivity:** every op stays sub-3 s even on the 2 GB cube — the paper's interactive-on-multi-GB claim holds.

**Memory — read carefully:** peak RSS (ru_maxrss) includes the OS page cache of the memmap, so any op that touches the whole cube (moment, lasso) shows RSS ≳ cube size even though the design never materialises the cube as an array. The out-of-core benefit is therefore the *difference* vs the in-memory path, not a low absolute RSS: On **cubehi-m31** the streaming moment-0 peaked at 957 MB vs 2144 MB for the in-memory path (**2.2× less**). Preview/slice touch only a downsampled volume / a single plane, so their RSS IS bounded (~150–300 MB) regardless of cube size. `moment0` = streaming path, `moment0(mat)` = forced in-memory (only run on cubes ≤ 512 MB to avoid OOM).

| Cube | Dims | Size (MB) | Preview s / RSS | Slice s / RSS | Moment-0 s / RSS | Moment-0(mat) RSS | Lasso s / RSS |
|---|---|---:|---|---|---|---:|---|
| WALLABY | 196×280×109 | 24 | 0.241/169 | 0.192/140 | 0.201/270 | 368 | 0.41/575 |
| cubehi-m31 | 512×512×200 | 210 | 0.196/199 | 0.191/141 | 0.302/957 | 2144 | 1.091/2173 |
| HGC44 | 700×700×158 | 310 | 0.247/217 | 0.245/154 | 0.404/1851 | 3105 | 0.79/1012 |
| fornax | 1046×952×398 | 1585 | 0.233/275 | 0.241/161 | 0.926/4419 | — | 1.925/2234 |
| 3D-Ape | 661×661×1218 | 2129 | 0.236/307 | 0.224/152 | 1.011/3253 | — | 1.745/2168 |
