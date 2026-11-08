# Cosmology tools

Three tools work on cosmology: a calculator, the cosmology choice of the 3-D
catalogue viewer, and two texture tools for maps (the angular power spectrum
and the skeleton / void detector). Every number they show is computed by the
backend with astropy. The client does not integrate a Friedmann equation, with
one older exception: the 3-D catalogue viewer integrates its default Planck18
locally (see [Catalogues & HiPS](catalogues-hips.md#cosmology-models)).

## Cosmology calculator

Open it from **Tools ▸ Cosmology Calculator…** in the main window. You can also
open it from the 3-D catalogue viewer, through **Tools ▸ Cosmology Calculator…**
or the button in its *Cosmology* card. Opened from the catalogue viewer, it
starts on the model the sources are placed with.

1. Pick a **Model**. The choices are Planck18, Planck15, Planck13, WMAP9, or
   **Custom ΛCDM**.
2. For a custom model, set the parameters:
   - **H₀** in km/s/Mpc, from 40 to 120.
   - **Ωm**, from 0 to 1.
   - **ΩΛ**. *Flat* is ticked by default and sets ΩΛ = 1 − Ωm. Untick it to
     set ΩΛ yourself.

   A model with Ωm + ΩΛ < 1 is open. The window says so in words, and gives
   Ωk. The server does not support closed models (Ωm + ΩΛ > 1). It also does
   not support Ωm = 0 with ΩΛ = 1 (de Sitter), whose age is infinite.
3. Type one or many **redshifts**, separated by commas, spaces or new lines. A
   column pasted from a spreadsheet also works. One run takes up to 1000
   redshifts.
4. Press **Compute**.

The output columns:

| Column | Unit | Planck18 | Planck15 / Planck13 / WMAP9 | Custom |
|---|---|---|---|---|
| D_C, comoving distance | Mpc | ✓ | ✓ | ✓ |
| D_L, luminosity distance | Mpc | ✓ | ✓ | ✓ |
| D_A, angular-diameter distance | Mpc | ✓ (z ≤ 20) | — | ✓ (z ≤ 20) |
| Lookback time | Gyr | ✓ (z ≤ 20) | — | ✓ (z ≤ 20) |
| Age of the universe at z | Gyr | ✓ (z ≤ 20) | — | ✓ (z ≤ 20) |
| Proper scale | kpc/arcsec | ✓ | ✓ | ✓, from D_A |

The empty cells (—) are deliberate. No backend route computes D_A, lookback
time or age for Planck15, Planck13 or WMAP9, and the calculator does not fill
the gap with a computation of its own. The proper scale for a custom model is
the returned D_A expressed per arcsecond, the same quantity astropy's
`kpc_proper_per_arcmin` gives.

```{note}
A custom model is astropy's `LambdaCDM(H0, Om0, Ode0)` with `Tcmb0 = 0`: it
has matter, Λ and curvature, but no radiation. The presets include radiation
and neutrinos. So a custom model with Planck18's H₀ and Ωm does not reproduce
the Planck18 preset: its distances are 0.1–0.2 % larger and its ages up to
1 % larger at z ≤ 20. If you type Planck18's ΩΛ = 0.6889 instead of using
*Flat*, the model comes out slightly open (Ωk ≈ 0.0015): the missing radiation
density turns into curvature.
```

**Copy** puts the table on the clipboard, tab-separated, so it pastes into a
spreadsheet as columns. **Export CSV…** writes the same table as a file. Both
start with a line naming the model, for example
`# cosmology: Custom LambdaCDM (H0=70 km/s/Mpc, Om0=0.3, Ode0=0.7, Tcmb0=0, flat)`.
A refused redshift keeps its row, with the server's reason in the *Note*
column.

## The cosmology of a 3-D catalogue

In the 3-D catalogue viewer, the **Model** selector in the *Cosmology* card
sets how redshifts become comoving distances:

- **Planck18** is integrated locally.
- **Planck15**, **Planck13** and **WMAP9** are computed by the backend in one
  batch request.
- **Custom** uses the custom model from the calculator. To set it, define the
  model in the calculator and press **Use in 3-D Catalogues**. Every catalogue
  window already on Custom then re-places its sources.

The status line under the selector names the model the sources on screen were
placed with. For a custom model this is the full description, with H₀, Ωm, ΩΛ
and the curvature.

How a custom model is computed. The backend route that takes a custom model
answers one redshift per request. So:

- A catalogue with up to 128 distinct redshifts asks for each one.
- A larger catalogue asks for 128 nodes spread evenly in ln(1 + z) across its
  range, and interpolates D_C/z between them. The error is below 10⁻⁴
  (measured against astropy for flat, open and matter-only models).

The route accepts z ≤ 20. A source beyond that is placed with its catalogue
distance, or with Planck18 if it has none, and the status line counts these
sources separately.

The ray-traced view and the server-side point view are drawn by the backend,
which knows only the four presets. Turning on either of them under Custom puts
the model back to Planck18, and says so. While one of them is active, Custom
cannot be chosen.

## Angular power spectrum

**Image viewer:** *Tools ▸ Structure ▸ Angular Power Spectrum*.
**Cube viewer:** *Tools ▸ Maps ▸ Angular Power Spectrum of Moment Map*, on the
moment map on screen (or the moment computed last).

Turning the tool on shows its settings in **Inspector ▸ Parameters**: the
number of bins, and **Compute P(ℓ)**. The result opens in a window with these
parts:

- **The plot.** The spectrum is drawn on log axes with its error bars. The
  bins used in the fit are blue, the bins left out are grey.
- **The fit.** The slope n of P ∝ ℓⁿ comes with its 1σ error and the ℓ range
  it was fitted over.
- **The fit controls.** *Fit ℓ from … to* and *min. modes* refit the slope as
  you change them. The fit is weighted by the per-bin errors. Bins with fewer
  than 8 Fourier modes, or with no error estimate, are left out by default.
- **Export.** CSV and PNG export.

It is a **relative texture spectrum**. It has no beam, pixel-window or mask
deconvolution, so use it to compare maps, not as a calibrated Cℓ. Restrict the
fit range to exclude the beam and pixel-window fall-off, and any noise floor,
at high ℓ.

When a map has no celestial WCS, the axis is the wavenumber k in cycles per
pixel, and the plot says so.

## Skeleton and voids

**Image viewer:** *Tools ▸ Structure ▸ Skeleton / Voids*.
**Cube viewer:** *Tools ▸ Maps ▸ Skeleton / Voids of Moment Map*.

The settings, in **Inspector ▸ Parameters**:

- **Trace.**
  - *Filaments* gives the morphological skeleton (thinning) of the regions
    above the threshold.
  - *Voids* gives the skeleton of the regions below it.
- **Threshold.** By default the server uses the mean of the map. The value it
  used is recorded with the result.
- **Smoothing σ.** A Gaussian pre-smoothing, in pixels. Each pixel becomes
  the weighted mean of the valid pixels around it; blank (NaN) pixels stay
  blank, so no filament or void is traced inside an unobserved area.
- **Drop where map <.** An intensity cut on individual skeleton pixels. It is
  not a topological-persistence cut: it can split a filament and add end
  points. It applies to filaments only, and values ≤ 0 are ignored by the
  server.

The result is shown in two ways:

- **On an image**, the skeleton is drawn over the map, cyan for filaments and
  magenta for voids. **Clear** removes it from the view.
- **On a cube**, the mask (1 on the skeleton) opens as a new map in a pane.

## Products and provenance

Each run is a product in **Session Data**, labelled with its result and finish time, for example `n = -3.64 · 14:32:05 · Moment 0` or `Filaments · 412 px · 14:32:05`. Opening its row puts the plot, the
overlay or the mask back on screen. **Inspector ▸ Provenance** shows its
parameters, including the threshold actually applied and the fit range of the
slope.

The recipe export lists the service calls that reproduce the run, with their
requests under the API's own field names. A cube's moment map is not a
dataset the backend can read, so on a cube the recipe starts with two extra
steps:

1. Write the moment to the Workspace Exports (`POST /v1/tasks/moment_fits`),
   at full resolution.
2. Open that file (`POST /v1/datasets/open`).

The analysis step follows. The file is named
`<cube>_m<order>_<digest>_maptools.fits`. The digest covers the cube's path,
its size and modification time on the backend, and the moment's recipe.

A later run on the same moment of the same file opens that file instead of
writing a new one, in this session or a later one. A cube replaced at the same
path gets a new name, so an old moment is never reused for it. An export that
is still being written, or that does not open, is never reused or deleted: the
run writes its own. The recipe and Provenance name the same file.

## What each route is for

Every cosmology and cosmology-analysis route of the backend, and where the
desktop reaches it:

| Route | Computes | Desktop entry |
|---|---|---|
| `POST /v1/cosmology/distance` | D_C, D_L and the proper scale for one redshift, under a preset | Cosmology Calculator (presets) |
| `POST /v1/cosmology/distance/batch` | D_C for up to 100 000 redshifts, under a preset | 3-D catalogue viewer, Model = Planck15 / Planck13 / WMAP9 |
| `GET /v1/cosmology/distances` | D_C, D_L, D_A, lookback time and age for one redshift, under Planck18 or a custom ΛCDM | Cosmology Calculator (Planck18's D_A, lookback time and age; every custom model); 3-D catalogue viewer, Model = Custom |
| `POST /v1/cosmology/angular_power/{id}` | Angular power spectrum of a plane | Tools ▸ Structure ▸ Angular Power Spectrum (image); Tools ▸ Maps ▸ Angular Power Spectrum of Moment Map (cube) |
| `GET /v1/cosmology/powerspectrum/{id}` | The same spectrum, same worker | No entry of its own: it is an alias of the POST above, which the desktop uses |
| `POST /v1/analysis/skeleton/{id}` | Skeleton or void mask, with the threshold applied | Tools ▸ Structure ▸ Skeleton / Voids (image); Tools ▸ Maps ▸ Skeleton / Voids of Moment Map (cube) |
| `POST /v1/analysis/skeleton/{id}/binary` | The same mask as a binary frame | No entry of its own: same computation, and it does not report the threshold the server chose, which the product's record needs |
| `POST /v1/cosmology/skeleton/{id}` | The same as `/v1/analysis/skeleton` | No entry of its own: an alias kept for the cosmology grouping |

The Copilot reaches the distances, the spectrum and the skeleton too (see
[Scientific Copilot](copilot.md)).
