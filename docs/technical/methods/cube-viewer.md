# Cube viewer: methods and implementation

User reference: [Spectral cube viewer](../../user/reference/cube-viewer.md).

## The default threshold

The 3-D view, local or remote, starts from a threshold computed by the backend
from the cube's noise, so that the first view shows the emission and not a
fog of noise voxels:

- The threshold is the value above which about 100 noise voxels of the whole
  cube are expected. A fixed number of standard deviations would not do: 3σ
  lets through 0.13 % of the noise voxels, about 100 000 in a cube of 77 million
  voxels. So the cut rises with the size of the cube: about 4.2σ on a
  6-million-voxel cube, 4.7σ on 77 million (4.8σ on HCG 44, whose tails are a
  little heavier). In a small cube at most 0.01 % of the voxels are let
  through (at least 3.7σ).
- The noise is read from the negative side of the cube. Emission is
  positive, so the values below the median are noise alone; the threshold
  mirrors their tail. Where the noise has heavier tails than a Gaussian, the cut
  rises by itself (7.3σ on an APERTIF cube whose tails reach that far), up to
  1.6 times the Gaussian value, so that negative artefacts (absorption,
  sidelobes) cannot raise it without bound.
- Data that is not noise around a median (a constant cube, a skewed positive
  distribution such as a simulation's density) keeps the 99th percentile.

On HCG 44 and WALLABY, about 70 % of the flux inside their SoFiA-2 source
masks stays above this threshold and 99 to 100 % of what is drawn lies inside
the masks; at 3σ only 17 % of what is drawn on HCG 44 does
(`validation/threshold_from_noise.py`).

The remote view starts with the backend's threshold, computed from a sample of
the full-resolution data (out of core, of its bricks). The Threshold field and
slider show it, and their range covers the cube's full-resolution values. A
cube still being read into the backend shows a threshold once the volume
appears and a final one when it is complete. Once the user moves the slider or
edits the field, that threshold is used and kept across Local and Remote.
Until then the desktop's own starting value is not sent to the backend.

## Preview sampling

A preview is normally every N-th voxel along each axis (N = 4 by default).
When the cube already has a [brick pyramid](../design/brick-pyramid.md#the-desktop-preview-from-the-pyramid),
the preview is read from it instead: each voxel is the mean of the N × N × N
block it stands for, so thin or noisy structures do not flicker or vanish
between preview sizes, and the cube's file is not read. Opening a cube never
builds a pyramid just for the preview. With a pyramid the preview stride is
rounded up to a power of two (8 rather than 7). Picks, the lasso, regions, the
headset clip and the kinematic model are placed with the preview's own
sampling: a block mean stands for the centre of its block, half a block from
where every N-th voxel would put it.

## Level-of-detail refinement

About 200 ms after the camera stops moving (a drag paused with the button held
counts as stopped only after 3 s), the viewer works out the part of the cube in
view, how many cube voxels one screen pixel covers at the focal point, and the
pyramid level that matches. When the budget cannot hold everything in view,
the part around the focal point is refined. Blocks are read in the background
and the view changes only once all have arrived. Blocks already read stay in
memory, up to twice the detail budget, until another cube (a new preview, a
region, the full resolution) is shown or the window closes. Picks on a refined
part land on the finer voxel drawn there.

A refined box (*Refine Selected Box…*) is the chosen box rounded outward only
to the level's samples (at level 2, multiples of 4 voxels). A box larger than
the budget allows at any level finer than the preview is refined around its
centre. Level 0 is read from the cube's file and needs no pyramid; coarser
levels come from the pyramid when it is ready, otherwise from every 2^l-th
voxel. Box and preview share one colour map, threshold, scale and opacity and
meet without seams in perspective; in a parallel projection along an axis, a
face exactly on a pixel column can show as a faint line.

Measurements: rotating a refined view costs one ray-casting pass per block. In
an offscreen 2200 × 1400 view on an Apple M5 Max it ran at 64 frames/s with 7
volumes (one fine block), 29 with 14 and 23 with 17. On the 2048³ PLUTO
density, a settled view of seven 256³ blocks was read by a local backend in
0.4 to 1.3 s, and a level-0 box of 512 × 512 × 256 voxels (275 MB) took 2.9 s
from the click to the refined view.

## Whole-cube statistics

The *3D cube* rows in *Inspector ▸ Properties* come from a background scan of
the file by the backend, without loading it into the viewer. For a cube shown
as a preview, Min and Max come from its brick pyramid when one exists (exact
for single-precision data), the Mean from a coarse level, and the other rows
from a sample of about 4 million voxels; without a pyramid every row comes
from that sample. Until the scan lands, the rows show what the grid on screen
measured when that grid is the whole cube.

## Isosurface

The surface is extracted on the backend (`vtkFlyingEdges3D`) from a sampled
grid of at most 256³ voxels (each axis sampled on its own, so a cube with few
channels keeps them all) and coarsened while it has more than 4 million
triangles: a 512³ simulation field takes 1 to 2 s. See
[Isosurface API](../backend-api/isosurface.md).

## Remote rendering

The dataset-open response carries `server_side_rendering_available`; a
CPU-only backend, or a macOS backend with no window server, reports false and
the desktop disables the mode. Up to 6 GiB (`VISIVO_RENDER_OUT_OF_CORE_BYTES`)
the backend loads the whole cube; a larger one is drawn out of core from its
[brick pyramid](../design/brick-pyramid.md#rendering-from-the-pyramid). Building
the pyramid the first time took about 45 s for a 23 GB cube on PLEIADI-GPU.

With the JPEG transport the backend sends a frame whenever the view changes, a
few frames ahead of the client's acknowledgements, and lowers the volume's
quality while the camera moves, aiming at about 25 ms of GPU time a frame; on
release it sends a full-resolution frame. Each remote view renders in its own
process, spread over the machine's GPUs in turn. A 2 GB cube on a V100 node
behind an SSH tunnel rotated at about 28 frames/s.

At most `VISIVO_RENDER_MAX_SESSIONS` views (8 by default) render at once,
cubes and 3-D catalogues together. Further requests queue first come first
served (a user with no remote view goes first); the queue's wait limit is
10 minutes unless the operator changes it. A view switched to Local keeps its
session, but while others wait the backend may give that slot away.

Regions on the cutting plane are drawn by the backend on its own plane, placed
by the same rule as on the desktop (cube pixel *i* of the slice is where the
volume shows that pixel, on a strided or block-mean preview as at full
resolution), against the volume with the backend's depth buffer, labels on
top, and hidden while the view is dragged. See
[Server-side rendering](../backend-api/render.md).

## Picking a spectrum in 3-D

The pick follows the blend mode. MIP and MinIP return the brightest and
faintest voxel on the ray; the renderer interpolates linearly, so the extremum
of the interpolated ray can differ from that of the voxels it crosses in
marginal cases. Composite has no single producing voxel, so by convention the
pick returns the first voxel at or above the opacity threshold (the front
surface). The isosurface returns where the ray meets it; the cutting plane, the
voxel under the click. With the 3-D rendering layer off, or a click on empty
space in isosurface mode, nothing is returned.

The plotted spectrum is the full profile at the hit sky position; the status
bar names the channel the ray hit. Looking down the spectral axis the ray
crosses channels and the maximum is the line peak; looking side-on it crosses
sky positions at roughly fixed velocity.

## Live probe on a preview

On a cube shown as a decimated preview the probe asks the backend for the
line of sight at full resolution (`POST /v1/spectral/probe`). One request is in
flight at a time; only the last pixel the cursor reached is asked next, and a
reply for a pixel the cursor has left is dropped.
