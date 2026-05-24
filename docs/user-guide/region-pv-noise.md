# Regions, PV diagrams, noise

Three commonly-used interactive analyses on a 2-D slice or moment map:

| Tool | What it gives you | Where in UI |
|------|-------------------|-------------|
| [Region statistics](#region-statistics) | mean / median / σ / sum over a shape | drag on the slice |
| [PV diagram](#position-velocity-diagrams) | intensity along a path × velocity | polyline + Compute |
| [Noise estimation](#noise-estimation) | per-channel MAD + 1σ | choose a line-free region |

---

## Region statistics

Region tools let you draw a shape on the 2-D slice / moment map and read off
pixel statistics — useful for source photometry, background measurement,
quick comparisons.

### Drawing a region

Pick a shape from *Tools → Regions* in the viewer:

| Shape | How to draw | Notes |
|-------|-------------|-------|
| **Box** | Click + drag the two opposite corners | Axis-aligned. Fast and predictable. |
| **Circle** | Click + drag from centre to edge | Diameter snaps to integer pixel. |
| **Polygon** | Click each vertex, double-click to close | Arbitrary shape. Concave polygons are supported. |
| **Annulus** | Click centre, drag outer radius, then inner | Useful for background subtraction around a source. |

While drawing, the live preview is rendered in red on the slice. Once
released, the region becomes selectable; right-click to remove it.

### Statistics shown

For each region the *Region Info* card shows:

```{list-table}
:header-rows: 1
:widths: 22 78

* - Field
  - Meaning
* - **Pixels (N)**
  - Number of valid (non-NaN) pixels in the region.
* - **Mean / Median**
  - Standard summary; median is robust to outliers.
* - **σ (MAD)**
  - Median absolute deviation × 1.4826 (robust 1σ estimate). Use this for
    background-noise estimates; less affected by faint sources than the
    sample standard deviation.
* - **Min / Max**
  - Extrema, useful for sanity checks against the colour-map range.
* - **Sum**
  - Pixel-value sum × pixel area (BUNIT-dependent). For moment-0 maps this
    is the integrated flux of the region.
```

If your image carries a BUNIT all the numbers are reported in the same
unit; otherwise they're in raw counts.

```{tip}
For **annulus** regions, the inner ring values represent the local
background and the outer-vs-inner difference is shown automatically —
useful for aperture photometry.
```

### Sending the result somewhere

- *Copy* the statistics block to the clipboard (CSV-ish format).
- *Export PNG* of the region overlay together with the underlying slice
  / moment.
- *Send via SAMP* to TOPCAT: the region polygon is broadcast as a VOTable.

---

## Position-velocity diagrams

A **position–velocity (PV) diagram** is a 2-D plot that shows intensity
along a chosen spatial path (one axis) versus velocity / frequency (the
spectral axis of the cube, the other axis). It is the standard way to
inspect kinematic structure: rotation curves, outflows, jets, expansion.

### Extracting a PV

1. In the cube viewer click **Tools → Extract PV Diagram**. The cursor
   becomes a polyline tool.
2. Draw a polyline on the 2-D slice:
   - Click each vertex (one or more segments are fine).
   - The path is rendered as a yellow line, with control points at every
     vertex.
3. Set the **path width** (in pixels) in the right-side card — this
   controls how many adjacent pixels perpendicular to the path are
   averaged to make the PV (default 3). Larger width → smoother PV but
   blurs narrow features.
4. Click **Compute**. The backend reads the cube along the path and
   builds the PV map; the result opens in a dedicated *PV viewer* with
   its own LUT, range, and WCS overlay.

### What you see

- **X axis** — position along the polyline (arcsec or pixels, depending
  on whether the cube has spatial WCS).
- **Y axis** — spectral coordinate (velocity, frequency, channel).
- **Color** — intensity (BUNIT).

A small extras panel shows:

| | |
|---|---|
| **Total length** | Polyline length in arcsec (if WCS is available) |
| **Spatial unit** | arcsec / pixel |
| **Pixel scale** | arcsec / pixel along the path |
| **BUNIT** | Cube brightness unit |
| **Beam** | If the cube header carries beam metadata (BMAJ / BMIN / BPA) |

### Tips

- Place the polyline **along** the kinematic axis (the direction of
  rotation, the outflow axis, etc.) for the clearest signature.
- Two short segments at an angle let you trace a curved structure (jet
  break, S-shape).
- Use *Slice playback* in the cube viewer first to identify where the
  emission lives, then draw the PV path on a representative slice.

### Caveats

- The PV is computed on the **full-resolution** cube on the backend, not
  on a downsampled preview, so the first PV after opening a cube may take
  longer if the high-res swap hasn't completed.
- It's a heavy backend task, so it's throttled to leave a worker free for
  interactive slice scrolling; you can keep navigating the cube while
  the PV computes.

---

## Noise estimation

A robust noise (σ) estimate per channel is essential for setting moment
masks, deciding line-free channels for baseline subtraction, and computing
SNR for sources. VisIVO computes the noise on the **backend** using a
**median absolute deviation (MAD)** estimator over a user-defined spatial
region and channel range.

### How to use it

1. **Tools → Estimate Noise** in the cube viewer.
2. The *Noise Region — Select Region* dialog appears with six spinboxes:
   - **Spatial region** — `x min / x max`, `y min / y max` in slice pixel
     coordinates. Defaults to the full cube extent. *Reset to Full Extent*
     restores the defaults.
   - **Channel range** — `Channel start / end`. Defaults to all channels.
     For per-channel σ across the full cube, leave defaults; for a single
     scalar σ over a specific line-free band, narrow to those channels.
3. Click **OK**. The dialog closes and the backend starts computing.
4. The result panel shows summary stats over the per-channel arrays:
   - **σ per channel** = 1.4826 × MAD (robust 1σ for Gaussian noise),
     summarised as *Mean / Median / Min / Max*.
   - **MAD per channel**, same four summary stats.
   - **Subtitle** with the selected region and a warning when too much of
     the selection is blank — see [Blank-pixel handling](#blank-pixel-handling).

### Live preview overlays

While the *Noise Region* dialog is open — and through the result dialog's
lifetime — the cube viewer paints two synchronised amber overlays so you
can see exactly where σ is being sampled:

- **2D slice** — an amber outline rectangle following `(x0..x1, y0..y1)`.
  Distinct colour from the cyan Box-region tool so the two never collide.
- **3D volume** — a semi-transparent amber box (translucent face +
  wireframe edge) that mirrors the same `(x, y)` rectangle and extrudes
  it along the spectral axis over `chStart..chEnd`. The volume rendering
  stays visible through the translucent face so you can see emission
  features inside the sampling box.

Both overlays update **live** as you tune any of the six spinboxes —
including the channel range, so the 3D box stretches/shrinks along Z in
real time.

The overlays survive the picker → compute → result-dialog sequence so
you can read the σ numbers while still seeing where they came from. They
are torn down automatically when you close the result dialog.

```{tip}
The 3D box is the quickest way to verify your channel range avoids the
line: rotate the camera so you look down the spectral axis, then resize
the box along Z until it sits in a flat (line-free) region of the volume.
```

### Blank-pixel handling

Many radio / IR cubes use exactly `0.0` for off-source pixels instead of
NaN. If you compute MAD over a region that mostly overlaps that blanked
area, the formula collapses (`median(finite)=0` AND `median(|finite-0|)=0`)
and you get `σ=0` for every channel — clearly wrong, but easy to miss
without context.

The backend has a **zero-as-blank heuristic** in
`worker_noise_estimate`: when more than half the finite pixels in a
channel are exactly 0, those zeros are excluded from that channel's MAD
(treated as blanks). Zeros are kept in when they are a minority because
some cubes (post-baseline subtraction, calibrated brightness
temperatures) legitimately have noise centred on zero.

The response also carries a `blank_fraction` field (0–1) covering the
whole selection. The result-dialog subtitle surfaces it:

- `blank_fraction > 20 %` → soft warning "X % of the selected pixels are
  blank, result still computed over all finite pixels".
- `blank_fraction > 50 %` → stronger advice "σ excluding zeros — pick a
  tighter region inside the data footprint for a clean estimate", and
  σ/MAD are computed on the non-zero subset only.

When you see this warning, restrict the spatial region to a clearly-non-
blank area (use the amber overlays as a guide — anywhere the box sits
over the black border of the slice is contributing zeros).

### Why MAD instead of stddev?

A sample standard deviation is dominated by outliers (faint sources, RFI,
edge artefacts). MAD × 1.4826 returns the same value for purely Gaussian
data but is unaffected by ≤50 % contamination. This is the de-facto
robust estimator in radio astronomy.

### Picking a noise region

The region should be:

- **In the same field of view** as your source (similar PSF, primary
  beam attenuation).
- **Line-free** in the spectral range of interest (otherwise you'll
  measure line + noise, not noise).
- **Large enough** to be statistically meaningful (a few thousand pixels
  per channel is comfortable; smaller regions become noisy at the σ
  level).

```{tip}
A common workflow: open the cube, do a *Mean Spectrum* (Tools menu) to
spot the line-free channels, then estimate noise over a generous spatial
region restricted to those channels.
```

---

## See also

- [Moment maps](moment-maps) — apply the noise as RMS / threshold mask.
- [Spectral tools](spectral-tools) — baseline subtraction uses line-free
  channels you can identify with the noise tool.
- Developer reference: [`/v1/cube/noise`](../backend-api#cube),
  [`/v1/cube/pv`](../backend-api#cube).
