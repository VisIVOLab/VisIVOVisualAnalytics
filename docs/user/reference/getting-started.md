# Getting started

How to install VisIVO Visual Analytics from a release package, start it, and
open a first file. To build it from source, follow
[`BUILDING.md`](https://github.com/VisIVOLab/VisIVOVisualAnalytics/blob/main/BUILDING.md)
in the repository.

For a walk-through on a real cube, see
[First steps](../use-cases/01-first-steps.md).

## Installing a release

The [GitHub releases](https://github.com/VisIVOLab/VisIVOVisualAnalytics/releases)
hold one package per platform. Each carries its own Python runtime with the
backend's dependencies, so nothing else needs to be installed.

| Platform | Package | To start it |
|---|---|---|
| macOS 14 or newer (Apple Silicon or Intel) | `VisIVOVisualAnalytics-macos-{arm64,x86_64}.dmg` | Drag the application to *Applications*. The first time, right-click it and choose *Open* (the builds are unsigned). |
| Linux x86-64 (Ubuntu 22.04 or newer) | `VisIVOVisualAnalytics-linux-x86_64.tar.gz` | Extract it and run `./VisIVOVisualAnalytics` in the extracted folder. Keep the folder together: the launcher looks for `./app` and `./python` beside it. |
| Windows 10/11 x64 | `VisIVOVisualAnalytics-windows-x86_64.zip` | Extract it and run `VisIVOVisualAnalytics.exe` in `bin\`. |

On Linux, use the tarball of 1.0.11 or later: the older AppImage could crash at
start. Volume rendering needs a GPU (any Apple Silicon GPU or a recent desktop
GPU).

*Help ▸ Check for Updates…* asks GitHub for a newer release. The client also
checks by itself at most once a day; *Check for new versions at startup* in
*Settings* turns that off.

## First launch

The first window is the startup dialog. It runs three steps and then opens the
main window:

1. *Backend*: the client checks for a running backend and starts its own if
   none answers, then shows *Connected* or *Started*.
2. *Authentication*: the backend token in `~/.visivo_token` is read
   automatically. If there is none, paste one: *Save & Continue* stores it,
   *Skip (no VLKB)* goes on without it.
3. *Loading application*: the main window opens on the *Data Hub* tab.

If the backend cannot be reached, *Retry* tries again and *Show Logs* shows the
launcher's output (see [Troubleshooting](troubleshooting.md)). The VLKB sign-in
is separate, in *Settings ▸ Data Access ▸ VLKB ▸ Sign in*, and only VLKB
services need it.

## The backend

The backend is the Python service that reads the files and runs the
computations. The client always works through one.

- Local backend: started by the client when nothing is listening on the
  configured address (`http://127.0.0.1:8000` by default). A backend you
  started yourself on that address is used instead of starting a second one.
- Token: a backend writes its token to `~/.visivo_token` (mode 600) at first
  start, and the client reads it from there. `VISIVO_TOKEN` in the environment
  overrides the file. This token is sent only to the local backend.
- Remote backend: a backend on another machine (a cluster node behind an SSH
  tunnel, an SRC node) is registered in *Settings ▸ Backends ▸ Remote
  Backends* with its URL and its own token. See
  [Backend on a cluster](../use-cases/09-backend-on-a-cluster.md) and, for the
  cluster side,
  [Running the backend on a cluster](../../technical/deployment/hpc-slurm.md).
- Files are always read by the backend: the file browser shows the backend's
  file system, and on a remote backend a path must exist on that machine.

To run the backend by hand (to watch its log, or on another machine), see
`BUILDING.md` and the [Configuration reference](../../technical/configuration.md).

## Opening a file

In the *Data Hub*:

- *Open…* (⌘O, also *File ▸ Open…*): opens a file browser on the backend's
  files. Its title names the backend (*Backend: Local*, or the cluster or
  remote backend). The backend identifies the file from its contents and the
  matching viewer opens:
  - 2-D FITS image: [Image viewer](image-viewer.md)
  - 3-D spectral cube: [Cube viewer](cube-viewer.md)
  - FITS with time and frequency axes, or HDF5: the dynamic-spectrum viewer
  - CSV, TSV, semicolon-delimited text, VOTable, IPAC `.tbl`, `.speck`: the
    3-D catalogue viewer ([Catalogues and HiPS](catalogues-hips.md))
  - VisIVO Binary Table (`.bin` + `.head`): points in the 3-D catalogue
    viewer, or one cube per field for a volume ([VBT viewer](vbt-viewer.md))
- *File type* (in the browser): filters the list.

  | File type | Names listed |
  |---|---|
  | All supported files (default) | every name below |
  | FITS images and cubes | `.fits .fit .fts`, their `.gz` forms, `.fz` |
  | Catalogues | `.csv .tsv .txt .vot .votable .xml .tbl .speck`, FITS tables |
  | VisIVO Binary Tables | `.bin.head`, `.head`, `.bin` |
  | HDF5 | `.h5 .hdf5 .hdf .bfdata` |
  | All files | everything, hidden files included |

  The filter goes by name, with one exception: a FITS file with no extension
  (the ATCA images `AGN_SHORES_I`, `_Q`, `_U` are delivered so) is recognised
  by its first header card and listed with the FITS groups too. A file with an
  unusual extension opens from *All files*, because the backend identifies it
  by content.
- *Preview* (in the browser): the start of the selected file, read by the
  backend: FITS HDUs and header cards, the first lines of a text table, a VBT
  `.head`, the groups of an HDF5 file.
- *Recent Datasets*: what you opened, newest first. Click to reopen, ✕ to
  remove the entry (the file is not touched). A file that has moved is reported
  as not found, with an offer to remove the entry.
- *Last 5 jobs*: the backend's latest work (opens, moments, PV diagrams,
  exports) with its duration. A failed open is listed too.
- *Archives*: *HiPS Viewer* opens the [HiPS viewer](catalogues-hips.md);
  *VLKB Inventory* the [VLKB archive](vlkb-archive.md) tab; *SKAVA Discovery*
  the [SKAVA Discovery](skava-discovery.md) tab.

Notes on what opens:

- A FITS table can be a catalogue or an overlay table, so you are asked which
  viewer to use. Nothing else asks.
- An image or cube in an extension HDU (ESO phase-3, APEX) opens from the
  first extension that holds data; the header viewer shows the primary cards
  beside it.
- Compressed FITS (`.fits.gz`, fpack `.fz`) open directly. The first open
  decompresses the file once into the backend's cache, behind a progress window
  with *Cancel*; later opens are as fast as a plain file. The backend needs the
  full uncompressed size on its disk. fpack's default float quantisation is
  lossy: values differ from the original by a fraction of the noise.
- While a file opens, the status bar shows *Opening name…* and then *Opened
  name.* or *Could not open name.*

### Opening from the desktop or the command line

VisIVO Visual Analytics registers itself for FITS files and also opens
catalogues and VBT files given on the command line:

```bash
VisIVOVisualAnalytics /data/WALLABY.fits      # several paths are accepted
open -a "VisIVO Visual Analytics" cube.fits    # macOS
```

If the application is already running on the same backend, the file opens in
that instance; otherwise a new instance starts. A launch with `--backend URL`
or `--connect-cluster NAME` goes to the instance on that backend. The file is
opened by path through the backend, so the backend must be able to read it.

- macOS: VisIVO Visual Analytics is offered in *Open With* without taking FITS
  away from DS9, CARTA or QFitsView. To make it the default, select a FITS
  file, press ⌘I, and choose *Open With ▸ VisIVO Visual Analytics ▸ Change
  All…*.
- Linux: install the association from `deploy/linux/`:

  ```bash
  xdg-mime install --novendor visivo-fits-mime.xml
  xdg-desktop-menu install --novendor visivo-visual-analytics.desktop
  update-mime-database ~/.local/share/mime
  ```

## Command palette

⌘K (Ctrl+K on Linux and Windows) opens a search over the application's actions
and the recent datasets. Type a few letters (*open*, *mosaic*, *samp*,
*settings*). Opened from a viewer, it also lists every entry of that viewer's
menus, so a cube tool such as SoFiA-2 is found the same way.

## Diagnostics

*View ▸ Diagnostics* opens one window per process with every backend request,
sign-in step, computation and WCS repair, from all windows. Select rows and
press *Copy Selected* to attach them to a bug report.

## Where to go next

- [Cube viewer](cube-viewer.md) and [Image viewer](image-viewer.md)
- [Moment maps](moment-maps.md)
- [Glossary](glossary.md)
