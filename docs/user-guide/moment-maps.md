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

1. **Tools → Compute Moment** (or the menu shortcut in the Tools sidebar).
2. The Moment dialog appears (non-modal — keep cubing while it computes):
   - **Order** — see the table below.
   - **Channel start / end** — the spectral range to integrate over.
     Defaults to the full cube. Restrict it to the channels containing the
     line to avoid integrating background.
   - **Mask** — **on by default**. Only voxels above the threshold contribute,
     which is what keeps M1/M2 physically clean (see the note below).
   - **Auto threshold (≈ 3 σ)** — **on by default**. The backend derives the
     mask threshold from the data itself (median + 3 · σ, with σ from the MAD),
     the same convention CASA/SoFiA use. Untick it to type a value by hand in
     **Mask threshold**; untick **Mask** entirely to compute an unmasked moment.
   - **RMS** — the cube noise level, used by some orders (e.g. weighted by
     1/σ²). Auto-filled from the latest noise estimate if available.
3. Click **Compute**. The Moment card in the sidebar tracks the state
   (`Computing… → Ready` or `Error`).
4. When ready the result appears in the 2-D dock (replacing the slice
   view); the *2-D mode* combo flips to **Moment**.

You can switch back and forth between *Slice* and *Moment* without
recomputing — the moment map is cached in memory and on the backend's
LRU `PRODUCT_CACHE` (keyed by `dataset_id + parameters`). Identical
re-requests return instantly.

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
Use the [Probe](cube-viewer#probing-a-spectrum) to find the start/end
channels visually, or extract a noise map first
([Noise](region-pv-noise)) and use the *Mask* threshold.
```

## Reading the result

The moment dialog displays:

- **Value range** (min / max) over finite pixels
- **BUNIT** (carried from the cube header)
- **Spectral axis unit** (used for orders 1, 2, etc.)
- **Sanity warnings** if the WCS was sanitised, if the integration range
  is fully blanked, etc.

The result colour map defaults to *Inferno* but you can change it from the
*2-D View Settings → Moment LUT* combo. Open the *LUT Customizer* for
fine-grained percentile clamping.

## Using a moment map elsewhere

You can right-click the moment dock and:

- **Export PNG** — current view, with WCS overlay.
- **Save as new dataset** — registers the moment as a backend dataset and
  returns a new `dataset_id` you can open in the [Image viewer](image-viewer)
  for further analysis (regions, photometry, layer comparisons).
- **Send via SAMP** — broadcast to TOPCAT / Aladin
  ([Catalogues & HiPS](catalogues-hips#samp)).

## Performance & caching

- The computation runs in the backend worker pool. Default `VISIVO_WORKERS=4`
  with `VISIVO_HEAVY_SLOTS=3` reserves one slot for interactive requests:
  you can scroll slices, probe spectra, rotate the cube during a moment
  compute.
- Results are LRU-cached on the backend (`PRODUCT_CACHE`, default 32
  entries). Identical recomputes (same dataset + params) return in
  sub-milliseconds.
- **Out-of-core by default.** M0/M1/M2 stream the cube in spectral slabs
  rather than loading the whole channel range, so a moment over a 100+ GB
  cube never materialises it in RAM. The streaming path kicks in when the
  selected subset would exceed `VISIVO_MOMENT_STREAM_BYTES` (default 2 GiB);
  slab size is `VISIVO_MOMENT_SLAB_CHANNELS` (default 64).
- **Dask is on by default** (`VISIVO_DASK_MODE=auto`). Large moments
  (materialised subset ≥ `VISIVO_DASK_MIN_BYTES`, default 512 MB) are computed
  with the chunked `dask.array` path — across a distributed cluster when
  `VISIVO_DASK_SCHEDULER` is set, otherwise with a local threaded scheduler
  that parallelises across cores. This also gives the high-order moments
  (M3–M10) an out-of-core path they previously lacked. Small cubes stay on the
  fast in-process worker. Set `VISIVO_DASK_MODE=off` to force the plain worker,
  `local` to always use local threaded Dask, or `distributed` to require a
  cluster. See the [backend API reference](../backend-api) for the full
  tunables table.

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
- Developer reference: [`/v1/products/moment` API](../backend-api#products) — request/response shapes, full unit table.
