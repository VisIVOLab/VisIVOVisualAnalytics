# Kinematic Lasso: methods and implementation

User reference: [Kinematic Lasso](../../user/reference/kinematic-lasso.md).
The cost function, the geodesic level field and the backend routes are in the
design note [Kinematic Lasso](../design/kinematic-lasso.md); this page keeps the
validation results and the record formats that the user reference leaves out.

## Choosing Reach

The default of 8 suits compact, well-detected sources. An extended,
low-surface-brightness object may need 30 or more.

Across ten sources on two HI cubes the best value ranged from 4 to 64, and it
tracks the source's surface brightness relative to the noise, not the beam, the
pixel scale or the source's size. Two rules for deriving it automatically were
tested and both performed worse than leaving the default alone. Deriving it from
the header does not work either: the two cubes have near-identical beams and
their best Reach differs eightfold.

Raising Reach can only add voxels. If the surface stops growing well before
expected, the grow has reached the edge of the connected component rather than
the budget: the object is separated from the rest by sub-nσ voxels, and lowering
nσ is what would join them.

Dragging Reach needs no request to the backend: the geodesic field is computed
once per click and every budget is a threshold on it. Changing λ_v alters the
cost function and recomputes.

## Smoothing

A per-voxel threshold does not see a faint envelope that a smoothed one does.
The kernels are, in voxels (velocity, y, x): Light (1, 3, 3), Medium (2, 4, 4),
Strong (2, 6, 6). On the extended disc of NGC 3137, Strong takes a default click
from 68 909 to 319 793 voxels. The mask is applied to the raw cube.

The largest kernel is two channels by six pixels, so smoothing extends the
envelope of a detected source; it does not make the lasso a detector for
sources visible only after smoothing over many channels.

## Noise scale

σ is the session's noise estimate (*Tools ▸ Estimate Noise*) when there is one,
otherwise the cube's own robust σ. The panel's *Noise σ* line says which, and
Isolate and Remove record it at full precision.

## Graph budget and domain trimming

A source larger than the grow's graph budget (four million nodes) is cut to a
seed-centred box; the selection is then exact within that box only. The status
bar says so, the product's Provenance carries a *Domain trimmed* line, and the
FITS `HISTORY` a `domain_trimmed=` card, with the largest box half-width, so a
reader of the record knows the selection was bounded by the cap and not by the
emission.

## Recipe record

Isolate and Remove write the mask exactly as displayed. The product's
Provenance lists the seed, Reach, velocity coupling, threshold, smoothing, the
number of selected voxels and whether the mask was refined by hand; the same
recipe goes into the FITS `HISTORY` of the new cube, one card per item
(`Lasso reach=8`, …).

*Export recipe as JSON…* writes the record under `"schema": "visivo-recipe/1"`:

- the product: its id, kind and the calls that replay it, in order.
  `/v1/cube/kinematic_select_bin` regrows the selection from the seed and
  parameters, giving the same voxels the slider's level field gave, because the
  replay passes the recorded *Level-field budget* as `reach_max`;
  `/v1/cube/mask_apply_bin` makes the cube. Each call carries its `request` as
  the service takes it, under the API's field names, with numbers as numbers and
  seeds as `[z, y, x]`, so a script posts it back without parsing the labels.
  `body` names what travels as bytes: the float32 mask the grow returns, posted
  to the apply with the origin and dimensions of that response, which the record
  describes but does not carry. The request recorded is the one the mask on
  screen was grown with, not the sliders' values at the time of the apply;
- the dataset it was made from: name, path, the backend's dataset and session
  ids (a replay opens the path again and uses the dataset id that open returns);
- the parameters as the Provenance tab shows them (extra seeds as coordinates,
  the noise estimate at full precision), the application version and the export
  time.

When the parameters alone do not regenerate the product the record says so in
`replay_caveat`: a hand-refined mask is not reducible to them (the recipe says
`Hand-edited: yes`, and the masked cube with its `HISTORY` is the exact record),
and a kinematic fit needs the selection mask it ran on, so its record points
back to the selection's recipe. See also
[Reproducibility](../backend-api/reproducibility.md).

## Selections from the VR headset

A selection made in the headset and applied on the desktop records the
headset's grow (*Made in: VR headset*), or its box, or, for a selection received
without its grow, the mask alone with a caveat. A box's apply is the one replay
step that records its region (`origin_*`, `width`, `height`, `depth`): for a box
the region is the selection itself and there is no grow response to take it
from. Every other apply takes its region from a response (the grow's, or the
shared mask's `X-Visivo-Origin`, `-Width`, `-Height`, `-Depth` headers).

## Validation against SoFiA-2

| Case | Result |
|---|---|
| Compact, well-detected sources | Overlap with an independently produced SoFiA-2 mask is typically Dice > 0.8 from a single click at default settings, essentially independent of where inside the source the click falls. |
| Large, low-surface-brightness objects | Works with a higher Reach; the result depends more on the click position. |
| Sources detectable only after smoothing | Out of scope: their voxels sit below the nσ contour individually, and a voxel-level geodesic cannot follow emission that is nowhere above threshold. |

Integrated fluxes inside a lasso selection and inside a SoFiA-2 mask of the same
source differ by a few percent, and the difference is statistically significant:
the two methods include different voxels, and the noise floor is an order of
magnitude below the disagreement. Such a comparison is an agreement between two
segmentations, not consistency within errors.

The full validation on two HI cubes (Dice, completeness, reliability, seed
robustness, flux uncertainties) and the scripts that reproduce it are in
`validation/README.md` in the repository.
