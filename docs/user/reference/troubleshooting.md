# Troubleshooting

Common problems, what they mean and what to do. For most errors the full
message from the backend is in *View ▸ Diagnostics*. If a problem is not listed
here, open a
[GitHub issue](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues)
(see [Reporting an issue](#reporting-an-issue)).

## Backend and connection

### The startup dialog cannot reach the backend

The client gets no answer from `http://127.0.0.1:8000` (or the URL set in
*Settings*). Press *Show Logs* to read the launcher's output, then check:

- that the backend is running, if you started it by hand, and on the expected
  port;
- on macOS, that you allowed incoming connections when the system asked;
- the token: a `VISIVO_TOKEN` left in your shell from an earlier backend
  overrides `~/.visivo_token`. Remove it or update it.

### The backend started by the client keeps crashing

Its errors are in *View ▸ Diagnostics*, category *Backend*. The usual cause,
when running from source, is a missing Python package: run
`pip install -r requirements.txt` in `backend/` and restart the application.

### "Session not found."

The backend was restarted after the dataset was opened. Close the viewer and
open the file again.

### "Could not load preview." in the cube viewer

The backend could not read the cube. Diagnostics has the reason. Common causes:
the path does not exist on the backend's machine (a share not mounted there),
or the data type is not a standard FITS one (VisIVO Visual Analytics reads 8-,
16- and 32-bit integers, with `BSCALE`/`BZERO`, and 32- and 64-bit floats).
`[Errno 32]` or `BrokenPipeError` means a backend worker crashed: restart the
backend.

### The remote 3-D view is choppy, frozen or shows only the proxy

- The view is coarser while it moves and sharpens when you release the mouse.
- A view that stops updating usually means the SSH tunnel dropped; the cluster
  helper reopens it within a few seconds.
- A decimated proxy means the backend cannot render on its side (no EGL in its
  image, or no GPU for its job). See
  [Running the backend on a cluster](../../technical/deployment/hpc-slurm.md#troubleshooting).

### "Switch to Remote" is missing, or *View ▸ Remote* is greyed out

The backend cannot render on its side: it has no GPU rendering, or it runs on
macOS, which has no EGL. Rendering then happens on your machine. For a large
cube use *View ▸ Load Full Resolution* and watch the memory, or crop the region
you need.

## Opening files

### "The file … is truncated"

The file is shorter than its header declares (an interrupted copy or download,
or a full disk). The message gives the size on disk, the declared size and the
planes present. Copy or download it again. A file fetched with *Open URL* is
removed from the cache when refused, so opening the URL again downloads it
again.

### A layer will not go onto my image: "covers none of the sky…"

The new FITS is of another part of the sky, so as a layer it would fall off the
image. Use *Open in New Window*. If the two do overlap (a rotated field across
RA = 0 can be measured too narrow), press *Add Anyway*.

## Memory and speed

### Full resolution is refused: not enough memory

Loading a cube at full resolution needs about three times its size while it
arrives. When that exceeds the machine's memory, the client keeps the
decimated preview and gives the numbers. Use *View ▸ Crop Full Resolution…* for
the region you need, or render on a remote backend.

A cube opens as a preview when it is at least 4 GiB, or a quarter of the memory
the backend may use if that is smaller (its container or SLURM limit). The
backend operator can change this with `VISIVO_FULL_RES_THRESHOLD_BYTES`
([Configuration](../../technical/configuration.md#memory-and-full-resolution-loading)).

### The switch from preview to full resolution takes several seconds

Expected above about 500 MB: the whole volume is uploaded to the GPU. The slice
view stays usable meanwhile. *View ▸ Use Camera ROI* fetches only the part
visible in the 3-D view.

### Volume rendering is choppy

The cube is too large for the GPU memory, or an integrated GPU limits the 3-D
texture size. Use *Use Camera ROI*, crop the cube, or open a downsampled file.

### The line-width map takes long

The Gaussian fit runs per pixel; about 10 s on a 512 × 512 cube is normal.
Repeating it with the same parameters is immediate. To go faster, choose a
method without a fit (*FWHM, moment-2 dispersion* or *Equivalent width*), or
restrict the channel range to the line.

### A moment map is still "Computing the moment map"

On a cube of hundreds of GB a moment 1 or 2 can take 10 to 15 minutes. The
progress window says whether the task is queued or running; *Cancel* stops it.
If it fails, Diagnostics has the reason: no spectral axis, an empty channel
range, or only blank voxels in the range.

### "Too many concurrent compute tasks"

Each session runs at most three heavy computations at once (default); the
client already retries before showing this. Something long is running,
often the full-resolution load and the statistics of a large cube just opened.
The status bar shows *N running · M queued*. Wait for it to finish and retry.

## WCS

### "WCS repaired" or "WCS degraded" in the status bar

The header was incomplete or non-standard and the backend filled it in with
safe defaults (a missing `SPECSYS`, `BLANK` on float data). Click the badge for
the list of changes. The cube can be used normally.

### "Sanity: Warning" in the status bar

The sanity check found an inconsistent axis, a large blank fraction, or a
celestial axis without its pair. Click the badge for details. Most warnings are
informational.

### Sky coordinates read 0:00:00.0 everywhere

The cube has no celestial axes, or `CRVAL`/`CRPIX` are missing. Open the header
from the *CUBE · W×H×D* tag in the toolbar. *Edit…* there writes a corrected
copy into the Workspace (the original file is not changed) and says whether the
copy has a celestial WCS.

## Tools

### A tool is greyed out

Hover it: the tooltip gives the reason, in the Tools menu and in
*Inspector ▸ Analysis*. Typical reasons: the view the tool acts on is in no
pane (pick it from a pane's ▾ menu), or the tool needs something first (a
probed pixel for *Pin Spectrum*, a region for *Export Region (CRTF/DS9)…*). See
[Cube viewer](cube-viewer.md).

### I armed a tool and cannot switch it off

Press Esc, or choose the tool's entry again. The pane the armed tool listens to
has a thick amber border.

### "Pick Spectrum on Plane Click" does nothing

Hold the mouse still: a press that moves is a rotation. Click on something that
can be picked (the cutting plane, the rendered volume or the isosurface), not
on empty space, the box or the axes. With 3-D rendering off and the plane
hidden there is nothing to pick.

### My spectrum disappeared when I closed Extract Spectrum

Only the live curve, the one following the cursor, goes with the tool. A
clicked pixel (*pinned*) is kept as a spectrum product in Session Data. Use
*Tools ▸ Pin Spectrum* to keep a curve. *Compare* in the SPECTRUM panel only
overlays a curve on the plot and saves nothing.

### "Load Lines…" drew nothing

- *"none falls inside the plotted range"*: usually the wrong unit, or a missing
  systemic velocity or redshift.
- *"this cube's header has no RESTFRQ"*: a frequency list cannot be placed on a
  velocity axis. Load a list in the axis's own unit.
- Markers are dropped when the pane moves to a product on another spectral
  axis. Load them again.

See [Spectral tools](spectral-tools.md).

### I closed a tool window while it was computing

The computation goes on in the backend and its result is discarded; the dialog
asks first (*Close and Discard* or *Keep Waiting*). It holds a compute slot
until it ends. Moment maps, the moment FITS export, PV diagrams and SoFiA-2 can
be cancelled with *Cancel*; quitting the application asks once and cancels
them.

### "Stack Cubes to a Spectrum" lists only one cube

Open the other cubes first. Cubes of a different shape are disabled (hover a row
for its shape), and the same file opened twice is listed once.

## Reporting an issue

Include:

1. The diagnostics log: *View ▸ Diagnostics*, select the rows (⌘A or Ctrl+A)
   and press *Copy Selected*.
2. The backend's output, with any traceback.
3. The operating system and the application version (*Help ▸ About*).
4. A small file that shows the problem, when possible.
