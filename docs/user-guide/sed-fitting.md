# SED fitting

A compact source in a band-merged catalogue has a flux in every band it was
detected in. Plotted against wavelength that is a **spectral energy
distribution**, and fitting a dust model to it turns a list of fluxes into
physical quantities: the mass of the clump, the temperature of its dust, how
steeply the opacity falls with wavelength, and the bolometric luminosity of
whatever is heating it.

VisIVO fits the SED of any source in a loaded catalogue overlay. Three models
are available: an optically thin modified blackbody, an optically thick one, and
a grid of pre-computed theoretical models served by the VLKB.

```{note}
The fit runs on the **backend**, not in the desktop client
(`POST /v1/sed/fit`). Anything you can do from the window you can also do from
the API, and two people fitting the same photometry get the same answer whatever
is installed on their laptops.
```

## Opening a SED

1. Load a catalogue overlay on an image — either from a file
   (*Tools → Load Catalogue Overlay*) or from the VLKB
   (*Tools → Overlay VLKB Compact Sources*, which queries
   `compactsources.sed_view_final` over the image footprint).
2. In the catalogue table at the bottom of the window, **right-click a source**
   → *Fit SED of this source…*.

What ends up in the SED depends on the catalogue's shape, and VisIVO recognises
both:

- **Band-merged (VLKB)** — one physical source is several rows, one per band,
  tied together by the band-merged designation. The whole family becomes one
  SED: right-clicking any band gives you all of them. In the table these rows
  are the children under the disclosure arrow of the band-merged parent.
- **Flat** — one row carrying several flux columns (`flux_250`, `F350`,
  `S_870um`…). Each recognised column becomes one point of the SED.

Bands with no usable flux — zero, negative, `missing`, or a value that is not a
number — are left out, and the window says which ones and why. A missing band
and a band that failed to parse look the same on a plot; they should not look
the same in the log.

### Branches

A band-merged source can have **more than one counterpart in the same band**:
the association is positional, and two sources at 250 µm can both fall inside
the band-merged ellipse. Those are the SED's *branches*.

VisIVO keeps them all. The brightest of a band starts in the fit, the others sit
next to it as alternatives — hollow markers on the plot, greyed rows in the
table — so the choice is visible and yours. Silently keeping the brightest is
still a choice; it is just one made on your behalf and never shown.

Two ways to resolve a branch:

- **Pick one** — tick the alternative you want (or click it on the plot). The
  band contributes that measurement.
- **Collapse branches** — the checkbox under the plot sums the branches of each
  band into a single point: fluxes added, errors in quadrature, size taken from
  the largest. This is the legacy tool's *collapse*, and it answers the other
  reading of the same data: the source is all of the counterparts together.

A band whose branches mix a detection with an upper limit is **not** collapsed —
adding 10 Jy to a "< 5 Jy" would hand the fit a measured 15 Jy nobody observed.
Those branches stay separate and the log says where and why.

```{tip}
A SED fit needs detections at **two different wavelengths**. Upper limits
constrain the fit but cannot define it, and two branches of one band are two
measurements of the same point on the spectrum — neither case gives a curve to
fit, and both are refused with that explanation rather than fitted to something
meaningless.
```

## The window

The plot is on the left, four tabs on the right.

```{list-table}
:header-rows: 1
:widths: 18 82

* - Tab
  - What it holds
* - **Photometry**
  - One row per measurement: wavelength, flux, error, angular size, per-band
    designation, and two checkboxes. **Use** includes it in the fit; **UL**
    declares its flux an upper limit. A band with several counterparts has
    several rows — see *Branches*.
* - **Fit**
  - The model (thin / thick / both), the source and dust constants (distance,
    κ at a reference wavelength, that reference wavelength, source size), the
    PACS colour-correction switch, and one *min / max / steps* row per fitted
    parameter.
* - **Results**
  - Fitted parameters with their uncertainties, χ² / χ²ᵣ / dof, and a log of
    everything the fit wants you to know (see *Reading the warnings*).
* - **Models**
  - Theoretical models from the VLKB grid, ranked by χ². Selecting a row
    overplots it.
```

