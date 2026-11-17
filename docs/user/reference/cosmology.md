# Cosmology tools

The cosmology tools are a distance calculator, the cosmology choice of the 3-D
catalogue viewer, and two texture tools for maps: the angular power spectrum
and the skeleton and void tracer. The backend computes every number with
astropy, except the 3-D catalogue viewer's default Planck18, which the client
integrates itself.

## Cosmology calculator

Open it with *Tools ▸ Cosmology Calculator…* in the main window, or from the
3-D catalogue viewer (*Tools ▸ Cosmology Calculator…* or the *Cosmology
Calculator…* button in its *Cosmology* card, which starts on the model the
sources use).

- *Model*: Planck18, Planck15, Planck13, WMAP9 or *Custom ΛCDM*.
- *H₀*: km/s/Mpc, 40 to 120 (custom model).
- *Ωm*: 0 to 1 (custom model).
- *ΩΛ*: with *Flat* ticked (the default), 1 − Ωm; untick it to set it. An open
  model (Ωm + ΩΛ < 1) is accepted and its Ωk shown; closed models are not.
  De Sitter (Ωm = 0, ΩΛ = 1) has no finite age.
- Redshifts: one or many, separated by commas, spaces or new lines (a pasted
  spreadsheet column works); up to 1000 per run.
- *Compute*: fills the table.
- *Copy*: copies the table, tab-separated, for a spreadsheet.
- *Export CSV…*: writes the table to a file. Both start with a line naming the
  model.
- *Use in 3-D Catalogues*: makes the custom model the *Custom* choice of every
  3-D catalogue window.

| Column | Unit | Planck18 | Planck15, Planck13, WMAP9 | Custom |
|---|---|---|---|---|
| D_C, comoving distance | Mpc | yes | yes | yes |
| D_L, luminosity distance | Mpc | yes | yes | yes |
| D_A, angular-diameter distance | Mpc | z ≤ 20 | no | z ≤ 20 |
| Lookback time | Gyr | z ≤ 20 | no | z ≤ 20 |
| Age of the universe at z | Gyr | z ≤ 20 | no | z ≤ 20 |
| Proper scale | kpc/arcsec | yes | yes | yes, from D_A |

A refused redshift keeps its row, with the reason in the *Note* column.

A custom model has no radiation (astropy `LambdaCDM` with `Tcmb0 = 0`), while
the presets include radiation and neutrinos. A custom model with Planck18's H₀
and Ωm therefore gives distances 0.1 to 0.2 % larger and ages up to 1 % larger
than the preset.

## Cosmology of a 3-D catalogue

In the 3-D catalogue viewer, *Model* in the *Cosmology* card sets how
redshifts become comoving distances: Planck18 (computed locally), Planck15,
Planck13, WMAP9 (computed by the backend) or *Custom* (the calculator's custom
model). The status line under it names the model the sources on screen use.

Sources beyond z = 20 under *Custom* are placed with their catalogue distance,
or with Planck18, and are counted separately in the status line. The
ray-traced view and the backend-drawn point view know only the presets:
turning either on under *Custom* switches back to Planck18 and says so.

## Angular power spectrum

- Image viewer: *Tools ▸ Structure ▸ Angular Power Spectrum*.
- Cube viewer: *Tools ▸ Maps ▸ Angular Power Spectrum of Moment Map*, on the
  moment map on screen (or the last one computed).

Settings in *Inspector ▸ Parameters*: *Bins*, and *Compute P(ℓ)*. The result
window shows:

- the spectrum on log axes with error bars; bins used in the fit in blue,
  others grey;
- the slope n of P ∝ ℓⁿ with its 1σ error and fitted ℓ range;
- *Fit ℓ from … to* and *min. modes*: refit the slope. Each bin is weighted
  by 1/σ² of log P, so the many-mode bins at high ℓ dominate. Bins with fewer
  than 8 Fourier modes or no error are left out by default;
- χ²/dof of the fit. When it rules out a single power law (p < 0.001) the
  slope line says so, the 1σ error is widened by √(χ²/dof), and the line
  follows the high-ℓ bins: narrow the range to the part that is straight;
- CSV and PNG export.

This is a relative texture spectrum, with no beam, pixel-window or mask
correction: use it to compare maps, and keep the fit range clear of the beam
fall-off and any noise floor at high ℓ. Without a celestial WCS the axis is the
wavenumber k in cycles per pixel.

## Skeleton and voids

- Image viewer: *Tools ▸ Structure ▸ Skeleton / Voids*.
- Cube viewer: *Tools ▸ Maps ▸ Skeleton / Voids of Moment Map*.

Settings in *Inspector ▸ Parameters*:

- *Trace*: *Filaments* (skeleton of the regions above the threshold) or
  *Voids* (skeleton of the regions below it).
- *Threshold*: with *Mean of the map* ticked (the default), the map's mean;
  otherwise a value in the map's units.
- *Smoothing σ*: Gaussian pre-smoothing in pixels; blank pixels stay blank.
- *Drop where map <* (*Skeleton pixels*): drops skeleton pixels below this
  intensity, filaments only. It can split a filament. Zero and negative values
  are valid cuts.
- *Trace* (button): runs it. On an image the skeleton is drawn over the map,
  cyan for filaments and magenta for voids; *Clear* removes it. On a cube the
  mask (1 on the skeleton) opens as a new map in a pane.

## Products

Each run is a product in *Session Data*, labelled with its result and time
(for example `n = -3.64 · 14:32:05 · Moment 0`). Opening the row shows the
plot, overlay or mask again; *Inspector ▸ Parameters* shows the parameters,
including the threshold applied and the fit range. On a cube, the exported
recipe first writes the moment map to Workspace Exports and opens it, then
runs the analysis.

The [Scientific Copilot](copilot.md) can also compute distances, spectra and
skeletons.

Details: [Cosmology: methods and implementation](../../technical/methods/cosmology.md)
and the [cosmology API](../../technical/backend-api/cosmology-analysis.md).
