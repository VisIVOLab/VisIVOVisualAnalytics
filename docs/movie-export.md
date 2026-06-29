# Movie Export (F3)

Export an animation of a spectral cube as a video, from the 3-D cube viewer:
**Tools ▸ Export Movie…** (`vtkWindowCube::showExportMovieDialog`,
`src/gui/vtkWindowCube_Movie.cpp`).

---

## Modes

- **Channel scan (2D channel maps)** — steps through the velocity channels and
  records each 2-D channel map. Any **kinematic-model contour** enabled on the
  2-D view is captured too, so you can export a model-vs-data movie. Choose the
  first/last channel (defaults to the full range).
- **Camera orbit (3D view)** — rotates the 3-D camera a full 360° turn around
  the volume over *N* frames. The camera is snapshotted and **restored** after
  the export (cancel or complete), so the interactive view is left unchanged.

Common option: **frames per second**.

---

## Output

- If **ffmpeg** is on `PATH` (`QStandardPaths::findExecutable`), frames are
  written to a temporary folder and assembled into an **MP4** (H.264,
  `-pix_fmt yuv420p`, `-crf 18`, even-dimension padding) at the path you choose.
- Otherwise the **PNG frame sequence** is written to a folder you pick
  (`frame_00000.png`, `frame_00001.png`, …) for assembly elsewhere.

Frame files use a **contiguous** counter (only successful captures advance it),
so a skipped/failed frame never leaves a gap that would break ffmpeg's
`frame_%05d.png` input.

---

## How frames are captured

Each frame is grabbed straight from the Qt GL widget with
`QOpenGLWidget::grabFramebuffer()` (the widget's own FBO), which is reliable
across Qt/VTK/platform combinations — `ui->vtkImage` for the 2-D scan,
`ui->vtkCube` for the orbit. The capture happens immediately after the frame is
rendered (`applyRemoteSliceResult()` for the scan, `renderWindow()->Render()`
for the orbit).

The capture loop runs on the GUI thread under a **window-modal, cancellable**
`QProgressDialog`; an `m_exportMovieInProgress` guard + disabled action prevent
re-entry, and the modal dialog blocks closing the viewer mid-export. Cancelling
stops immediately and produces **no** output (no partial movie).

---

## Limitations (phase-1)

- The **channel scan fetches each channel synchronously** from the backend
  (bounded by `BackendClient`'s request timeout). It's an export path, not the
  interactive viewer, so the UI can briefly block between frames; *Cancel* takes
  effect between frames. Async per-frame fetch is a later refinement.
- ffmpeg encoding is run **synchronously** (`waitForFinished`, 10-min cap);
  async encoding with its own progress/cancel is future work.
- The 2-D **resolution of the movie is the on-screen widget size** (frames are
  grabbed from the live widget), so resize the window for a larger movie.