Below: **Log axes** (a SED spans four decades in both directions — linear axes
are almost never what you want), **Select on plot**, **Collapse branches**,
**Fit greybody**, **Fit theoretical models**, **Clear fits**, and **Export…**.

### Choosing the bands on the plot

The plot is not a picture of the fit input — it *is* the input. What is filled
and bright is in the fit; what is hollow and dim is not, and a dashed guide line
joins the points that are.

- **Click** a point to fit that one alone at its wavelength.
- **⌘/Ctrl-click** to add a point to the selection.
- Tick **Select on plot** and **drag** a rectangle to take in everything inside
  it. With the box unticked, dragging pans the plot instead — the two gestures
  share a mouse button, so one of them has to be asked for.

The Photometry table and the plot are the same state seen twice: a tick in one
moves the other.

### Upper limits

Ticking **UL** changes what the band means. It is removed from the χ² — a
non-detection is not a measurement, and scoring a model against it would pull the
fit towards a flux nobody observed — but any model brighter than the limit is
**rejected outright**. That is the whole point of an upper limit: it says the
source is not brighter than this, and models that violate it are wrong.

Upper limits are drawn as downward triangles, never as discs, so a glance at the
plot tells you which points are measurements.

## The optically thin model

The standard modified blackbody for cold dust:

$$F_\nu(\lambda) = \frac{M\,\kappa_\mathrm{ref}}{d^2}
  \left(\frac{\lambda_\mathrm{ref}}{\lambda}\right)^{\beta} B_\nu(\lambda, T)$$

Three fitted parameters — **mass**, **dust temperature**, **β** — plus four
constants you supply:

```{list-table}
:header-rows: 1
:widths: 26 74

* - Setting
  - What it is
* - **Distance**
  - Distance to the source in parsecs. Mass scales as $d^2$ and luminosity as
    $d^2$: a distance wrong by a factor 2 is a mass wrong by a factor 4. This is
    almost always the dominant uncertainty, and it is *not* included in the
    quoted errors.
* - **κ(λ_ref)**
  - Dust opacity per unit mass at the reference wavelength, in cm² g⁻¹.
    Default 0.1 at 300 µm, the usual Hi-GAL choice. It already includes the
    gas-to-dust ratio, so the mass that comes out is a *total* (gas + dust) mass.
* - **λ_ref**
  - Where that opacity is quoted. Changing it without changing κ changes the
    mass.
* - **β**
  - The opacity spectral index. Fixed at 2 by default (one step). Free it only
    when the SED has enough sub-mm coverage to constrain it — otherwise it trades
    off against temperature and you get a well-fitted meaningless answer.
```

## The optically thick model

Below some wavelength λ₀ the dust becomes optically thick and the source
radiates as a blackbody of its own solid angle:

$$F_\nu(\lambda) = \Omega\, B_\nu(\lambda, T)
  \left(1 - e^{-\tau}\right), \qquad \tau = (\lambda_0/\lambda)^{\beta}$$

Fitted: **temperature**, **β**, **λ₀**, and a **size scale** multiplying the
measured angular size. Mass is not fitted here — it is *derived* from the solid
angle, λ₀ and β.

**Source size** is taken from the catalogue's per-band FWHM (the median over the
bands in use) and shown in the *Fit* tab, where you can override it. Without a
size there is no solid angle and no thick fit; asking for *both* then simply
skips the thick model and says so, rather than failing the whole request.

```{warning}
The size must be an **angular** size in arcsec. VisIVO takes it from the
catalogue's FWHM columns, never from the drawn ellipse — the ellipse radii are
in pixels, and using them would scale the solid angle, and therefore the mass,
by the pixel scale squared.
```

## Theoretical models from the VLKB

*Fit theoretical models* scores your photometry against a grid of pre-computed
models held by the VLKB and returns them ranked by χ². Each model carries its
physical parameters — clump mass, dust temperature, bolometric luminosity,
compact-mass fraction, upper age, ZAMS star content — and its own sampled SED,
which is overplotted when you select the row.

