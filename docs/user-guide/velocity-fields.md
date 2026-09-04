# Velocity fields and cosmic flows

A viewer for reconstructed peculiar-velocity fields — CosmicFlows and similar —
where the data is a 3-D grid of velocity vectors rather than intensities. It
answers a different question from the cube viewer: not *what is here* but
*where is everything moving*.

| | |
|---|---|
| **Open** | *File → Open Velocity Field…* |
| **Input** | a FITS grid with three velocity components (km s⁻¹) |
| **Overlay** | a galaxy catalogue, to compare the reconstruction against observed motions |

---

## What you can see

```{list-table}
:header-rows: 1
:widths: 22 78

* - **Streamlines**
  - Traces that follow the flow. The clearest way to see where matter is going:
    convergent bundles mark attractors, divergent ones mark voids.
* - **Arrows**
  - Vectors sampled on the grid, with an adjustable *Arrow scale*. Good for
    reading local direction and speed; cluttered over a large volume.
* - **Basins**
  - Watershed regions of the flow — the volumes that drain into the same
    attractor. Switch *Basin type* between **Attraction (superclusters)** and
    **Void (+∇·v)** to segment either the sinks or the sources.
* - **Density**
  - Structure inferred from the velocity **divergence**: converging flow implies
    mass. Adjust with *Density level %*.
* - **Vorticity**
  - Colour the field by curl instead of speed, to pick out shear and rotation
    that speed alone hides.
```

Colour source is chosen with *Colour by* — **Speed |v|**, **Divergence**,
**Vorticity** or **Structure** — with the usual colormap picker.

---

## Working with a large grid

The viewer opens at reduced resolution and fetches detail on demand, the same
approach the cube viewer uses.

- The initial view is a decimated version of the grid, so a large field opens
  quickly.
- **Define ROI** selects a sub-volume and fetches it at **full resolution**. The
  status line reports the extent it retrieved, e.g. `Full-res ROI: 128×128×128
  at [64,64,64]`.
- On a field small enough to load whole, the button reads *Define ROI (n/a —
  full res)*: there is nothing to refine.

*Box size (Mpc)* sets the physical extent the grid covers, which is what turns
grid indices into distances — check it against your data's own convention before
reading any number in Mpc off the screen.

---

## Overlaying a galaxy catalogue

*Overlay Catalogue* places observed galaxies in the same volume as the
reconstruction. This is where the viewer earns its keep: a reconstruction is a
model, and the galaxies are the observations it is meant to explain.

Each galaxy can be drawn by *Galaxy shape*, *Galaxy size* and *Galaxy color*,
and coloured by:

```{list-table}
:header-rows: 1
:widths: 30 70

* - **Uniform**
  - One colour. Use when position is all you want to read.
* - **Velocity arrows**
  - The galaxy's own measured peculiar velocity.
* - **Reconstruction (at galaxies)**
  - The field's prediction sampled at each galaxy's position.
* - **Residual arrows** / **Residual |v|**
  - Observed minus reconstructed. This is the diagnostic view: a good
    reconstruction leaves small, unstructured residuals, while a coherent
    residual pattern means the model is missing something real.
```

```{caution}
Galaxy distances come from the catalogue, and a redshift column holding `0`
means "not measured" — such rows fall back to the catalogue distance field, or
to a nominal 300 Mpc if there is none. Check how many rows that affects before
reading anything into their positions; the status line reports the count when
distances are recomputed.
```

### Bulk flow

*Bulk flow vs radius…* plots the mean velocity of galaxies within a growing
sphere, and *Field bulk flow vs radius…* does the same for the reconstruction.
Plotted together they are a direct test of the model: the two curves should
agree within the errors out to the radius where the reconstruction is
constrained, and diverge beyond it.

*Field agreement* summarises how well the reconstruction matches the observed
motions where both exist.

```{tip}
Bulk flow needs galaxies at a range of distances. If the plot is empty or flat,
check the message *"Bulk flow: galaxies have no radial extent"* — your catalogue
has no usable distances, so every galaxy sits at the same radius.
```

---

## Comparing two fields

Two reconstructions of the same volume can be loaded and compared — different
methods, different data releases, or the same method at two smoothing scales.
Use the residual and basin views rather than eyeballing the streamlines: two
fields can look similar in streamlines and disagree substantially about where
the basin boundaries fall, and the boundaries are what determine which
supercluster a galaxy belongs to.

---

## Reading the numbers honestly

- **The density is inferred, not measured.** It comes from the divergence of a
  reconstructed field, so it inherits every assumption in that reconstruction —
  smoothing scale, bias model, the survey's selection function.
- **Basins are sensitive to smoothing.** A boundary that moves when you change
  resolution was never well determined. Check it at two settings before
  attributing a galaxy to a supercluster.
- **The edge of the volume is not the edge of the universe.** Flows near the
  boundary are constrained by data on one side only. Treat the outer region as
  indicative.

---

## Troubleshooting

```{list-table}
:header-rows: 1
:widths: 42 58

* - Symptom
  - Cause / fix
* - Arrows are invisible, or fill the screen.
  - *Arrow scale* is relative to the grid, not to km s⁻¹. Adjust it, and prefer
    streamlines for an overview.
* - *"Basin computation failed"*
  - Usually a field with no interior structure to segment, or a ROI too small.
    Compute basins on the full field before a sub-volume.
* - Distances in Mpc look wrong by a constant factor.
  - *Box size (Mpc)* does not match the grid's own convention.
* - The overlay places galaxies outside the box.
  - Their distances exceed the field's extent — real, and worth knowing, but the
    field cannot say anything about them.
* - Streamlines stop abruptly inside the volume.
  - They have reached a blanked or zero region of the grid.
```

## See also

- [Catalogues and HiPS](catalogues-hips) — loading and filtering galaxy catalogues.
- Developer reference: [Velocity-field viewer](../velocity-field-viewer) — LOD,
  ROI fetching, the watershed, and the coordinate model.
