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

## Desktop overlay (cube viewer)

In the 3-D cube viewer (`vtkWindowCube`): **Tools ▸ Kinematic Model Overlay…**.

The workflow is split in two:

- **The dialog collects only the *model* parameters** (centre, PA, inclination,
  disk radius, Vsys, Vrot, σ). Changing any of these defines a different model,
  so *Overlay* triggers a backend fetch + contour (`vtkFlyingEdges3D`) and adds
  the surface to the cube renderer.
- **A live "Kinematic Model" dock panel** then restyles the overlay with **no
  re-fetch** — the model cube and its FlyingEdges filter are kept client-side:
  - **Contour level (% peak)** — re-`SetValue` on the kept filter.
  - **Opacity**, **Colour** (colour picker), **Representation** (Wireframe /
    Surface), **Show overlay** (visibility), **Remove overlay**.
  - **Contour on 2D channel maps** (default on) — see below.
  The panel is reachable again via **View ▸ Kinematic Model** if you close it.

### 2-D channel-map contour (model-vs-data)

The clearest model-vs-data comparison is **per channel**, not in 3-D — this is
how kinematic models are normally read (SlicerAstro / ³ᴰBarolo / TiRiFiC). With
*Contour on 2D channel maps* enabled, the model's contour for the **currently
displayed velocity channel** is drawn over the 2-D channel map; it follows the
channel slider, the panel's contour level and the overlay colour. A 2-D contour
is never occluded by the volume, so it reads cleanly:

- Data emission **inside** the contour → gas following regular rotation.
- Data emission **outside** the contour, or at a velocity the contour doesn't
  reach → anomalous / extra-planar gas (e.g. a high-velocity cloud).

Implementation: the model's XY plane for the channel is extracted from the kept
model cube at full XY resolution / unit spacing — exactly the 2-D slice's
geometry, regardless of any 3-D-display downsampling — contoured with
`vtkContourFilter`, and drawn as a polyline actor on the slice renderer
(`sliceWin`), refreshed from `applyRemoteSliceResult()` on every channel change.

**Representation.** Default is **Wireframe** — a mesh lets the data volume show
*through* the model, the way a kinematic model is normally read against the cube
(SlicerAstro / ³ᴰBarolo). Where the data pokes outside the mesh, or sits at a
velocity the mesh doesn't cover, you're seeing gas that departs from regular
rotation. In 3-D the composite volume rendering is opaque and can occlude the
model where it sits inside the emission — lower the cube's volume opacity or
switch the cube to **Isosurface** mode to compare two surfaces directly, and
rotate the box to view the disk edge-on along the velocity axis.

- **Off the UI thread.** The fetch, the float→`vtkImageData` copy and the
  contour all run on a `QtConcurrent` worker that captures only plain values
  (never `this`); a `QFutureWatcher` member delivers the result on the GUI
  thread, which only does `AddActor` + `Render`. Closing the window during a
  build is safe.
- **Registration.** The model is the full grid, but the cube may be showing a
  downsampled preview, the full-resolution cube, or an ROI sub-volume. Before
  launching, the viewer snapshots the live display image geometry
  (`cubeDisplaySource`) and the worker maps the model onto it: for a preview /
  full-res display (origin 0) the model is scaled to the displayed extent
  (spacing `(dispDim-1)/(fullDim-1)`, → 1 at full res); for an ROI (placed at
  its global voxel offset) the model stays in global voxel coordinates.
- **Stale-result guard.** If the display is reloaded (preview→full-res / ROI)
  while the build runs, the finished handler detects the geometry change and
  discards the now-misregistered actor, prompting a re-run.
- **Empty model.** If no emission lands on a channel (e.g. Vsys outside the
  cube's spectral range, or a FREQ cube with no rest frequency), the GUI reports
  the spectral mode/unit so you can re-enter Vsys/Vrot/σ in the right unit.
- **ROI note.** In ROI mode the full model surface is *not* clipped to the
  loaded sub-volume — it shows the whole disk for context, so part of it may
  render outside the visible data box.

## Limitations (phase-1) / roadmap

- Single PA/incl and a flat rotation curve; **per-ring** parameters (a real
  tilted-ring fit, à la 3DBarolo/TiRiFiC) are the next refinement.
- Uniform disk intensity (the overlay is about kinematics/footprint, not flux).
- Image-pixel PA rather than WCS sky PA (see convention note).
- Full in-memory materialisation bounded by the voxel cap.
- Overlay registration distinguishes preview/full-res from ROI by display
  origin; an ROI starting at voxel (0,0,0) is treated as a full-extent display.
- The worker builds the mapper/actor off the GUI thread (fully built before
  hand-off); baking `vtkPolyData` and creating the actor on the GUI thread is a
  possible future cleanup.
