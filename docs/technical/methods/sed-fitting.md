# SED fitting: methods and implementation

User reference: [SED fitting](../../user/reference/sed-fitting.md). Routes:
[SED fitting and simulations](../backend-api/sed-simulations.md).

The fit runs on the backend (`POST /v1/sed/fit`), so the result does not depend
on what is installed on the desktop, and anything the window does can be done
from the API.

## Building the SED

- Band-merged (VLKB) catalogues: one physical source is several rows, one per
  band, tied together by the band-merged designation. The whole family is one
  SED; right-clicking any band gives all of them.
- Flat catalogues: each recognised flux column of the row (`flux_250`,
  `F350`, `S_870um`, …) is one point.

Bands with zero, negative, missing or unparsable fluxes are dropped and listed
with the reason, so a missing band and a band that failed to parse are told
apart in the log.

### Branches

The band-merged association is positional, so two sources in one band can
both fall inside the merged ellipse. All branches are kept: the brightest of a
band starts in the fit and the others are shown as alternatives, so the
choice is visible. *Collapse branches* sums the branches of each band (fluxes
added, errors in quadrature, size from the largest), which is the legacy
tool's *collapse*: the source is all the counterparts together. A band whose
branches mix a detection with an upper limit is not collapsed: adding 10 Jy to
"< 5 Jy" would give a measured 15 Jy nobody observed.

A fit needs detections at two different wavelengths. Upper limits constrain a
fit but cannot define it, and two branches of one band are two measurements of
the same point; both cases are refused.

### Upper limits

An upper limit is removed from the χ² (scoring a model against a non-detection
would pull the fit towards a flux nobody observed), and any model brighter
than the limit is rejected outright.

## Models

Optically thin:

$$F_\nu(\lambda) = \frac{M\,\kappa_\mathrm{ref}}{d^2}
  \left(\frac{\lambda_\mathrm{ref}}{\lambda}\right)^{\beta} B_\nu(\lambda, T)$$

Optically thick, below λ₀:

$$F_\nu(\lambda) = \Omega\, B_\nu(\lambda, T)
  \left(1 - e^{-\tau}\right), \qquad \tau = (\lambda_0/\lambda)^{\beta}$$

In the thick model the solid angle comes from the source size (median per-band
FWHM in arcsec) times the fitted size scale, and the mass is derived from Ω,
λ₀ and β. The size is taken from the catalogue's FWHM columns, never from the
drawn ellipse: its radii are in pixels, and using them would scale the solid
angle, and the mass, by the pixel scale squared. When `model` is `both` and
there is no size, the thick model is skipped with a warning rather than failing
the request.

## Fitting algorithm

Grid search with iterative refinement, from the ViaLactea engines (D. Elia),
reworked:

1. The parameter ranges are evaluated as a grid.
2. Refinement starts from several separated minima of that grid, not only its
   lowest cell. On a four-parameter thick fit, zooming from the single best
   coarse cell reliably converges into the wrong basin.
3. Each step narrows every axis around the best model found so far (a step on
   a shifted grid can lower the resolution but never the quality of the
   answer), and can push a boundary outwards when the optimum sits on it.
4. It stops when the axes are resolved, or when two consecutive steps find
   nothing better, with at most 10 iterations.

Uncertainties are a profile-likelihood Δχ² = 1 search: each parameter is
scanned outwards from the best fit with the others re-minimised at every step,
and the crossing is bracketed and bisected. They are 1σ intervals for one
parameter, not the width of the final grid (which measures the zoom, not the
data). For the thick model the mass range is the spread over the corners of
the three profiled intervals of size, λ₀ and β.

PACS 70, 100 and 160 µm colour corrections are applied by default,
interpolated in temperature. PACS fluxes are calibrated for a νF_ν = constant
source; a 12 K greybody needs a factor of roughly 2 at 70 µm.

## Theoretical models

*Fit theoretical models* proxies the VLKB model service through the backend.
The band weights (mid-IR, far-IR, sub-mm), the pre-filter and Δχ² are passed
through to the service. The backend keeps the service's query format in one
place, validates the URL against server-side request forgery on the first
request and on every redirect, and reads the URL from
`VISIVO_VLKB_SEDFIT_URL`; the client gets it from `GET /v1/sed/defaults`.

## Provenance

Each fit is registered in Session Data with the model, parameters,
uncertainties, χ² and dof, and the settings snapshot taken when the request was
sent. If the selection changed while a fit was in flight, the arriving result
is flagged as describing the earlier selection, and the CSV export says that
its `fit_*` columns describe the recorded selection.
