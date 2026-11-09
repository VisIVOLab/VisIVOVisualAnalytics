# Moment maps

A **moment map** collapses the spectral axis of a cube into a single 2-D
image whose pixel value encodes a statistical property of the spectrum at
that line of sight. They are the most common derived product in radio
astronomy because they distill cube structure into intuitive 2-D pictures:
where the line is bright, where it is centred, how broad it is, how
skewed.

VisIVO computes moments backend-side, so the heavy work runs in a Python
worker and the result is sent back as a base64-encoded `float32` map plus
its scientific metadata.

## How to compute one

In the [cube viewer](cube-viewer):

1. **Tools → Maps → Compute Moment…** (or *Compute Moment* in Inspector ▸
   Analysis).
2. The *Moment Map Settings* dialog appears (non-modal — keep working while it
   computes):
   - **Moment order** — see the table below.
   - **Start → End** — the channel range to integrate over.
     Defaults to the full cube. Restrict it to the channels containing the
     line to avoid integrating background.
      - **Enable threshold mask** — **on by default**. Only voxels above the threshold contribute,
     which is what keeps M1/M2 physically clean (see the note below).
   - **3σ of the noise estimate** — shown, and chosen by default, once you have
     run *Tools → Estimate Noise* in this session: the threshold is 3σ of the σ
     you measured on a line-free region (the tooltip says where), and the
     product's provenance records it as such.
      - **Auto threshold (≈ 3σ from MAD)** — the default when there is no estimate. The
     backend derives the mask threshold from the data itself (median + 3 · σ,
     with σ from the MAD over the cube), the same convention CASA/SoFiA use.
          Untick both to type a value by hand in **Mask threshold**; untick **Enable
     threshold mask** to compute an unmasked moment.
3. Click **Compute**. The status bar says *Computing moment map...* and then
   gives the result's order, unit, channel range and mask. The channel range and mask settings are
   **remembered per dataset**, so re-opening the dialog for the same cube (this
   session or later) restores your last-used range and threshold rather than
   the full-cube default.
4. When ready the map is shown in the 2-D Slice pane in place of the
   channels and added to Session Data; *2D Slice* in the pane's ▾ menu brings
   the channels back.

```{note}
**Long computations.** The moment runs as a backend *task*, which has no time
limit: on an SKA-scale cube an M2 takes minutes (about 15 for a 450 GB cube
on a cluster file system).
While it runs, a progress window above the cube window says whether the task
is queued or running and how long it has been going. **Cancel** stops the task
on the backend (it does not just hide the window), and so does closing the cube
window, or quitting the application: the window does not block the rest of the
application, so **Quit** (⌘Q, the Dock's *Quit*) stays available on macOS while
a moment runs. The Moment dialog can be closed meanwhile; a second *Compute*
waits for the first to end. If the task fails or is cancelled, the client reports it; it does not
start the computation again by another route. Only a backend too old to have
task routes is asked with a single synchronous request instead, which cannot
be cancelled and is limited by that backend's request timeout. Exporting the
moment as FITS ([below](#using-a-moment-map-elsewhere)) works the same way, and
computes the map exactly as the moment itself is computed: the exported file
holds the map you see, value for value, and an export right after the moment is
served from the backend's cache.
```

You can switch back and forth between *Slice* and *Moment* without
recomputing — the moment map is cached in memory and on the backend's
LRU `PRODUCT_CACHE` (keyed by the file, its version on disk and the
parameters). Identical re-requests return instantly; a file replaced or
rewritten on disk since is computed again.

```{note}
**Why M1/M2 are masked by default.** An intensity-weighted mean velocity (M1)
is only meaningful where there is signal. On pure-noise pixels the denominator
M₀ → 0 and the ratio explodes — without a mask you get "velocities" far outside
the cube's spectral axis (even faster than light), which blow out the colour
scale and hide the real rotation field. VisIVO applies two safeguards: a
**default ~3 σ mask** (removes the noise pixels), and an always-on
**physical-range guard** that blanks any M1 outside the sampled velocity axis
and any M2 outside `[0, span²]` — even when you deliberately compute unmasked.
The guard never removes a legitimate voxel, because a positive-weight moment is
bounded by construction.
```

## The supported orders

