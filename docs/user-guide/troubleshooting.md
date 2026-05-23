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

You've requested an action (e.g. *Stack Spectral Cubes*) referencing a
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

## Tools

### "Pick Spectrum on Plane Click" does nothing when I click

The pick uses a **double-click** on the textured cutting plane (not a
single click, which is reserved for camera rotation).

If even the double-click does nothing:

- Make sure the toggle is on (cursor should be a cross-hair over the
  3-D view).
- The cutting plane must be visible. Lower its opacity to make sure you
  see it.
- Click *on the plane itself*, not on the volume rendering behind it.

### "Stack Spectral Cubes" shows only one cube

You only have one cube open in the current backend session. Open the
other cubes first (via *Data Hub → Open Remote Dataset*). The dialog
enumerates all cubes whose shape matches the reference cube.

If you have multiple cubes open but only one appears in the list:

- Their shapes are different. The dialog disables incompatible cubes
  (different `width × height × depth`); hover the disabled rows to read
  the tooltip with the actual shape.

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
