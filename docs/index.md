# VisIVO Visual Analytics

**An interactive desktop tool for exploring radio-astronomy data —
spectral cubes, 2-D maps, 3-D catalogues, and HiPS surveys — backed by a
scientific compute service.**

VisIVO Visual Analytics gives astronomers a fast workspace for inspecting
FITS data and deriving the products needed for science: moment maps,
position–velocity diagrams, spectral profiles, line-width and equivalent-width
maps, baseline-subtracted cubes, stacked spectra, isosurfaces, region
statistics, and source overlays. Visualisation runs on the desktop;
heavy computation runs on a co-located backend that can be embedded with the
app or deployed on an HPC cluster.

::::{grid} 1 2 2 3
:gutter: 3
:class-container: sd-mb-4

:::{grid-item-card} 🚀 Getting started
:link: user-guide/getting-started
:link-type: doc

Install, launch the backend, open your first FITS dataset.
:::

:::{grid-item-card} 🧊 Spectral cube viewer
:link: user-guide/cube-viewer
:link-type: doc

The 3-D + 2-D workspace: slice navigation, cutting plane, probe spectra,
camera ROI, animations.
:::

:::{grid-item-card} 🗺️ Moment maps
:link: user-guide/moment-maps
:link-type: doc

Orders 0–10 explained, channel range, RMS mask, scientific interpretation.
:::

:::{grid-item-card} 📈 Spectral analysis
:link: user-guide/spectral-tools
:link-type: doc

Line-width (FWHM + EW) maps, baseline subtraction, spectral stacking
across multiple cubes.
:::

:::{grid-item-card} ✏️ Regions, PV, noise
:link: user-guide/region-pv-noise
:link-type: doc

Box / circle / polygon / annulus statistics, position–velocity
extraction, MAD-based noise.
:::

:::{grid-item-card} 🌌 Catalogues, images, HiPS
:link: user-guide/catalogues-hips
:link-type: doc

3-D catalogue scatter, multi-layer 2-D images, all-sky HiPS surveys,
SAMP messaging with TOPCAT/Aladin.
:::
::::

---

```{toctree}
:caption: User Guide
:hidden:
:maxdepth: 2

user-guide/getting-started
user-guide/cube-viewer
user-guide/image-viewer
user-guide/moment-maps
user-guide/spectral-tools
user-guide/region-pv-noise
user-guide/catalogues-hips
user-guide/glossary
user-guide/troubleshooting
```

```{toctree}
:caption: Developer reference
:hidden:
:maxdepth: 2

architecture-note
backend-api
async-patterns
service-mapping-note
catalogue3d-viewer
testing
```

---

## What you can do with it

```{list-table}
:header-rows: 1
:widths: 28 36 36

* - Task
  - Where in the UI
  - Output
* - Open a remote FITS cube / image
  - **Data Hub → Open Remote Dataset** (or ⌘O)
  - Cube viewer or Image viewer
* - Navigate the spectral axis
  - Slice slider, ▶ Play animation
  - Slice tile + textured 3-D cutting plane
* - Compute a moment map
  - **Tools → Compute Moment** in the cube window
  - 2-D map (M0 / M1 / M2 / …)
* - Extract a spectrum at a pixel
  - Probe mode (2-D view), or *Pick Spectrum on Plane Click* (3-D)
  - Spectral profile window
* - Position-velocity diagram
  - **Tools → Extract PV** + polyline on the slice
  - 2-D PV map
* - Per-pixel line-width map
  - **Tools → Line-Width Map…**
  - FWHM + EW maps
* - Subtract a polynomial baseline
  - **Tools → Baseline Subtraction…**
  - New cube dataset (registered in the session)
* - Stack multiple cubes
  - **Tools → Stack Spectral Cubes…**
  - 1-D stacked spectrum
* - Catalogue overlay on the slice
  - **Tools → Load Catalogue Overlay** (CSV / VOTable)
  - Sources rendered + table dock
* - SAMP send/receive with TOPCAT / Aladin
  - Built-in SAMP hub bridge
  - File / catalogue exchange
```

---

## Quick install

For full instructions see [Getting started](user-guide/getting-started).

```bash
# 1. backend (FastAPI)
cd backend
python -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
uvicorn app.main:app --host 127.0.0.1 --port 8001
# A bearer token is printed at startup (and saved to ~/.visivo_token)

# 2. desktop client (Qt 6.5+ / VTK 9.5+ / C++17)
cmake -B build -S .
cmake --build build
./build/VisIVOVisualAnalytics.app/Contents/MacOS/VisIVOVisualAnalytics
```

---

## Status

| | |
|---|---|
| Active branch | `next-server` |
| Backend | FastAPI + Python 3.12+ |
| Client | Qt 6.5+ / VTK 9.5+ / C++17 |
| License | See repository `LICENSE` |
| Repo | <https://github.com/VisIVOLab/ViaLacteaVisualAnalytics> |