```{list-table}
:header-rows: 1
:widths: 6 22 28 16 28

* - Order
  - Name
  - Formula
  - Unit
  - Notes
* - **0**
  - Integrated intensity
  - Σᵢ Iᵢ · Δvᵢ
  - BUNIT · spectral
  - The flux integral. Use for total-line maps and to identify where
    emission is.
* - **1**
  - Intensity-weighted velocity
  - Σᵢ Iᵢ · vᵢ · Δvᵢ / M₀
  - spectral
  - The line centroid per pixel. Reveals velocity gradients (rotation,
    outflows, infall).
* - **2**
  - Intensity-weighted velocity dispersion
  - √( Σᵢ Iᵢ · (vᵢ − M₁)² · Δvᵢ / M₀ )
  - spectral
  - Line width per pixel (standard deviation, not FWHM). For FWHM-style
    line widths see also [Line-width maps](spectral-tools).
* - **3**
  - Skewness
  - μ₃ / σ³
  - dimensionless
  - >0 → tail on the high-velocity side; <0 → tail on the low side.
* - **4**
  - Excess kurtosis
  - μ₄ / σ⁴ − 3
  - dimensionless
  - >0 → peakier than Gaussian; <0 → flatter / two-peak.
* - **5**
  - Standardised 5th moment
  - μ₅ / (M₀ · σ⁵)
  - dimensionless
  - Sensitive to asymmetric tails on top of skewness.
* - **6**
  - RMS
  - √( Σᵢ Iᵢ² · Δvᵢ / Σᵢ Δvᵢ )
  - BUNIT
  - Root-mean-square value over the integration range. Useful as a
    quick "signal strength" map.
* - **8**
  - Maximum value
  - max(I)
  - BUNIT
  - Peak intensity per spectrum. Equivalent to a single-line peak map.
* - **10**
  - Minimum value
  - min(I)
  - BUNIT
  - Sometimes useful for absorption studies.
```

> NaN / blanked voxels are excluded from every formula. For orders 1–5 the
> denominator (M₀ or σ) is required to be non-zero; pixels where it
> vanishes are set to NaN in the result.

## How to choose orders and channels

```{list-table}
:header-rows: 1
:widths: 28 72

* - Goal
  - Recipe
* - "Where is the line?"
  - M0 over the full range, no mask. Look at the morphology and total flux
    distribution.
* - "Is it rotating?"
  - M1 over a tight channel range that contains only the line, with a
    threshold mask above ~3 × RMS to remove noise pixels (M1 is noisy when
    M0 → 0).
* - "Where is the line wide?"
  - M2 with the same tight range + 3σ mask. High M2 ≈ broadened linewidths
    (turbulence, multiple components, outflow wings).
* - "Where is the line asymmetric?"
  - M3 with a wider range so the tails are captured. Sign tells you which
    side the tail is on.
* - "Where is the peak signal?"
  - M8 over the line range. Often more robust than M0 against noise
    integration when the line is narrow.
* - "Quick S/N proxy"
  - M6 over the line range divided by the noise σ map.
```

```{tip}
Choosing the channel range matters. Integrating over a channel-wide range
that includes line-free noise channels makes M0 noisier and biases M1/M2.
Use the [Probe](cube-viewer.md#extract-spectrum-probe-a-single-pixel) to find the start/end
channels visually, or extract a noise map first
([Noise](region-pv-noise)) and use the *Mask* threshold.
```

## Reading the result

The status bar sums the result up (*Moment map: … | Unit: … | Ch: … | Mask:
… | Scope: …*), and its tooltip and the product's provenance (Inspector ▸
Parameters) give the details:

- the moment's **unit** and the cube's **BUNIT**
- the **spectral axis** type and unit (used for orders 1, 2, etc.)
- the **channel range**, the **mask** and its **threshold**
- a transient **warning** if the WCS was sanitised or degraded.

The colour map follows the order: *Inferno* for M0 (and the orders above 2),
*CoolWarm* for the velocity field M1, *Viridis* for M2. Change it with *Color map* in the left dock's DISPLAY section while the
moment's pane is the active one; **Edit** beside it opens the LUT editor for
finer clamping.

## Using a moment map elsewhere

From the cube viewer's **Tools ▸ Export** menu, with the moment's pane
focused:

