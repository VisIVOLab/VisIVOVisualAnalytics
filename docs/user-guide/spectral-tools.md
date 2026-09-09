# Spectral analysis tools

Tools that work along the velocity axis. Three of them compute something from
the whole cube; two work on a spectrum you already have on screen.

| Tool | What it does | Output |
|------|--------------|--------|
| [Line identification](#line-identification-overlaying-a-line-list) | Overlay a rest-frame line list on a spectrum, shifted to the source | Markers on the plot |
| [Gaussian line fit](#gaussian-line-fit) | Single Gaussian + linear baseline, with uncertainties | Fitted curve + summary |
| [Line-width map](#line-width-maps-s-02) | Per-pixel FWHM (Gaussian fit) + equivalent width | Two 2-D maps |
| [Baseline subtraction](#baseline-subtraction-s-03) | Polynomial / median baseline fit and subtract | New cube dataset |
| [Spectral stacking](#spectral-stacking-s-04) | Combine N cubes into a single spectrum / cube | 1-D spectrum |

The three cube-wide tools are non-modal — you can keep interacting with viewers
while they run — and gated by the backend's heavy-task throttle so they don't
block slice scrolling.

How to *get* a spectrum on screen in the first place (probe a pixel, a region
mean, a 3-D pick) is in [Cube viewer → Extract
Spectrum](cube-viewer#extract-spectrum-probe-a-single-pixel).

---

## Line identification (overlaying a line list)

**Inspector ▸ Analysis ▸ SPECTRUM ▸ Load Lines…**, with any spectrum pane
selected. Draws each transition of a line list as a vertical dashed amber
marker with a rotated label, so you can see at once whether a peak sits where a
species predicts.

### The two things a line list cannot tell you

A line list carries **rest** frequencies. Two conversions stand between that
and a position on your plot, and both are asked for in the dialog because
guessing either puts every marker in the wrong place:

1. **The unit.** A bare number is not a frequency. The dialog pre-selects the
   unit when the file declares one — a `# unit = GHz` comment, or a column
   named like Splatalogue's `Freq-GHz(rest frame)` — and otherwise asks.
2. **The frame.** Rest frequencies must be redshifted to the frame of *your*
   source: `f_obs = f_rest / (1 + z)`. Give either a **systemic velocity in
   km/s** (read as an optical velocity, which is what source catalogues quote,
   so `z = v/c`), or **z** directly, or say the values are **already in the
   observed frame** if your list is pre-shifted.

The dialog states what it has to work with, e.g. *"This axis: VOPT in m/s ·
cube rest frequency 1420.4058 MHz"*, or *"no RESTFRQ in the header"* when the
cube does not carry one.

### Velocity conventions are not a detail

Placing a line on a **velocity** axis needs the cube's own rest frequency
(header `RESTFRQ`/`RESTFREQ`) *and* the axis's velocity convention, taken from
its `CTYPE`:

```{list-table}
:header-rows: 1
:widths: 18 34 48

* - CTYPE
  - Convention
  - Axis coordinate of an observed frequency *f*
* - `VOPT`, `FELO`
  - Optical
  - `v = c (f_rest/f − 1)`
* - `VRAD`
  - Radio
  - `v = c (1 − f/f_rest)`
* - `VELO`
  - Relativistic (the FITS standard reading)
  - `v = c (f_rest² − f²)/(f_rest² + f²)`
* - `FREQ`
  - —
  - the observed frequency itself, converted to the axis unit
```

Optical and radio velocities of the same line differ by about `v²/c`: already
**3.3 km/s at 1000 km/s**, which is several channels of a typical HI cube. This
is why the convention is read from the header rather than assumed — and why a
marker drawn by an older version of this tool, which used the file's number as
a plot coordinate directly, could be off by channels or off-plot entirely.

### Where the lines come from

- **Common radio / mm lines (bundled)** — the default, and it works with no
  network: HI 21 cm, the four OH ground-state lines, the CH₃OH 6.7 GHz and
  H₂O 22 GHz masers, NH₃ (1,1)/(2,2)/(3,3), HCN / HCO⁺ / HNC / N₂H⁺ / CS 1–0,
  the CO, ¹³CO and C¹⁸O ladders up to 3–2, H₂CO 218 GHz and C I 492 GHz.
- **A file** — either a two-column `value,label` list (`#` for comments), or a
  **catalogue export**: the loader reads the header row and recognises the
  frequency column (skipping error columns), the species / chemical-name
  column, and the quantum-number column, which it appends to the label so a
  marker reads *"CO 2-1"*. Comma, tab, semicolon and colon separators are all
  handled, and quoted fields are unquoted.

```{caution}
The bundled list is a **convenience**, rounded to the kHz from the public
CDMS / JPL values as distributed through Splatalogue. For a line
identification that goes into a paper, export the transition from
CDMS / JPL / Splatalogue yourself and load that file — the loader reads such
an export directly. The bundled file is
`resources/spectral_lines/common_lines.csv` in the source tree if you want to
extend it.
```

There is deliberately **no online catalogue query**. The value it would add over
"export the CSV once and load it" is small, the query interfaces of the public
services change without notice, and the backend is routinely deployed on compute
nodes with no outbound internet — a demo that hangs on a network call is worse
than one extra step.

### When nothing appears

A list in the wrong unit, or drawn unshifted, converts perfectly well and lands
entirely outside the plotted band, where you would see nothing and conclude the
feature is broken. So the tool checks and says so:

- *"3 lines placed, but none falls inside the plotted range (9e+05 – 1.31e+06
  m/s) — check the unit and the systemic velocity / redshift"*;
- *"None of the 12 lines could be placed on this axis: this cube's header has
  no RESTFRQ, so a frequency list cannot be converted to its velocity axis"* —
  in that case load a list in the axis's own unit instead;
- a velocity list on a frequency axis is refused with the same kind of message.

**Clear Lines** removes the overlay. It is also dropped automatically when the
pane is re-used for a product on a **different spectral axis**: the markers are
stored as coordinates on the axis they were computed for, and keeping them
across an axis change would draw them at meaningless positions.

### Worked example (HI, WALLABY cube)

The demo cube has `CTYPE3 = VOPT`, `CUNIT3 = m/s`,
`RESTFRQ = 1420405751.79`, covering roughly 900–1310 km/s.

1. Probe a pixel on a source so a spectrum pane is on screen, and select it.
2. **Load Lines…** → keep *Common radio / mm lines (bundled)*; the unit shows
   **GHz**.
3. Set *Systemic velocity* to the source's velocity — say **1100** km/s.
4. The **HI 21 cm** marker lands at exactly 1.1 × 10⁶ m/s: for the cube's own
   transition the axis coordinate *is* the systemic velocity, which makes this a
   good sanity check of the whole chain. Every other line in the bundled list is
   far outside the band, and the dialog says so rather than drawing nothing.

---

## Gaussian line fit

**Inspector ▸ Analysis ▸ SPECTRUM ▸ Fit Gaussian** fits a single Gaussian plus
a **linear baseline** to the displayed spectrum, through the backend, and
overlays the model as an orange dashed curve. The stats bar is replaced by the
fit summary:

> `Gaussian fit · peak 0.0243 ± 0.0011 · centre 1.1013e+06 ± 420 · FWHM 8.4e+04 ± 3.1e+03 · ∫ 2.18 · rms 0.0021`

- Uncertainties are the backend's 1 σ values. Where one cannot be determined no
  `±` is printed rather than a fake zero.
- **Correlated channels**: a spectrum smoothed by the instrument (or by the
  display kernel) has fewer independent samples than channels, so naïve
  least-squares errors are too small. When the backend can quantify it, the
  summary says `errors ×1.4 for correlated channels`; when it cannot, it says
  the uncertainties are uncorrected. Either way, do not quote them as if the
  channels were independent.
- Only finite channels are sent: region spectra contain NaN channels where the
  region falls outside valid data, and at least 4 finite channels are required.
- **Clear Fit** removes the curve and its summary and puts the spectrum's own
  statistics back. A fit still in flight is invalidated too, so a clear is not
  undone a moment later by a late reply.

```{caution}
One Gaussian plus a line is a *model*, and a poor one for a rotating disk's
double-horned HI profile, for self-absorbed lines, or for blended components.
The residual is not shown: judge the fit by eye against the data, and treat the
FWHM of a non-Gaussian profile as a summary number, not a line width. For a
per-pixel width over the whole cube use the [line-width
map](#line-width-maps-s-02), which reports the model-free equivalent width
beside the fitted FWHM for exactly this reason.
```

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
