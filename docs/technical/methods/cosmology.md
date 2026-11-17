# Cosmology tools: methods and implementation

User reference: [Cosmology tools](../../user/reference/cosmology.md).
Routes: [Cosmology and map analysis API](../backend-api/cosmology-analysis.md).

## Where the numbers come from

Every number is computed by the backend with astropy. The client does not
integrate a Friedmann equation, with one exception: the 3-D catalogue viewer
integrates its default Planck18 locally (see
[3-D catalogue viewer design](../design/catalogue3d-viewer.md)).

The calculator leaves D_A, lookback time and age empty for Planck15, Planck13
and WMAP9 because no backend route computes them for those presets, and the
calculator does not fill the gap with a computation of its own. The proper
scale for a custom model is the returned D_A expressed per arcsecond, the same
quantity as astropy's `kpc_proper_per_arcmin`.

## Custom model versus the presets

A custom model is astropy's `LambdaCDM(H0, Om0, Ode0)` with `Tcmb0 = 0`: it
has matter, Λ and curvature, but no radiation. The presets include radiation
and neutrinos. So a custom model with Planck18's H₀ and Ωm does not reproduce
the Planck18 preset: its distances are 0.1 to 0.2 % larger and its ages up to
1 % larger at z ≤ 20. Typing Planck18's ΩΛ = 0.6889 instead of using *Flat*
gives a slightly open model (Ωk ≈ 0.0015): the missing radiation density turns
into curvature.

De Sitter (Ωm = 0, ΩΛ = 1) is computed; it has no Big Bang, so its age column
stays empty and the row's note says the age is not finite. Closed models
(Ωm + ΩΛ > 1) are not supported by the server.

The export header line names the model, for example
`# cosmology: Custom LambdaCDM (H0=70 km/s/Mpc, Om0=0.3, Ode0=0.7, Tcmb0=0, flat)`.

## Custom model in a 3-D catalogue

The route that takes a custom model answers one redshift per request. So:

- a catalogue with up to 128 distinct redshifts asks for each one;
- a larger catalogue asks for 128 nodes spread evenly in ln(1 + z) across its
  range, and interpolates D_C/z between them. The error is below 10⁻⁴
  (measured against astropy for flat, open and matter-only models).

The route accepts z ≤ 20. A source beyond that is placed with its catalogue
distance, or with Planck18 if it has none. Planck15, Planck13 and WMAP9 are
computed in one batch request.

The ray-traced view and the server-side point view are drawn by the backend,
which knows only the four presets; while one of them is active, *Custom* cannot
be chosen.

## Map tools on a cube: recipe and moment export

A cube's moment map is not a dataset the backend can read, so on a cube the
recipe starts with two extra steps:

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
run writes its own. The recipe and the provenance name the same file.

## Skeleton details

The skeleton is the morphological skeleton (thinning) of the regions above
(filaments) or below (voids) the threshold. The Gaussian pre-smoothing makes
each pixel the weighted mean of the valid pixels around it; blank (NaN) pixels
stay blank, so no filament or void is traced inside an unobserved area. The
*Drop where map <* cut acts on individual skeleton pixels; it is not a
topological-persistence cut, so it can split a filament and add end points.

## Routes and desktop entries

| Route | Computes | Desktop entry |
|---|---|---|
| `POST /v1/cosmology/distance` | D_C, D_L and the proper scale for one redshift, under a preset | Cosmology Calculator (presets) |
| `POST /v1/cosmology/distance/batch` | D_C for up to 100 000 redshifts, under a preset | 3-D catalogue viewer, Model = Planck15 / Planck13 / WMAP9 |
| `GET /v1/cosmology/distances` | D_C, D_L, D_A, lookback time and age for one redshift, under Planck18 or a custom ΛCDM | Cosmology Calculator (Planck18's D_A, lookback time and age; every custom model); 3-D catalogue viewer, Model = Custom |
| `POST /v1/cosmology/angular_power/{id}` | Angular power spectrum of a plane | Tools ▸ Structure ▸ Angular Power Spectrum (image); Tools ▸ Maps ▸ Angular Power Spectrum of Moment Map (cube) |
| `GET /v1/cosmology/powerspectrum/{id}` | The same spectrum, same worker | None: an alias of the POST above |
| `POST /v1/analysis/skeleton/{id}` | Skeleton or void mask, with the threshold applied | Tools ▸ Structure ▸ Skeleton / Voids (image); Tools ▸ Maps ▸ Skeleton / Voids of Moment Map (cube) |
| `POST /v1/analysis/skeleton/{id}/binary` | The same mask as a binary frame | None: it does not report the threshold the server chose, which the product's record needs |
| `POST /v1/cosmology/skeleton/{id}` | The same as `/v1/analysis/skeleton` | None: an alias kept for the cosmology grouping |
