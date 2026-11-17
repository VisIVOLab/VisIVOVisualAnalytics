# SED fitting

Fits the spectral energy distribution of a catalogue source (its fluxes
against wavelength) with a dust model, giving mass, dust temperature, the
opacity index β and the bolometric luminosity. Three models are available: an
optically thin modified blackbody, an optically thick one, and a grid of
theoretical models served by the VLKB. The fit runs on the backend.

## Opening a SED

1. Load a catalogue overlay on an image: *Tools ▸ Catalogue ▸ Load Catalogue
   Overlay…* (a file) or *Tools ▸ Catalogue ▸ Overlay VLKB Compact Sources*
   (the Hi-GAL band-merged catalogue over the image footprint).
2. In the catalogue table under the image, right-click a source and choose
   *Fit SED of this source…*.

The SED is built from:

- a band-merged catalogue (VLKB): all the rows of the source, one per band
  (the children under the parent row in the table);
- a flat catalogue: the flux columns of the row (`flux_250`, `F350`,
  `S_870um`, …), one point per recognised column.

Bands with no usable flux (zero, negative, missing, not a number) are left out,
and the log says which and why. Fluxes are in Jy, wavelengths in µm.

### Branches

A band-merged source can have more than one counterpart in one band. The
brightest starts in the fit; the others are shown as alternatives (hollow
markers, greyed rows). Tick an alternative, or click it on the plot, to use it
instead. *Collapse branches* sums the branches of each band into one point
(fluxes added, errors in quadrature, the largest size). A band that mixes a
detection with an upper limit is not collapsed, and the log says so.

A fit needs detections at two different wavelengths at least; upper limits
alone, or two branches of one band, are refused with the reason.

## The window

The plot is on the left, four tabs on the right.

- *Photometry*: one row per measurement (wavelength, flux, error, size,
  designation). *Use* includes it in the fit; *UL* marks the flux as an upper
  limit.
- *Fit*: the model (*Optically thin*, *Optically thick*, *Both (compare)*), the
  *Source and dust* constants (*Distance*, *κ(λ_ref)*, *λ_ref*, *Source size*),
  *Apply PACS colour corrections*, and a *min / max / steps* row per fitted
  parameter.
- *Results*: fitted parameters with uncertainties, χ², reduced χ² and degrees
  of freedom, and the log.
- *Models*: theoretical models from the VLKB ranked by χ²; selecting a row
  plots it.

Under the plot:

- *Log axes*: logarithmic axes (on for a SED spanning decades).
- *Select on plot*: dragging draws a box that selects the points inside it;
  unticked, dragging pans.
- *Collapse branches*: see above.
- *Fit greybody*: runs the thin, thick or both fits.
- *Fit theoretical models*: scores the photometry against the VLKB model grid.
- *Clear fits*: removes the fitted curves.
- *Export…*: CSV with the photometry, the model flux at each band and the fit
  parameters.

On the plot, filled points are in the fit, hollow ones are not; a click fits
that point alone at its wavelength, ⌘/Ctrl-click adds a point. The plot and
the *Photometry* table always agree.

Upper limits (*UL*) are drawn as downward triangles. They are not in the χ²,
but any model brighter than a limit is rejected.

## Optically thin model

F_ν(λ) = M κ_ref / d² · (λ_ref/λ)^β · B_ν(λ, T)

Fitted: mass, dust temperature, β. Constants:

| Setting | Meaning |
|---|---|
| *Distance* | in parsecs. Mass and luminosity scale as d²; the distance error is not in the quoted uncertainties and is usually the largest. |
| *κ(λ_ref)* | opacity per unit mass at λ_ref, cm² g⁻¹, default 0.1 at 300 µm (Hi-GAL). It includes the gas-to-dust ratio, so the mass is gas plus dust. |
| *λ_ref* | wavelength of that opacity, in µm. |
| *β* | fixed at 2 by default (one step). Free it only when sub-mm bands constrain it; otherwise it trades off against temperature. |

## Optically thick model

F_ν(λ) = Ω B_ν(λ, T) (1 − e^(−τ)), τ = (λ₀/λ)^β

Fitted: temperature, β, λ₀ and a *Size scale* on the measured angular size.
The mass is derived from the solid angle, λ₀ and β. *Source size* is the
median of the catalogue's per-band FWHM, in arcseconds, and can be changed in
the *Fit* tab. Without a size there is no thick fit; *Both (compare)* then
runs the thin fit only and says so. The size must be angular: it is never
taken from the drawn ellipse, whose radii are in pixels.

## Theoretical models from the VLKB

*Fit theoretical models* ranks the VLKB's grid of pre-computed models by χ².
Each model gives clump mass, dust temperature, bolometric luminosity,
compact-mass fraction, upper age and ZAMS star content, and its SED is plotted
when you select it. *Band weights* (Mid-IR, Far-IR, Sub-mm), *Pre-filter* and
Δχ² control the ranking. The backend must be configured with the model
service; otherwise the button says that no service is configured.

## Reading the results

Uncertainties are 1σ intervals for one parameter (Δχ² = 1, the other
parameters re-minimised). PACS 70, 100 and 160 µm colour corrections are on by
default; without them the temperature of cold sources is biased.

| Log message | What to do |
|---|---|
| *the Δχ² ≤ 1 region reaches the edge of the searched range for X; the quoted uncertainty is a lower bound* | widen the range, or report the value as a limit |
| *no flux errors were supplied, so χ² is not a statistical quantity* | the best fit stands, but the ± values have no confidence meaning |
| *N detections for M free parameters: the fit is not over-determined* | no reduced χ²; fix a parameter (usually β) or add a band |
| *grid refinement stopped after 10 iterations without the χ² settling* | ranges far too wide, or a SED no greybody fits |
| *mass is derived from the fitted size, λ₀ and β* (thick) | the mass range spans the corners of three profiled intervals, not a Δχ² = 1 interval on the mass |

*Results are out of date* means the photometry or settings changed after the
fit; a fit that was running when you edited is flagged the same way, and an
export then notes that the `fit_*` columns describe the recorded selection.

Every fit is a product in *Session Data*, with the model, parameters,
uncertainties, χ² and the settings used when it was sent.

Details: [SED fitting: methods and implementation](../../technical/methods/sed-fitting.md),
[SED API](../../technical/backend-api/sed-simulations.md).