The **band weights** (mid-IR / far-IR / sub-mm), the pre-filter and Δχ² are
passed through to the service and control how it ranks models.

The service is reached through the backend, not from the desktop client:

- the odd underscore-joined query format lives in one place;
- the URL is validated against server-side request forgery, on the first request
  *and on every redirect*, so a public service cannot bounce the backend onto a
  private address;
- a deployment configures it once with `VISIVO_VLKB_SEDFIT_URL`, and the client
  picks it up from `GET /v1/sed/defaults`.

If no service is configured the button reports that, rather than failing
obscurely.

## Reading the warnings

The *Results* log is where the fit tells you how much to trust it. All of these
are worth acting on:

```{list-table}
:header-rows: 1
:widths: 40 60

* - Warning
  - What to do
* - *the Δχ² ≤ 1 region reaches the edge of the searched range for X; the quoted
    uncertainty is a lower bound*
  - The data do not bound that parameter inside the range you gave. Widen the
    range, or report the value as a limit.
* - *no flux errors were supplied, so χ² is not a statistical quantity*
  - The catalogue had no error columns. The fitted parameters are still the
    best-fitting ones, but the ± column has no confidence-level meaning.
* - *N detections for M free parameters: the fit is not over-determined*
  - There is no reduced χ² (dof ≤ 0), and the table says so instead of printing
    a number. Fix a parameter (β is the usual candidate) or add a band.
* - *grid refinement stopped after 10 iterations without the χ² settling*
  - The search ran out of iterations. Usually a symptom of ranges that are far
    too wide, or of a SED no greybody can fit.
* - *mass is derived from the fitted size, λ₀ and β* (thick only)
  - The mass range quoted is the spread over the corners of three separately
    profiled intervals, not a Δχ² = 1 interval on the mass itself.
```

Two more things the window will tell you, in the status line and the log:

- **Results are out of date** — you changed the photometry or the parameters
  after the fit ran. The curves on screen belong to the previous selection.
- The same, detected at completion: if you edit while a fit is in flight, the
  answer that arrives describes what you *had* selected, and is flagged.

## How the fit actually works

Grid search with iterative refinement, inherited from the ViaLactea engines
(D. Elia) but reworked:

1. The parameter ranges are evaluated as a grid.
2. Refinement starts from **several separated minima** of that grid, not only
   its lowest cell. On a four-parameter thick fit, zooming from the single best
   coarse cell reliably converges into the wrong basin.
3. Each step narrows every axis around the best model found *so far* — a step
   that lands on a shifted grid can lower the resolution but never the quality
   of the answer — and can push a boundary outwards when the optimum sits on it.
4. It stops when the axes are resolved, or when two consecutive steps find
   nothing better.

Uncertainties are a **profile-likelihood Δχ² = 1** search: each parameter is
scanned outwards from the best fit with the others re-minimised at every step,
and the crossing is bracketed and bisected. They are therefore ordinary 1σ
intervals for one parameter — not the width of the final grid, which measures
the zoom rather than the data.

Colour corrections for PACS 70, 100 and 160 µm are applied by default,
interpolated in temperature. PACS fluxes are calibrated for a νF_ν = constant
source; a 12 K greybody needs a factor of roughly 2 at 70 µm, so leaving them
off biases the temperature.

## What you get back

Every fit is registered as a product in **Session Data**, carrying the model,
the parameters and their uncertainties, χ² / dof, and the exact settings that
produced it — the snapshot taken when the request was sent, not whatever the
spin boxes say later.

**Export…** writes a CSV with the photometry, the model flux at each band, the
fit parameters in the header, and — if you changed the selection after fitting —
an explicit note saying the `fit_*` columns describe the recorded selection
rather than the current one.

## See also

- [Catalogues, HiPS, and SAMP](catalogues-hips) — loading the overlay the SED
  comes from, and the band-merged table.
- [Image viewer](image-viewer) — the window the catalogue is overlaid on.
- [Backend API](../backend-api) — `/v1/sed/fit`, `/v1/sed/models`,
  `/v1/sed/defaults`.
