# Tilted-Ring Kinematic Model (F2)

Overlay a model of a *regularly rotating gas disk* on an observed HI spectral
cube. Where the data emits but the model does not — or at a different
line-of-sight velocity — you are looking at gas that departs from ordered
rotation: extra-planar gas, inflows/outflows, warps, tidal debris. This is the
SlicerAstro / van der Hulst "model-vs-data" use case, served by the analysis
backend and contoured in the 3-D cube viewer.

Backend: `backend/app/kinematic_model.py` + endpoint
`POST /v1/cube/kinematic_model_bin` (`backend/app/routers/cube.py`).

---

## The model

Phase-1 is a single-disk **tilted-ring** model: one position angle, one
inclination, a **flat (constant) rotation velocity** out to a disk radius, and a
Gaussian line of fixed dispersion. For each sky pixel the model places a
unit-amplitude Gaussian line centred on the line-of-sight velocity

```
Vlos(x, y) = Vsys + Vrot · sin(incl) · cos(θ)
```

where `r, θ` are the galactocentric radius and azimuth from the standard
tilted-ring deprojection of the pixel about the disk centre `(x0, y0)`:

```
x_d = -(x-x0)·sin(PA) + (y-y0)·cos(PA)
y_d = [-(x-x0)·cos(PA) - (y-y0)·sin(PA)] / cos(incl)
r   = √(x_d² + y_d²)
cos θ = x_d / r
```

Pixels with `r > rmax` emit nothing. The cube is **not** renormalised, so the
realised peak depends on how close a channel falls to each pixel's `Vlos`.

### Parameters (request body)

| Field | Unit | Meaning |
|-------|------|---------|
| `dataset_id` | — | open cube dataset |
| `x0`, `y0` | pixels | disk centre |
| `pa_deg` | degrees | major-axis position angle (see convention below) |
| `incl_deg` | degrees | inclination, `0` face-on … `90` edge-on (`0–90`) |
| `vsys` | spectral unit | systemic velocity |
| `vrot` | spectral unit | flat rotation velocity |
| `rmax_pix` | pixels | disk radius (`> 0`) |
| `sigma` | spectral unit | line dispersion (`> 0`) |

All floats are validated finite at the API boundary (NaN/inf → HTTP 422), with
`incl_deg ∈ [0, 90]` and `rmax_pix, sigma > 0`.

---

## Velocity units (FREQ vs velocity axes)

The model is built in **velocity**. The backend converts the cube's spectral
axis to **radio-convention velocity (km/s)** before modelling, so `vsys / vrot /
sigma` are always entered in km/s when the conversion succeeds:

- **Velocity axis** (`VRAD`/`VELO`/… or a unit equivalent to km/s) → converted
  directly with astropy.
- **Frequency axis** (`FREQ`, Hz) with a rest frequency in the header
  (`RESTFRQ`/`RESTFREQ` > 0) → `v = c · (f₀ − f) / f₀`.

The realised working unit and mode are returned in the response headers and the
GUI labels its fields accordingly:

| Header | Values | Meaning |
|--------|--------|---------|
| `X-Visivo-Spectral-Mode` | `velocity` | axis converted to km/s — enter km/s |
| | `native` | FREQ axis with no rest frequency, or an unrecognised type — enter values in the cube's own unit |
| | `channel` | WCS unusable — enter values as channel indices |
| `X-Visivo-Spectral-Axis-Unit` | e.g. `km/s` | unit the model was built in |

> **PA convention.** `pa_deg` is measured in **image-pixel** coordinates
> (`PA = 0` → major axis along **+y** of the array, increasing toward **+x**),
> *not* sky north-through-east. The viewer is meant to tune PA visually against
> the data, so the UI labels this as an image/viewer PA. Deriving sky PA from
> the celestial WCS is a planned refinement.

---

## Wire format

The model streams as the same binary layout as `cube/subvolume_bin`: raw
little-endian `float32`, x-fastest (vtkImageData point order), one value per
voxel, with geometry in response headers:

| Header | Meaning |
|--------|---------|
| `X-Visivo-Width` / `-Height` / `-Depth` | nx / ny / nchan |
| `X-Visivo-Scalar-Type` | `float32` |
| `X-Visivo-Peak` | model peak (for contour scaling) |
| `X-Visivo-Byte-Order` | `little` |

The desktop builds a `vtkImageData` matching the data cube's grid and contours
it (e.g. at a fraction of `X-Visivo-Peak`) as a semi-transparent overlay.

---

## Size guard

A model cube is materialised in full (float32 + a raw bytes copy ≈ 2× the cube),
so requests are bounded by a voxel cap — `VISIVO_KINEMATIC_MAX_VOXELS`
(default 256 M voxels ≈ 1 GiB cube + 1 GiB wire). The cap is resolved in the
parent process and passed to the worker (the env var would not survive the
pre-forked pool). The geometry is checked **before** any allocation; oversize
requests return **HTTP 413** — crop the cube to a sub-region or raise the cap.
Chunked/streamed generation for very large cubes is future work.

---

## Limitations (phase-1) / roadmap

- Single PA/incl and a flat rotation curve; **per-ring** parameters (a real
  tilted-ring fit, à la 3DBarolo/TiRiFiC) are the next refinement.
- Uniform disk intensity (the overlay is about kinematics/footprint, not flux).
- Image-pixel PA rather than WCS sky PA (see convention note).
- Full in-memory materialisation bounded by the voxel cap.