- **Export … as FITS…** (the entry carries the moment's name) writes the
  moment as a 2-D FITS in Workspace Exports, with the celestial WCS and
  `BUNIT`. The backend recomputes it at full precision from its recipe. Open
  that file with *File ▸ Open…* to use it in the [Image viewer](image-viewer)
  (regions, photometry, layer comparisons), or hand it to another tool. The
  same entry on another map's pane writes that map, with the parameters it
  was computed with recorded as `HISTORY` ([FITS exports](cube-viewer.md#fits-exports--workspace)).
- **Tools ▸ Views ▸ Overlay Slice on an Open Image** draws the current 2-D map
  as contours over an image already open in an image viewer.

*Tools ▸ Maps ▸ Angular Power Spectrum of Moment Map* and *Skeleton / Voids of
Moment Map* analyse the moment on screen. They first write it at full
resolution to the Workspace Exports. See
[Cosmology tools](cosmology.md#angular-power-spectrum).

## Performance & caching

- The computation runs in the backend worker pool. Default `VISIVO_WORKERS=4`
  with `VISIVO_HEAVY_SLOTS=3` reserves one slot for interactive requests:
  you can scroll slices, probe spectra, rotate the cube during a moment
  compute.
- Results are LRU-cached on the backend (`PRODUCT_CACHE`, default 32
  entries, `VISIVO_PRODUCT_CACHE_ENTRIES`). Identical recomputes (same dataset + params) return in
  sub-milliseconds.
- **In memory only when it fits.** The selected channel range (channels ×
  width × height × 4 bytes) is held in memory only when it is no larger than
  the smaller of `VISIVO_MOMENT_STREAM_BYTES` (default 2 GiB) and one twelfth
  of the memory the backend has available (its container's headroom, when it
  runs in one). A larger range is computed out of core: M0, M1 and M2 stream
  the cube from disk in spectral slabs, M3 to M10 go through the chunked Dask
  path, so a moment over a 100+ GB cube never materialises it in RAM.
- **Block size is set in bytes.** Each streamed slab or Dask block holds at
  most `VISIVO_MOMENT_CHUNK_BYTES` (default 64 MiB), and at least one channel.
  `VISIVO_MOMENT_SLAB_CHANNELS` (default 64) only caps the number of channels
  in a slab. Moments that need the centroid first (M2 to M5) read the data in
  two passes, so memory stays bounded by one block.
- **Dask is on by default** (`VISIVO_DASK_MODE=auto`). With Dask installed,
  moments over a range of at least `VISIVO_DASK_MIN_BYTES` (default 512 MiB)
  are computed with the chunked `dask.array` path and a local threaded
  scheduler that parallelises across cores; when `VISIVO_DASK_SCHEDULER` names
  a distributed cluster, every moment is computed there. Smaller ranges stay on
  the in-process worker. Set `VISIVO_DASK_MODE=off` to force the plain worker,
  `local` to always use local threaded Dask, or `distributed` to require a
  cluster. All the moment and Dask variables are listed in the
  [Configuration reference](../configuration.md#moments-and-dask).

## Common pitfalls

```{list-table}
:header-rows: 1
:widths: 40 60

* - Symptom
  - Cause / fix
* - M1 map is dominated by noise everywhere except the bright source.
  - You unticked the mask. Re-enable it (it is on by default with an auto
    ~3 σ threshold), or tighten the channel range. Note the map can no longer
    show *super-luminal* values — the physical-range guard blanks those — but
    an unmasked map still speckles with in-range noise.
* - M2 has implausibly large values at the cube edges.
  - Low M0 in the denominator → numerical noise amplification. Keep the mask on
    (default) or raise the threshold; the guard already caps M2 at `span²`.
* - "Backend returned an error: M5 normalisation undefined."
  - Cube is mostly blanked over the integration range. Choose a different
    range or extract a noise mask first to ensure σ ≠ 0.
* - Result is identical even after changing channels.
  - You hit the cache: the parameters were already used. Open the
    Diagnostics panel to confirm (`[cache] hit key=moment:…`). To force a
    recompute change any param (even by ±1 channel).
```

## See also

- [Spectral tools](spectral-tools) — FWHM line-width maps, baseline subtraction.
- [Regions, PV, noise](region-pv-noise) — for noise estimation and PV diagrams.
- Developer reference: [`/v1/products/moment` API](../backend-api/products.md) — request/response shapes, full unit table.
