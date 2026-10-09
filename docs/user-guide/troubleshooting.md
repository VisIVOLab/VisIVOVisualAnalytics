# Troubleshooting

Common problems, what they mean, and how to fix them. Open a
[GitHub issue](https://github.com/VisIVOLab/ViaLacteaVisualAnalytics/issues)
if you hit something not covered here.

## Backend / connection

### "Could not load preview." in the cube viewer

The backend returned an error response for `/v1/cube/preview`. Open the
**View → Diagnostics** panel — the actual error message (and a Python
traceback if available) is in the *Backend* category. Most common
causes:

- The FITS file path the backend tried to open doesn't exist or is
  unreadable from the backend host (different mount points, network
  share not mounted, etc.).
- The cube has a non-standard scalar type (e.g. complex). VisIVO supports
  float32 cubes only.
- A `BrokenPipeError` / `[Errno 32]` typically means a backend worker
  process crashed mid-response. Restart the backend.

### "Session not found."

You've requested an action (e.g. *Stack Cubes to a Spectrum*) referencing a
`session_id` that the backend no longer has — usually because the
backend was restarted after the cube was opened.

**Fix**: close the cube viewer, reopen the cube from the Data Hub, retry
the action.

### Backend health is red in the Startup Dialog

The desktop client cannot reach `http://127.0.0.1:8000/v1/health`
(or the URL set in *Settings*). Check:

- Is `uvicorn` running? Look at the terminal where you launched it.
- Is the port correct? Default is `8000`.
- Firewall? On macOS, the first run may pop up a "Allow incoming
  connections?" dialog. Allow it.
- Token mismatch? If you previously set `VISIVO_TOKEN` in your shell and
  the backend regenerated one, the client uses `~/.visivo_token`. Either
  remove the env var or copy the new token into the env.

### Auto-launched backend keeps crashing

The client auto-spawns a backend if none is reachable. Errors during
auto-launch are captured in *View → Diagnostics → Backend* (the same
panel as runtime errors).

Most common cause: missing Python deps. Open a terminal and run:

```bash
cd backend
pip install -r requirements.txt
```

Then restart the app.

## Performance

### The cube swap from preview to full-resolution takes several seconds

Expected for cubes ≥ ~500 MB. The first full-resolution Render uploads
the entire volume texture to the GPU — that's the unavoidable cost.
Mitigations already built in:

- The **slice view** stays interactive throughout.
- Per-pixel work (NaN sanitisation, blank-fraction stats) runs on the
  worker thread; the UI thread does only the data swap.
- A worker-pool slot is always reserved for interactive requests via the
  [heavy-task throttle](../async-patterns#heavy-task-throttle-backend-side)
  — slice scroll, probe, ROI still respond immediately.

If the wait is still painful: enable *Use Camera ROI* in the View menu
to fetch only the sub-volume visible in the 3-D camera viewport.

### Line-width map takes a long time

Yes — it's a per-pixel Gauss fit; ~10 s on a 512×512 cube with default
settings is normal. Mitigations:

- Already runs in parallel across `VISIVO_WORKERS` worker chunks.
- Already skips low-SNR background pixels.
- Cached on the backend; re-clicking *Compute* with the same parameters
  returns instantly.

To go faster you can raise `VISIVO_LINEWIDTH_SNR` (skips more pixels) or
restrict the channel range to just the line.

### Slice scrolling stutters during a heavy compute

With the default settings this shouldn't happen — the backend reserves
one worker for interactive requests so a moment / line-width / stack
compute can't starve them. If it does:

- Check `VISIVO_HEAVY_SLOTS` and `VISIVO_WORKERS` envvars. Defaults are
  `WORKERS=4`, `HEAVY_SLOTS=3` (one always free).
- The Python worker pool grows lazily; if your `WORKERS=1` you only have
  one slot and heavy tasks will block.

### Volume rendering is choppy

The volume mapper is GPU-bound. Causes:

- Cube is too big for your GPU memory; pre-existing rendering is fine,
  but interaction tanks. Try *Use Camera ROI* to fetch only what's
  visible.
- Some integrated GPUs cap 3-D texture size; reduce the cube via ROI or
  open a downsampled FITS.

## WCS

### "WCS repaired" / "WCS degraded" in the status bar

The cube's WCS metadata was incomplete or non-conformant and the backend
fixed it with safe defaults. Click the badge for the full list of
changes — typical cases include missing `SPECSYS`, ambiguous CTYPE
versions, `BLANK` keyword on float data.

You can still use the cube normally; the metadata fixes are stored
server-side for the session.

### "Sanity: Warning" in the status bar

The sanity-check ran across the cube and found either:

- Inconsistent WCS for one or more axes.
- Heavy NaN / blanked fraction in the loaded sub-volume.
- Missing celestial axis pairing.

Click the badge to see the details. Most warnings are informational;
fix the source file (e.g. via `astropy.io.fits`) if you want them gone.

### Sky coordinates show as "RA: 0:00:00.0 Dec: 0:00:00.0" everywhere

The cube has no celestial axes (only the spectral axis is recognised),
or the `CRVAL` / `CRPIX` are missing. Check the FITS header
(*View → Show FITS Header* in the cube viewer).

If the keywords are simply missing or wrong — a `CTYPE1` that never got
written, a `CDELT` saved as a quoted string — **Edit…** in that header
window writes a corrected **copy** into the Workspace and tells you whether
the result has a celestial WCS. The file on disk is not modified.

### A layer will not go onto my image: "covers none of the sky…"

The FITS you are adding is of a different part of the sky. The viewer refuses
it because layering it would place it correctly — off the edge of the image —
and show nothing at all. Use **Open in New Window** for it.

If you believe the two really do overlap, **Add Anyway** is there: the check
uses `wcsrange()` bounds, which are a bounding box, and a rotated field
straddling RA = 0 can measure narrower than it is.

## Tools

### "Pick Spectrum on Plane Click" does nothing when I click

The pick uses a **double-click** on the textured cutting plane (not a
single click, which is reserved for camera rotation).

If even the double-click does nothing:

- Make sure the toggle is on (cursor should be a cross-hair over the
  3-D view, and its pane should carry the amber armed border). Selecting
  another pane does **not** disarm it any more, so the border is the thing to
  check.
- The cutting plane must be visible. Lower its opacity to make sure you
  see it.
- Click *on the plane itself*, not on the volume rendering behind it.

### "Switch to Remote" is missing, or *View → Remote* is greyed out

The backend told us it cannot render server-side. It reports that in the
dataset-open response (`server_side_rendering_available`), and it is false for a
CPU-only backend and for **any backend running on macOS** — server-side
rendering needs an EGL context, which macOS does not provide. The tooltip on the
disabled *Remote* mode says so.

This is not a limitation of your cube: rendering simply happens in this process.
For a cube above the local-load threshold, use **Load full resolution** (in the
banner, or *View → Load Full Resolution*) and watch the memory — the whole volume
becomes resident, in RAM and as a 3-D GPU texture.

### A tool in the Analysis list is greyed out

Hover it: every disabled tool in this app states its own reason, and the
tooltip is the answer.

- *"acts on the 2D Slice — show it in a pane"* / *"acts on the 3D view — …"*:
  the tool is bound to a view, and that view is not currently in any pane. Pick
  it from a pane's ▾ menu (a tool does **not** need its pane to be the selected
  one — see [Cube viewer](cube-viewer#when-a-tool-is-available-and-where-it-acts)).
- *"probe a pixel first"* (Pin Spectrum), *"draw or import a region first"*
  (Export Region), *"open a 2D image viewer first"* (Overlay Slice on an Open
  Image), *"nothing to link yet"* (Link Views): the tool works on something that
  does not exist yet. **Hover the greyed entry to read the reason** — it is on
  the row in Inspector ▸ Analysis as well as in the Tools menu.
- *Export Moment Map as FITS* stays disabled until a moment map has actually
  been computed; *Open in VR* until a VR runtime is available.

### I armed a tool and cannot switch it off

<kbd>Esc</kbd> disarms whatever is armed. The pane an armed tool listens to
wears a **thicker amber border** — that border, not the blue selection border,
is what says "this tool is live here". Clicking the tool's own entry a second
time also disarms it.

### My spectrum disappeared when I closed Extract Spectrum

Only a **live** curve — one still following the cursor — goes with the tool. If
you had **clicked** a pixel (header: *🔒 pinned*), ending the tool keeps that
spectrum as a SPEC product in Session Data. Once it is saved the header says
*💾 saved as "Spectrum N"*, and ending the tool then removes only the live row,
because the curve is already safe. To keep one and carry on exploring, use
*Tools ▸ Pin Spectrum* at any moment.

Note that *Compare* in the Inspector's SPECTRUM panel does **not** save
anything: it lays the curve on the plot for comparison. That distinction used to
be invisible — both buttons were called *Pin Spectrum*.

### "Load Lines…" drew nothing (or nothing where I expected)

The tool tells you which of the two happened.

- *"none falls inside the plotted range"*: the numbers converted fine but land
  outside the band. Almost always the **unit** or the **systemic velocity / z**
  — a rest-frame list drawn unshifted sits outside the observed band by
  construction.
- *"this cube's header has no RESTFRQ"*: a frequency list cannot be converted to
  a velocity axis without the cube's rest frequency. Load a list in the axis's
  own unit instead.
- The markers vanished after you changed what the pane shows: they are stored as
  coordinates on the axis they were computed for, so they are dropped when the
  pane moves to a product on a **different** spectral axis. Load them again.

See [Line identification](spectral-tools#line-identification-overlaying-a-line-list).

### I closed a tool window while it was computing

The computation **keeps running on the backend** — it cannot be cancelled (these
requests have no cancel route, and the backend cannot kill a running compute
child) — and its result is discarded when the window that asked for it is gone.
So the dialogs ask first: *Close and Discard* / *Keep Waiting*.

If you close anyway, watch the status bar: "N running" counts it until it
finishes, and Diagnostics has the line where it started. Until it does, it holds
one of the session's compute slots, so another heavy tool may have to wait.

### "Too many concurrent compute tasks" / the backend is busy

The backend admits a limited number of concurrent computations **per session**
(`VISIVO_MAX_CONCURRENT_TASKS`, default 3) and answers HTTP 429 beyond that.
This is deliberate backpressure, not an error — it keeps a long compute from
starving slice scrolling — but you should rarely see it:

- the server now **waits** up to `VISIVO_TASK_SLOT_WAIT_S` (default 15 s) for a
  slot before refusing, and
- the client **retries** a refused heavy request (isosurface, moment, noise,
  line-width, stacking) with backoff before reporting it.

If you still get it, something long is genuinely running. It is most likely on a
large cube, where opening the dataset itself launches heavy work — the
full-resolution load and the whole-cube statistics — and a heavy request that is
merely *queued* for a worker also holds a session slot. Check the
*Diagnostics* panel (the status bar shows `N running · M queued`), let the
running computation finish, and retry. Raising
`VISIVO_MAX_CONCURRENT_TASKS` / `VISIVO_HEAVY_SLOTS` on the server trades
interactive responsiveness for throughput.

### "Stack Cubes to a Spectrum" shows only one cube

You only have one cube open in the current backend session. Open the
other cubes first (via *Data Hub → Open…*). The dialog
enumerates all cubes whose shape matches the reference cube.

If you have multiple cubes open but only one appears in the list:

- Their shapes are different. The dialog disables incompatible cubes
  (different `width × height × depth`); hover the disabled rows to read
  the tooltip with the actual shape.
- They are the same file. The list shows one row per *file*, and a cube can be
  registered in a session twice (a baseline run registers its output, and
  opening that output from its Session Data row registers it again), so the
  duplicate is collapsed rather than offered as a second cube to stack.

### Moment dialog says "Computing…" forever

Check the *Diagnostics* panel — a backend exception probably prevented
the result from arriving. The moment compute is async (you can keep
using the app), so a frozen "Computing…" state is always a backend
error. Common causes:

- Cube has no spectral axis.
- Channel range is empty after clamping.
- All voxels in the range are NaN/blanked.

## Sessions & multi-user

### "Created anonymous session" warning

The desktop client did not send `X-Visivo-Session`. Each request gets
its own anonymous session, so cross-dataset features (Stacking,
"all cubes in session") won't work as expected.

This shouldn't happen in normal use; if it does, file an issue with the
Diagnostics log attached.

## Reporting an issue

When opening a ticket please include:

1. **Diagnostics log** — *View → Diagnostics → Copy All*.
2. **Backend terminal output** — full traceback if any.
3. **OS / Qt / VTK versions** — `cmake --version`, `qmake --version`,
   `vtk-config --version` (or the equivalent on Linux).
4. **A minimal FITS** that reproduces the problem when possible.
