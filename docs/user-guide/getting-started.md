# Getting started

This page takes you from a fresh checkout to a FITS cube open on screen, in
about five minutes.

## What you need

| | |
|---|---|
| **macOS** | 14 or newer (primary target). Linux works too; the build files are CMake-portable. |
| **Compiler** | Apple Clang 15+, GCC 11+, or MSVC 2022. C++17. |
| **Qt** | 6.5 or newer (Core, Gui, Widgets, OpenGL, OpenGLWidgets, Concurrent, Network, NetworkAuth, Svg, PrintSupport). |
| **VTK** | 9.5 or newer, built with Qt 6 support. |
| **Python** | 3.12+ for the backend (FastAPI, NumPy, Astropy, SciPy). |
| **GPU** | Any Apple-Silicon GPU or a modern desktop GPU. Volume rendering uses `vtkGPUVolumeRayCastMapper`. |

For full build flags and dependency notes see the repository
[`BUILDING.md`](https://github.com/VisIVOLab/ViaLacteaVisualAnalytics/blob/master/BUILDING.md).

## Starting the backend

The backend is a small FastAPI service that does the data-intensive work
(FITS I/O, moment maps, isosurface, spectral analysis, HiPS tiles, catalogue
queries, …). The desktop client always talks to a backend instance —
either one it starts itself (the default), or one you started by hand
on the same machine or on a remote node.

```bash
cd backend
python -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
uvicorn app.main:app --host 127.0.0.1 --port 8000
```

On the first launch a token is printed:

```
[VisIVO] Backend token: qL3_mGlqZgCVw4bzeHC22wB5rVdvUW-Qym3cA3Fu4Mc
[VisIVO] Or set: export VISIVO_TOKEN=qL3_mGlqZgCVw4bzeHC22wB5rVdvUW-Qym3cA3Fu4Mc
[VisIVO] Token written to /Users/<you>/.visivo_token (mode 600).
```

You don't normally need to copy it: the desktop client reads
`~/.visivo_token` automatically. If you want to use the same token next time,
either keep the file or set the env var.

:::{tip}
You can skip the manual backend launch entirely. When the client starts and
no backend is reachable on the configured URL, it auto-spawns one for you
(see `BackendLauncher` and the *Settings* dialog).
:::

## Launching the client

```bash
cmake -B build -S .
cmake --build build
open build/VisIVOVisualAnalytics.app                  # macOS bundle
# or:
build/VisIVOVisualAnalytics                           # Linux binary
```

The first window you see is the **Startup Dialog**:

1. **Backend check** — the client pings the backend health endpoint and
   shows green when it is reachable.
2. **Authentication** — the static bearer token is loaded automatically if
   `~/.visivo_token` exists. You can paste a different one in *Settings* if
   you point the client at a remote backend.
3. **Optional VLKB sign-in** — if you plan to use VLKB-protected services
   (catalogue search, cutout staging) you can complete the OIDC sign-in here.
   Otherwise just continue.

Click **Continue** to enter the main window.

## Opening your first dataset

The **Data Hub** is the landing tab of the main window. Use it to browse
the backend-side filesystem:

- **Open Remote Dataset…** opens a remote file browser rooted at the
  backend's working directory. Select a `.fits` file. The client classifies
  it server-side and decides whether it's a 2-D image or 3-D spectral cube,
  then opens the appropriate viewer:
  - 2-D FITS image → [Image viewer](image-viewer)
  - 3-D spectral cube → [Spectral cube viewer](cube-viewer)
- **Open 3-D Catalogue…** (CSV / VOTable) launches the
  [3-D catalogue viewer](catalogues-hips).
- **HiPS Viewer…** opens the all-sky [HiPS viewer](catalogues-hips).
- **VLKB Inventory** browses VLKB-served sources behind the OIDC flow.

You can also drag-and-drop a local FITS file onto the Data Hub — it will
be staged on the backend automatically.

## The ⌘K Command Palette

Almost every action in the app is searchable from a single launcher.
Press <kbd>⌘K</kbd> (or <kbd>Ctrl</kbd>+<kbd>K</kbd> on Linux) anywhere
in the application and start typing — *"open"*, *"diagnostics"*,
*"settings"*, *"hips"* — to jump to actions and recent datasets without
hunting through menus. It works from inside the viewer windows too.

## Diagnostics

Every backend round-trip, OIDC step, moment compute, SAMP event, and WCS
sanitisation is logged into a structured event panel. Open it with
**View → Diagnostics** in any window (or via the Command Palette). One
diagnostics window per process — entries from the cube viewer, the
spectral tools, and the backend all stream into the same table.

## Where to go next

- Open a spectral cube and read the [Spectral cube viewer](cube-viewer)
  page to learn the workspace.
- Compute your first [moment map](moment-maps).
- Estimate the noise of a region with [Noise / PV / Regions](region-pv-noise).
- Look up unfamiliar terms in the [Glossary](glossary).
