# Spectral analysis tools

Three tools live under **Tools → Spectral Analysis** in the main window and
in the cube viewer:

| Tool | What it does | Output |
|------|--------------|--------|
| [Line-width map](#line-width-maps-s-02) | Per-pixel FWHM (Gaussian fit) + equivalent width | Two 2-D maps |
| [Baseline subtraction](#baseline-subtraction-s-03) | Polynomial / median baseline fit and subtract | New cube dataset |
| [Spectral stacking](#spectral-stacking-s-04) | Combine N cubes into a single spectrum / cube | 1-D spectrum |

All three are non-modal — you can keep interacting with viewers while they
run — and gated by the backend's heavy-task throttle so they don't block
slice scrolling.

---

## Line-width maps (S-02)

For each spatial pixel the backend computes two summary quantities of the
spectral profile:

```{list-table}
:header-rows: 1
:widths: 22 38 40

* - Quantity
  - Definition
  - Use it when
* - **FWHM** (full width at half maximum)
  - 2.3548 × σ from a Gaussian fit of the spectrum at that pixel
  - You want the classical line width in spectral units (km/s, Hz, …).
    Sensitive to line shape: very robust for Gaussian-like emission,
    overestimates non-Gaussian profiles.
* - **EW** (equivalent width)
  - M0 / peak brightness = ∫ I dv / max(I)
  - You want a model-free width estimate. Works on any profile shape.
    Returns a "boxcar-equivalent" channel width.
```

### How to use it

1. **Tools → Line-Width Map…** in the cube viewer or main window. The
   dialog accepts:
   - **Channel start / end** — restrict the fit to where the line is. The
     fit is sensitive to including line-free noise channels.
   - **Mask threshold** — only fit pixels above this intensity (a quick way
     to skip background voxels).
2. Click **Compute**. A progress overlay appears in the dialog (the rest
   of the app stays interactive).
3. The result opens in a dedicated *Science Map* window — two side-by-side
   maps (FWHM, EW), each with its own range and unit.

### Optimisations (under the hood)

- The cube is split into row chunks and fitted in parallel across the
  backend worker pool. With `VISIVO_WORKERS=4` you get ~4× speed-up on a
  cold compute.
- An adaptive **SNR cutoff** (default 3, set via `VISIVO_LINEWIDTH_SNR`)
  skips background pixels before the Gauss fit — typically ≥80 % of a
  cube is background, so the actual fit cost is much lower than naïve
  pixel count.
- Results are cached in the backend `PRODUCT_CACHE`: re-clicking
  *Compute* with the same parameters returns in sub-milliseconds.

### Interpreting the maps

- **FWHM**: peaks in regions of broadened lines — turbulence, multiple
  components blended, large-scale flows. Compare with M2 (moment-2)
  → if FWHM ≫ M2 the line is non-Gaussian.
- **EW**: complementary check on FWHM. Where EW ≪ FWHM the profile is
  peaked above a Gaussian; where EW ≫ FWHM the profile has flat or
  multi-peak shape.
- Cells set to NaN in the output mean the fit failed (low SNR, divergent,
  bounds hit). Lower the SNR cutoff or relax the channel range to recover
  some.

```{caution}
The Gaussian fit is per-pixel and unmasked between pixels. There is no
clean-image regularisation. For very deep maps with low SNR you may get
patchy results; consider averaging adjacent pixels or running the tool on
a smoothed cube.
```

---

## Baseline subtraction (S-03)

Polynomial (or median) baseline fitting per pixel along the spectral axis.
The fit is anchored on **line-free channel ranges** that you specify and
subtracted from every pixel. The result is a **new cube dataset** with the
baseline-subtracted data; it's registered on the backend with a fresh
`dataset_id` and can be opened immediately in a new cube viewer.

### How to use it

1. **Tools → Baseline Subtraction…**
2. Fill in:
   - **Channel start / end** — overall range to process (defaults to full
     cube).
   - **Method**: *Polynomial* or *Median*.
     - *Polynomial* fits a `poly_order` polynomial (typically 1–3) to the
       line-free channels and subtracts it from every channel in the
       range.
     - *Median* subtracts the median of the line-free channels (constant
       per pixel; equivalent to *poly_order = 0*).
   - **Polynomial order** — order of the fit (1 = linear, 2 = quadratic,
     3 = cubic). Higher orders fit instrumental ripples but overfit
     emission if `line_free_channels` is too aggressive.
   - **Line-free channels** — required. Either a flat list
     `[5, 6, 7, 80, 81]` or a list of ranges `[[5, 7], [80, 90]]`. These
     must NOT contain emission lines, otherwise the fit absorbs your
     signal.
3. Click **Compute**. On success the dialog shows:
   - **RMS before / after** — the residual RMS in the line-free channels
     before and after subtraction. After should be similar to or below
     before (otherwise the fit is too rigid).
   - **Fit channels** — how many channels actually contributed.
   - **New dataset ID** — click it to open the subtracted cube in a new
     cube viewer.

```{tip}
Estimate the noise once first ([Noise tool](region-pv-noise#noise-estimation))
to find line-free regions; or look at a spatially averaged spectrum
(*Tools → Mean Spectrum* in the cube viewer) and pick the channels by
eye in the Profile window.
```

### Why subtract a baseline?

Many radio receivers add a slowly-varying bandpass response on top of the
science signal. Without removing it:

- Moment-0 integration accumulates the baseline area on top of the line
  area → biased flux.
- Spectral stacking averages baselines from multiple cubes (which often
  don't align) → false features.
- The displayed dynamic range is dominated by the baseline drift, not by
  the line.

Subtracting a polynomial baseline before computing moments / stacking is
the standard first step in spectral-line analysis.

### Limits & caveats

- The fit is **per pixel**, independently. There is no spatial smoothing
  of the baseline coefficients.
- Pixels where the fit fails (singular matrix, all-NaN spectrum) are left
  unchanged in the output and flagged in the diagnostic log.
- The cube is materialised on disk as a new FITS in the backend's temp
  directory. Repeated baseline runs on the same dataset can accumulate
  files; clean periodically.

---

## Spectral stacking (S-04)

Combine N spectral cubes opened in the same backend session into a single
1-D combined spectrum (or, in upcoming versions, a stacked cube). Useful
when you have several pointings or several sources of the same type and
want their average / median spectrum for population-level analysis.

### How to use it

1. Open **two or more cubes** in the session. Each `Open Remote Dataset`
   from the same client instance shares the same session.
2. **Tools → Stack Spectral Cubes…** The dialog lists all the cubes
   currently open in the session, each labelled with the file basename and
   shape `(W × H × D)`:
   - The cube of the window from which you opened the dialog is
     pre-selected.
   - Cubes whose `(W × H × D)` differs from the reference cube are listed
     but **greyed out** with a tooltip — they can't be stacked because
     the spectral grid and spatial shape must match.
3. Tick the cubes you want to include (≥ 2 required).
4. Choose:
   - **Method**: *Mean*, *Median*, or *Weighted mean*.
   - **Weight by** (only for *Weighted mean*): *Uniform*, *RMS*,
     or *Peak*. With *Uniform*, all cubes get weight 1; with *RMS* the
     cubes are weighted by 1/σ² (their noise level), with *Peak* by
     `peak_intensity`.
   - Or pass **explicit weights** as a list (e.g. for unequal exposure
     times).
5. Click **Stack**. The result opens in a QCustomPlot 1-D spectrum window
   with the combined spectrum, the method used, and the number of
   contributing cubes.

### When to use which method

```{list-table}
:header-rows: 1
:widths: 20 80

* - Method
  - When to pick
* - **Mean**
  - Default. Best statistical SNR when all cubes have similar noise and
    no contamination.
* - **Median**
  - Robust against outliers. Use when individual cubes may have spikes,
    RFI, or strong residual baselines.
* - **Weighted mean**
  - When cubes have noticeably different noise levels (different
    integration time). With `weight_by="rms"`, lower-noise cubes contribute
    more, improving final SNR over plain mean.
```

### Caveats

- The stacking does NOT align cubes spatially — pixel `(i, j)` in cube A
  is averaged with pixel `(i, j)` in cube B. If your cubes are not on the
  same spatial grid (same WCS, same pixel size), reproject them first.
- Same goes for the spectral axis: same number of channels, same
  resolution, same reference frame.
- Output is currently a 1-D spectrum (spatially averaged across the
  selected cubes). Pixel-by-pixel stacked cubes are a planned addition.

---

## See also

- [Moment maps](moment-maps) — derive M0–M10 maps from a cube.
- [Regions, PV, noise](region-pv-noise) — noise estimation, region
  statistics, PV extraction.
- Developer reference: [`/v1/spectral/*` API](../backend-api#spectral)
  — request/response shapes and binary frame format.
