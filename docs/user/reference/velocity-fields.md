# Velocity fields and cosmic flows

The velocity-field viewer shows reconstructed peculiar-velocity fields
(CosmicFlows and similar): a 3-D grid of velocity vectors in km/s, read from a
FITS file with a length-3 component axis. Open it with *Data ▸ Open Velocity
Field…*, or with *File ▸ Open…* on such a file. A galaxy catalogue can be
overlaid to compare the reconstruction with observed motions.

## Flow

- *Streamlines*: traces that follow the flow from *Seeds* starting points.
  Convergent bundles mark attractors, divergent ones voids.
- *Arrows*: vectors sampled on the grid, at most *Max arrows*, sized by
  *Arrow scale* (relative to the grid, not to km/s).
- *Colour by vorticity*: colours the streamlines by the curl |∇×v| instead of
  the speed, to show shear and rotation.
- *Colormap*: the colour map of streamlines and arrows.

## Structure

- *Show basins*: watershed regions of the flow, the volumes that drain into the
  same attractor. *Basin type* chooses *Attraction (superclusters)* or
  *Repulsion (voids)*.
- *Overdensity (−∇·v)*: an isosurface of the converging regions, inferred from
  the velocity divergence.
- *Void (+∇·v)*: an isosurface of the diverging regions (voids, repellers).
- *∇·v level %*: the isosurface level, as a percentage of the peak.

The density shown here is inferred from a reconstruction, so it carries the
reconstruction's smoothing, bias model and selection function. Basin
boundaries depend on smoothing: check a boundary at two resolutions before
assigning a galaxy to a supercluster. Near the edge of the volume the flow is
constrained from one side only.

## Region of interest

The viewer opens a large grid at reduced resolution.

- *Define ROI*: shows a box you drag to choose a sub-volume.
- *Load full-res ROI*: loads that sub-volume at full resolution; the status
  line gives its size and origin, for example
  `Full-res ROI: 128×128×128 at [64,64,64]`.
- On a grid small enough to load whole, the check reads *Define ROI (n/a, full
  res)*.
- *Box size (Mpc)*: the physical extent of the grid (0 means grid units). It
  turns grid indices into distances, so check it against your data before
  reading any value in Mpc.

## Galaxy overlay

- *Overlay galaxy catalogue…*: places a catalogue in the field's volume. It must
  be Cartesian (X, Y, Z in Mpc) in the field's own frame and origin; a sky
  catalogue is placed with a note that it may not align.
- *Remove overlay*: removes the galaxies, density and legend.
- *Galaxy size*: the point size.
- *Galaxy shape*: *Points*; *Velocity arrows* (the galaxy's measured velocity,
  needs vx, vy, vz); *Residual arrows* (v_galaxy − v_field). Both arrow kinds
  share one physical scale. A good reconstruction leaves small, unstructured
  residuals.
- *Galaxy color*: *Uniform*, any numeric column, *Speed |v|*, *Field agreement*
  (the cosine of the angle between the galaxy's velocity and the field's at its
  position) or *Residual |v|*.
- *Galaxy density*: bins the galaxies into a density field and shows its
  isosurface at *Density level %* of the peak.

Distances come from the catalogue. A redshift of 0 means "not measured" and
falls back to the catalogue's distance column; a row with neither (or a
negative redshift) is not drawn. After an overlay the status line gives the
mean field agreement, residual and bulk flow (km/s) over the galaxies.

## Bulk flow

- *Bulk flow vs radius…*: the mean velocity of the galaxies within a growing
  sphere about the observer, with *Reconstruction (at galaxies)*, the field
  sampled at the same galaxies. The two curves should agree out to the radius
  where the reconstruction is constrained.
- *Field bulk flow vs radius…*: the reconstruction's own bulk flow, without a
  catalogue.

*Bulk flow: galaxies have no radial extent.* means the catalogue has no usable
distances.

Details: [Velocity-field viewer design](../../technical/design/velocity-field-viewer.md)
and the [velocity API](../../technical/backend-api/velocity.md).
