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

```{toctree}
:caption: VisIVO Server (CLI tools)
:hidden:
:maxdepth: 2

server/visivo_server
server/visivo_importer
server/visivo_filter
server/visivo_viewer
server/visivo_utils
```

---

## What you can do with it

Tools are grouped consistently between the *Tools* menu and the *Tools*
sidebar tab in the cube viewer — same four groups (EXPLORATION,
ANALYSIS, REGIONS, CATALOGUE) in the same order. The table below lists
every tool together with the page that documents its scientific use.

```{list-table}
:header-rows: 1
:widths: 24 28 30 18

* - Task
  - Where in the UI
  - Output
  - Documented in
* - **General**
  -
  -
  -
* - Open a remote FITS cube / image
  - **Data Hub → Open Remote Dataset** (or ⌘O)
  - Cube viewer or Image viewer
  - [Getting started](user-guide/getting-started)
* - Navigate the spectral axis
  - Slice slider, ▶ Play animation
  - Slice tile + textured 3-D cutting plane
  - [Cube viewer](user-guide/cube-viewer)
* - **EXPLORATION tools** (cube viewer → Tools menu / sidebar)
  -
  -
  -
* - Extract a spectrum at a pixel
  - **Tools → Extract Spectrum** (Probe), or *Pick Spectrum on Plane Click* (3-D)
  - Spectral profile window
  - [Cube viewer · Extract Spectrum](user-guide/cube-viewer#extract-spectrum-probe-a-single-pixel)
* - Open the cube in a VR headset (Windows / Linux, opt-in build)
  - **Tools → Open in VR**
  - OpenXR stereo render — LUT / threshold / opacity TF synced live
  - [Cube viewer · Open in VR](user-guide/cube-viewer#open-in-vr)
* - **ANALYSIS tools**
  -
  -
  -
* - Estimate the cube noise (per-channel MAD)
  - **Tools → Estimate Noise** + pick a line-free region
  - Per-channel σ vector + scalar median σ
  - [Regions, PV, noise · Noise estimation](user-guide/region-pv-noise#noise-estimation)
* - Compute a moment map (M0–M10)
  - **Tools → Compute Moment**
  - 2-D map (M0 / M1 / M2 / …)
  - [Moment maps](user-guide/moment-maps)
* - Per-pixel line-width map
  - **Tools → Line-Width Map…**
  - FWHM + EW maps
  - [Spectral tools · Line-width](user-guide/spectral-tools#line-width-maps-s-02)
* - Subtract a polynomial baseline
  - **Tools → Baseline Subtraction…**
  - New cube dataset (registered in the session)
  - [Spectral tools · Baseline](user-guide/spectral-tools#baseline-subtraction-s-03)
* - Stack multiple cubes
  - **Tools → Stack Spectral Cubes…**
  - 1-D stacked spectrum
  - [Spectral tools · Stacking](user-guide/spectral-tools#spectral-stacking-s-04)
* - Position-velocity diagram
  - **Tools → Extract PV Diagram** + polyline on the slice
  - 2-D PV map
  - [Regions, PV, noise · PV](user-guide/region-pv-noise#position-velocity-diagrams)
* - **REGIONS tools** (draw on the 2-D slice / moment)
  -
  -
  -
* - Box / Circle / Polygon / Annulus statistics
  - **Tools → Box / Circle / Polygon / Annulus Region Analysis**
  - Per-region mean / median / σ / sum + spectral profile
  - [Regions, PV, noise · Region statistics](user-guide/region-pv-noise#region-statistics)
* - **CATALOGUE overlay** (menu-only state toggles)
  -
  -
  -
* - Load a source catalogue overlay
  - **Tools → Load Catalogue Overlay** (CSV / VOTable)
  - Sources rendered + table dock
  - [Catalogues · Overlay](user-guide/catalogues-hips#catalogue-overlay-on-cubes--images)
* - Show / hide the overlay glyphs and labels
  - **Tools → Show Catalogue Overlay** / **Show Catalogue Labels**
  - Glyph or label visibility toggled (state preserved)
  - [Catalogues · Overlay](user-guide/catalogues-hips#catalogue-overlay-on-cubes--images)
* - Drop the overlay
  - **Tools → Clear Catalogue Overlay**
  - Glyphs + table dock removed
  - [Catalogues · Overlay](user-guide/catalogues-hips#catalogue-overlay-on-cubes--images)
* - **Interop**
  -
  -
  -
* - SAMP send / receive with TOPCAT / Aladin
  - Built-in SAMP hub bridge (auto-discovers running hub)
  - File / catalogue exchange
  - [Catalogues · SAMP](user-guide/catalogues-hips#samp)
* - Open VBT / volume binary tables produced by the VisIVO Server CLI
  - **File → Open** (or *Data Hub*) — any `.bin` / `.head` VBT pair
  - Particle viewer (`vtkWindowVbt` / `vtkWindowCatalogue3D`)
  - [VisIVO Server](server/visivo_server)
```

---

## Quick install

For full instructions see [Getting started](user-guide/getting-started).

```bash
# 1. backend (FastAPI)
cd backend
python -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
uvicorn app.main:app --host 127.0.0.1 --port 8000
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
