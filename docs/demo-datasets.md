# Demo datasets (feature test matrix)

Synthetic datasets that exercise every desktop viewer feature end-to-end, so you
can smoke-test the app without hunting for real data. They are produced by a
single reproducible generator (fixed RNG seed), so the **script is the
downloadable artifact** — commit it and regenerate anywhere. A regression test
(`backend/tests/test_demo_data.py`) generates the set and asserts every file
loads through the backend paths + that the HI cube matches the kinematic model at
the documented params, so the demo contract can't silently rot.

## Generate

```bash
backend/.venv/bin/python backend/scripts/generate_demo_data.py [OUTPUT_DIR]
```

Default `OUTPUT_DIR` is `~/VisIVO_demo_data` — the desktop file browser opens at
your home directory, so the files are immediately reachable. Total size ≈ 13 MB.
Requires the backend venv (numpy + astropy). Re-running overwrites the same
filenames in place.

Manifest:

```
~/VisIVO_demo_data/
  catalogue_sky.csv            catalogue_cartesian.csv     catalogue.speck
  tbl_snapshot/snap_demo_1_1_1.tbl  … _1_1_2.tbl  … _1_2_1.tbl
  velocity_field.fits          hi_cube.fits                image_2d.fits
  combo/velocity.fits          combo/galaxies.csv
```

## Prerequisites

Start the backend and the app (a bearer token is printed at backend startup):

```bash
cd backend && .venv/bin/python -m uvicorn app.main:app --host 127.0.0.1 --port 8000
open build/VisIVOVisualAnalytics.app
```

---

## 1 · 3-D catalogue — sky  ·  `catalogue_sky.csv`

2 500 sources with RA/Dec + redshift + morphology/size/flux.

**Open:** *Data ▸ Open 3D Catalogue (CSV)…* → `catalogue_sky.csv`.

| Feature | Do this | Expect |
|---|---|---|
| Sky path + distance | opens straight away | sources on a shell at the redshift-derived distance |
| Cosmology selector | switch Planck18 → Planck15/WMAP9 | distances recompute (async for the non-local models) |
| Coordinate frame | Frame → Galactic | positions rotate to (l, b) |
| Morphology colour | Color → *Morphology color* | halo/relic/… categories get distinct colours |
| Size-by-attribute | Size → *Major axis* | glyphs scale by `majoraxis` |
| Filters + paging | Filters tab → add a `flux > 20` filter → Apply | source count drops; *Load more / Load all* page the rest |

## 2 · 3-D catalogue — Cartesian  ·  `catalogue_cartesian.csv`

17 000 galaxies (4 clusters + field) in Mpc with `vx/vy/vz` + `mass`.

**Open:** *Data ▸ Open 3D Catalogue (CSV)…* → `catalogue_cartesian.csv`.

| Feature | Do this | Expect |
|---|---|---|
| Cartesian detection | opens | X/Y/Z rendered directly (no RA/Dec machinery) |
| Speed \|v\| colour | Color → *Speed \|v\|* | colour ∝ √(vx²+vy²+vz²); low in the centre (infall) |
| Raw-column colour | Color → `mass` | colour by mass |
| Velocity arrows | Shape → *Arrows (velocity)* | each galaxy an arrow along its peculiar velocity |
| Render budget / hover / pick | rotate; hover a point; click | smooth at 17 k; yellow hover sphere; click selects |

## 3 · Partiview `.speck`  ·  `catalogue.speck`

9 000 points with `luminosity` + `temperature` datavars.

**Open:** *Data ▸ Open 3D Catalogue (CSV)…* → `catalogue.speck` (Cartesian). Colour by
`luminosity` or `temperature`.

## 4 · IPAC `.tbl` snapshot (multi-file)  ·  `tbl_snapshot/*.tbl`

Three sub-box `.tbl` tables with **identical columns** (`x y z vx vy vz LogMass`).

**Open:** *Data ▸ Open 3D Catalogue (CSV)…* → any `snap_demo_*.tbl`. When prompted to
load the same-extension siblings, accept → all three concatenate into one
catalogue (~12 k points). Exercises the IPAC reader, multi-file concat, and
*Load all*. Also colour by *Speed \|v\|* / draw velocity arrows.

## 5 · Velocity field  ·  `velocity_field.fits`

64³ 3-component field superposing **one attractor** (sink → supercluster) and
**one repeller** (source → void), so one field shows the full cosmography.

**Open:** *Data ▸ Open Velocity Field…* → `velocity_field.fits`, then set
**Box size (Mpc) = 200**.

| Feature | Do this | Expect |
|---|---|---|
| Glyph arrows / streamlines | toggle each | arrows converge on the sink, diverge from the source |
| Colour + scalar bar | Colormap combo | speed LUT |
| **Show basins** | check it (Attraction) | the attraction basin around the sink |
| Basin type | Attraction ↔ Repulsion | Repulsion → the basin around the source (repeller) |
| **Overdensity (−∇·v)** | check it | an orange core at the **sink** (converging → overdense) |
| **Void (+∇·v)** | check it | a blue core at the **source** (diverging → underdense); show both at once for the full picture |
| ∇·v level % | drag | both isosurfaces tighten toward their peaks |

