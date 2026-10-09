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
| **Python** | **3.11 or newer** for the backend. The client checks this and says so if the interpreter it finds is older — note that macOS ships 3.9 and several Linux distributions ship 3.9 or 3.10, so a system `python3` is often not enough. |
| **GPU** | Any Apple-Silicon GPU or a modern desktop GPU. Volume rendering uses `vtkGPUVolumeRayCastMapper`. |

For full build flags and dependency notes see the repository
[`BUILDING.md`](https://github.com/VisIVOLab/ViaLacteaVisualAnalytics/blob/master/BUILDING.md).

## Starting the backend

The backend is a small FastAPI service that does the data-intensive work
(FITS I/O, moment maps, isosurface, spectral analysis, HiPS tiles, catalogue
queries, …). The desktop client always talks to a backend instance —
either one it starts itself (the default), or one you started by hand
on the same machine or on a remote node.

### Letting the client do it

If you have Python 3.11 or newer, you do not have to do anything: start the
client and it will find the backend that ships with it, create a virtual
environment on first run, install the dependencies, and launch the server. The
startup window reports progress.

That first run downloads several hundred megabytes and takes a few minutes.
Afterwards it is instant, because the environment is reused.

```{note}
The client needs to *find* a suitable interpreter. It looks, in order, at
`VISIVO_BACKEND_PYTHON`, an active virtual environment, the project's own
`.venv`, a Python shipped next to the application, and finally your `PATH`. If
your 3.11+ is installed somewhere unusual, point at it explicitly:

    export VISIVO_BACKEND_PYTHON=/opt/python3.12/bin/python3
```

### Doing it by hand

Useful when the backend runs on another machine, or when you want to watch its
log:

```bash
cd backend
python3.11 -m venv .venv && source .venv/bin/activate
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
A backend you started by hand always wins: the client probes the configured URL
first and connects to whatever is already listening, rather than starting a
second one (see `BackendLauncher` and the *Settings* dialog).
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

- **Open…** (⌘O, also *File ▸ Open…*) opens a remote file browser rooted at the
  backend's working directory. Pick any supported file: the backend identifies
  it from its contents and the matching viewer opens.
  - 2-D FITS image → [Image viewer](image-viewer)
  - 3-D spectral cube → [Spectral cube viewer](cube-viewer)
  - FITS with time and frequency axes, or HDF5 → the dynamic-spectrum viewer
  - CSV / VOTable / IPAC / speck → [3-D catalogue viewer](catalogues-hips)
  - VisIVO Binary Table (`.bin` + `.head`) → [VBT viewer](vbt-viewer)

  You are asked only when the file itself is ambiguous. A FITS **table** is the
  usual case: it can be a source catalogue or a table to overlay on an image,
  and nothing in the file says which — so the client asks instead of guessing.

Under **Archives**:

- **HiPS Viewer** opens the all-sky [HiPS viewer](catalogues-hips).
- **VLKB Inventory** goes to the [VLKB archive](vlkb-archive) tab.

Beside it sits the **VLKB** tab, which queries the ViaLactea Knowledge Base over
a region of the Galactic plane and cuts out the images and cubes that cover it —
see [VLKB archive](vlkb-archive).

You can also drag-and-drop a local FITS file onto the Data Hub — it will
be staged on the backend automatically.

### From the desktop: double-click a file

VisIVO registers itself as a FITS viewer, so a `.fits` (`.fit`, `.fts`, `.fz`)
file can be opened from the file manager or the command line — and so can
anything else the application opens: a catalogue (CSV, VOTable, IPAC `.tbl`,
`.speck`) or a VisIVO Binary Table.

```bash
VisIVOVisualAnalytics /data/WALLABY.fits      # also works with several paths
VisIVOVisualAnalytics /data/snapshot.tbl      # catalogue → 3-D catalogue viewer
VisIVOVisualAnalytics /data/volume.bin        # VBT → point or volume viewer
open -a "VisIVO Visual Analytics" cube.fits    # macOS
```

Whatever the file is, it goes through the same single decision as *File ▸
Open…*: the backend classifies it and the right viewer opens. When the content
is genuinely ambiguous — a FITS table can be either — you are asked which viewer
to use rather than guessed at.

If VisIVO is not running the startup sequence happens first — backend, then
authentication — and the file opens as soon as the main window appears. If it is
already running the file opens **in that same instance**, in a new viewer
window: on macOS the double-click reaches the running app directly, and on
Linux / Windows (where the desktop starts a new process for every double-click)
the new process hands the file over and exits. A launch with no file always
starts its own instance, so running two VisIVOs against two backends is still
possible. Several files opened at once are opened one after another, never on
top of each other's dialogs.

```{note}
**macOS**: the bundle claims FITS with handler rank *Alternate*, on purpose.
It appears in *Open With* and macOS will pick it when nothing else claims the
type, but it does not take the association away from DS9 / CARTA / QFitsView on
a machine where one of those is installed. To make it the default: select a FITS
file, ⌘I, *Open With* ▸ *VisIVO Visual Analytics* ▸ *Change All…*. After
installing a new build, launch it once so LaunchServices re-reads the bundle
(or run `lsregister -f /Applications/VisIVOVisualAnalytics.app`).

**Linux**: install the association from `deploy/linux/`:

```bash
xdg-mime install --novendor visivo-fits-mime.xml
xdg-desktop-menu install --novendor visivo-visual-analytics.desktop
update-mime-database ~/.local/share/mime
```

The `.desktop` file expects `VisIVOVisualAnalytics` on `PATH`; edit its `Exec=`
line if the binary lives elsewhere.
```

```{caution}
The file is opened **by path, through the backend** — the same route as *Open
Remote Dataset*, and the classification happens there too, so with the backend
down you are told that rather than asked to choose a viewer. A double-clicked
file therefore has to be readable by the backend process: with a local auto-started backend that is anything you can
read, but with a remote backend the path must exist *there*. Use the Data Hub's
drag-and-drop instead, which stages the file server-side.
```

## The ⌘K Command Palette

Almost every action in the app is searchable from a single launcher.
Press <kbd>⌘K</kbd> (or <kbd>Ctrl</kbd>+<kbd>K</kbd> on Linux) anywhere
in the application and start typing — *"open"*, *"diagnostics"*,
*"settings"*, *"hips"* — to jump to actions and recent datasets without
hunting through menus. It works from inside the viewer windows too.

Searchable groups:

- **File** — Open…, Open 3D Catalogue, Open VBT, HiPS Viewer, VLKB
  Inventory. (**Open…** takes any supported file; the two named entries are
  there for when you would rather say which opener to use.)
- **Recent datasets** — last six datasets opened in this session
  (refreshed each time the palette is opened).
- **Application** — Open Settings (⌘,), About, Quit VisIVO Visual
  Analytics (⌘Q).
- **View** — Open Diagnostics Window.
- **Science** — Line-Width Map, Baseline Subtraction, Spectral
  Stacking, Source Finding (SoFiA-2). These mirror the **Science**
  menu and respect its enabled state (open a dataset first — running
  one with no cube loaded silently no-ops, matching the menu).
- **SAMP** — Connect to / Disconnect from Hub, Send Current FITS
  Dataset, Send Current Catalogue. Mirrors the **SAMP** menu.

Try search terms like *"stack"*, *"samp"*, *"quit"* or *"sofia"* to
jump straight to the action.

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
