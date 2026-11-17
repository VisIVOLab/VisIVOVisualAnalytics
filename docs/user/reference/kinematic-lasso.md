# Kinematic Lasso

Selects a source in a spectral cube from a double-click on it in the 3-D view.
The selection grows from the clicked voxel through the connected emission:
bright voxels are cheap to cross, faint ones expensive, and blank (NaN) voxels
stop it. The velocity coupling λ_v sets the cost of a step along the spectral
axis, so the selection can follow a rotating disc across channels or stay
compact on the sky. Turn it on with *Tools ▸ Sources & Kinematics ▸ Kinematic
Lasso (click to select)* in the cube viewer, or from *Inspector ▸ Analysis*;
its controls open in *Inspector ▸ Parameters*.

The lasso segments a source you can already see. To search a field for sources,
use [Source finding](source-finding.md).

For a walk-through, see
[Find and measure a source](../use-cases/02-find-and-measure-a-source.md).

## Mouse

- Double-click: selects the connected object under the click, however large.
  A single drag still rotates the view.
- ⇧ double-click: adds another source to the selection.
- ⌥ double-click: carves a region out of the selection.
- Esc, or un-checking the menu entry: closes the tool and clears the selection.

A yellow marker shows where the click landed. A marker with no green surface
around it means the click found no source. The selection is drawn as a green
isosurface in the 3-D view and as an outline on the 2-D channel map. Taking the
3-D view out of every pane only suspends the tool; the selection is kept.

## Controls

| Control | Range | What it does |
|---|---|---|
| *Reach* | 1 to 64 (default 8) | How far into the faint outskirts the selection may grow. Raise it for extended, low-surface-brightness sources (30 or more). Raising it only adds voxels. |
| *Velocity coupling* (λ_v) | 0.1 to 5.0 | Cost of a step along the spectral axis. Below 1 the selection spreads along velocity (a rotating disc); above 1 it stays spatially compact (two objects overlapping on the sky at different velocities). |
| *Threshold* (nσ) | 1.0 to 5.0σ (default 2.5σ) | The level that defines the connected emission. Lower it to join a source to faint gas separated by sub-threshold voxels. σ comes from *Tools ▸ Estimate Noise* when it has been run, otherwise from the cube itself; the *Noise σ* line says which. |
| *Smoothing* | *Off*, *Light (1, 3, 3)*, *Medium (2, 4, 4)*, *Strong (2, 6, 6)* | Smooths the cube (kernel in voxels: velocity, y, x) before thresholding, to take in a faint envelope. The mask is applied to the unsmoothed cube. |
| *Clear selection* | | Starts again without closing the tool. |

Below the controls: *Selected voxels*, the selected pixels per channel as a
small profile, and the channel extent (with velocity or frequency when the axis
has one).

Dragging *Reach* updates the surface at once. Changing λ_v recomputes the
selection.

There is no universal value for *Reach*: on tested HI sources the best value
ranged from 4 to 64 and followed the source's surface brightness relative to
the noise. Adjust it while watching the surface, and state the value used in a
published figure. If the surface stops growing early, the object is separated
by sub-threshold voxels: lower *Threshold* rather than raising *Reach*.

## Refining on the 2-D map

- *Refine on 2-D map…*: draw a polygon on the current channel to add pixels to
  the mask (*Add*) or remove them (*Subtract*), channel by channel. Use it for a
  foreground star, an artefact or a connected companion.

**Changing *Reach* afterwards discards the hand refinements.** Settle the
controls first, then refine.

## Applying the selection

- *Isolate → new cube*: blanks everything outside the selection.
- *Remove → new cube*: blanks everything inside it, leaving the residual field.

Both write the mask as displayed, 2-D refinements included, into a new cube in
the Workspace Exports; the original file is not changed. The new cube keeps the
full dimensions and WCS of the source, so it overlays the original channel for
channel; to get a small file, focus the 3-D view and use *Tools ▸ Export ▸ Export
Sub-Cube as FITS…*, which crops.

The result appears in Session Data as a *LASSO* product. Its Provenance lists
the seed, the controls, the number of voxels and whether the mask was edited by
hand, and the same values go into the FITS `HISTORY`. *Export recipe as JSON…*
writes a record that replays the selection.

A source larger than the grow's limit (four million voxels) is cut to a box
around the click. The status bar says so and the Provenance shows a *Domain
trimmed* line.

## Selections in the VR headset

With the cube open in a headset (*Tools ▸ Views ▸ Open in VR Headset…*), *Show
in VR Headset* sends the current selection to the headset. A lasso or a box made
in the headset appears in this panel by itself; *Isolate* and *Remove* apply it,
and the product records that it was made in the headset. See [VR](vr.md).

## Where it works and where it does not

- Compact, well-detected sources: one click at the default settings usually
  matches an independent SoFiA-2 mask well (Dice above 0.8), wherever inside the
  source you click.
- Large, low-surface-brightness objects: need a higher *Reach*, and the result
  depends more on the click position. Try a few positions.
- Sources found only after smoothing over many channels: out of reach, because
  their voxels are nowhere above the threshold. Use SoFiA-2.
- Fluxes compared with a SoFiA-2 mask differ by a few percent, and the
  difference is real (the masks hold different voxels). Report it as agreement
  between two segmentations, not as consistency within errors.

## Problems

| Symptom | Fix |
|---|---|
| *"Seed voxel is blank/NaN (click on visible emission)."* | The click hit a blank voxel. Rotate so the source faces you and click nearer its centre. |
| The selection takes in a neighbour. | Carve it with ⌥ double-click, or raise λ_v if the two differ in velocity. |
| Only a small blob at the peak. | Raise *Reach*. |
| Raising *Reach* floods into noise. | Use the largest value before the surface stops looking like the source. |
| My 2-D edits vanished. | *Reach* was changed after them. Refine last. |
| Nothing happens on double-click. | The tool is off, or the cube is still loading its preview. |

Details: [Kinematic Lasso: methods and implementation](../../technical/methods/kinematic-lasso.md),
and the design note [Kinematic Lasso](../../technical/design/kinematic-lasso.md).