*(LOD downsampling + full-res ROI only engage above 128³, so they read "n/a" on
this 64³ demo — that path needs a larger field.)*

## 6 · CosmicFlows combo  ·  `combo/velocity.fits` + `combo/galaxies.csv`

A radial-infall field **and** a galaxy catalogue in the **same** 200 Mpc frame —
co-registered, so the overlay actually aligns.

**Steps:** open `combo/velocity.fits`, set **Box size (Mpc) = 200**, then
**Overlay galaxy catalogue…** → `combo/galaxies.csv`.

| Feature | Do this | Expect |
|---|---|---|
| Overlay (async) | pick the CSV | 23.5 k gold points land **inside** the field box; button disables during load |
| Galaxy colour + legend | Galaxy color → *Speed \|v\|* (or `mass`) | recolours; a legend appears **left**; centre = low speed (cyan), edge = high (red) |
| **Field agreement** | Galaxy color → *Field agreement* | ≈ +1 (red) almost everywhere — the demo galaxies carry the field velocity + noise, so they flow *with* the reconstruction |
| **Residual \|v\|** | Galaxy color → *Residual \|v\|* | small, roughly uniform (≈ the 30 km/s injected noise) — the reconstruction explains the rest |
| Velocity arrows | Galaxy shape → *Velocity arrows* | galaxy arrows point toward centre — **same direction as the field** |
| Residual arrows | Galaxy shape → *Residual arrows* | **tiny, randomly-oriented** arrows (the ~30 km/s noise) — the reconstruction explains the coherent motion; a real missed attractor would show long, aligned residuals |
| Density isosurface | check *Galaxy density*; drag *Density level %* | translucent cyan surface wraps the 5 clusters; tightens with level |
| Show galaxies / size | toggle / spin | overlay hides / points resize |
| Sample statistics | (read the status bar after loading) | *"agreement 0.9x · residual ~30 · bulk ~0 (km/s)"* — high agreement, residual ≈ the injected noise, near-zero bulk flow (radial infall is symmetric) |
| Bulk flow vs radius | click *Bulk flow vs radius…* | a plot with two near-identical curves (galaxies vs reconstruction) rising then flattening — the reconstruction tracks the galaxies because the demo galaxies follow the field |
| Remove overlay | click *Remove overlay* | galaxies/density/legend gone, controls reset |

## 7 · HI PPV cube  ·  `hi_cube.fits`

A rotating-disk HI cube (64³) with a velocity (VRAD) spectral axis + rest freq.

**Open:** *Data ▸ Open Remote Dataset…* → `hi_cube.fits` (opens the cube
viewer).

| Feature | Do this | Expect |
|---|---|---|
| Slice / volume | scrub the channel slider | disk emission sweeps velocity |
| Spectral profiler | Tools ▸ Extract Spectrum on a disk pixel | a Gaussian line |
| **Kinematic Model Overlay** | Tools ▸ Kinematic Model Overlay… → **x0=32, y0=32, PA=30, incl=60, Vsys=0, Vrot=150, rmax=26, σ=18** | the model shell/contour matches the data disk (params chosen to line up) |
| 2-D channel contours | keep the live panel's *Contour on 2D channel maps* on; scrub channels | model contour tracks the rotating disk on the slice |
| Export Movie | Tools ▸ Export Movie… → Channel scan | MP4 (or PNG sequence) of the channel sweep |
| Link Views | open the cube twice; Tools ▸ Link Views on both | camera / channel / colour sync |

The same cube also drives the rest of the cube-analysis tools — *Compute Moment*
(M0/M1/M2 show the disk / rotation gradient), *Extract PV Diagram* (a slit along
the major axis shows the rotation curve), *Line-Width Map*, *Estimate Noise*
(pick an off-disk channel region), *Baseline Subtraction*, and *Stack Spectral
Cubes* (open it twice). See the user-guide pages for each.

## 8 · 2-D image  ·  `image_2d.fits`

256² image with a celestial WCS, a beam (Jy/beam), and 8 Gaussian sources.

**Open:** *Data ▸ Open Remote Dataset…* → `image_2d.fits` (image viewer).

| Feature | Do this | Expect |
|---|---|---|
| Colour scale | LUT customizer → Linear/Log/Sqrt/Power(γ) | stretch changes |
| Profile / probe | Tools ▸ Profile across a source | a 1-D cut with the Gaussian peak |
| Region stats | draw a box/circle over a source | mean/rms/sum/flux (beam-aware) |
| Region IO | Export the region (CRTF/DS9), re-import | round-trips onto the image |
| Contours | enable contours | isophotes around the sources |

---

**Not aligned by accident:** the combo (§6) is the only pair guaranteed to share
a frame. Mixing an arbitrary field with an arbitrary catalogue (e.g. §5 + §2)
will *not* co-register — the overlay warns and the zoom-out makes the mismatch
obvious, which is itself the expected behaviour.
