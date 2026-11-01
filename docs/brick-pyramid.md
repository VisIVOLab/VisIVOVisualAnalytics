# Brick pyramid

A brick pyramid holds a cube as fixed-size bricks at several resolutions, so
that a renderer can read only the bricks a view needs at the resolution it
needs. It is step 0 of the plan for rendering cubes larger than a GPU or a
node's memory ([#73](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/73)):
the server-side out-of-core renderer
([#70](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/70)) and the
client-side bricked renderer
([#15](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/15)) read the
same file. The code is `backend/app/brick_pyramid.py`.

## Format

One HDF5 file per cube (version 1):

| Item | Content |
|---|---|
| root attributes | `format` (`visivo-brick-pyramid`), `version`, `brick` (voxels per side, a power of two, 64 by default), `levels`, `shape` of level 0, `axis_order` (`zyx`), and the source: `source_path`, `source_hdu`, `source_size`, `source_mtime_ns` |
| `levelK/data` | the cube at level K, float32, axes z, y, x, chunked by the brick, NaN for a blank voxel |
| `levelK/min`, `levelK/max` | one value per brick: the minimum and maximum of its finite voxels, NaN for a brick with none |

Level 0 is the cube. Level K+1 is level K halved on every axis (rounded up):
each voxel is the mean of a 2x2x2 block of level K over its finite voxels, NaN
when none is finite, so blank regions stay blank instead of pulling averages
towards zero. The last level fits in one brick. The per-brick extremes let a
renderer skip empty bricks and bricks entirely below the threshold without
reading them.

Bricks are stored without a border. A reader that interpolates across brick
edges asks for a ghost border, which is read from the neighbouring bricks and
padded with NaN outside the cube (`BrickPyramid.read_brick(level, bz, by, bx,
ghost=1)` returns `brick + 2*ghost` voxels per axis).

The pyramid records the file it was built from. Opening it with the source
(`BrickPyramid(path, source=...)`) checks the path, size and modification time,
and a changed or different file raises `StalePyramid`. Unless told otherwise,
pyramids go to a hidden directory of the workspace (`<exports>/.pyramids`, or
`VISIVO_PYRAMID_DIR`), named from the source's path, HDU, size and
modification time, so a changed file gets a new pyramid.

## Building

A pyramid is built in one of two ways: **automatically**, the first time a
remote 3-D view needs it (see [Rendering from the
pyramid](#rendering-from-the-pyramid): the view shows the progress and then
switches to it), or **by hand**, beforehand, so that nobody waits for it.

### By hand

```bash
cd backend
python -m app.brick_pyramid build cube.fits [out.pyr.h5] [--hdu N] [--brick 64] [--max-block-mb 256] [--workers N]
python -m app.brick_pyramid info out.pyr.h5
```

- **Where it goes.** Without `out.pyr.h5` the pyramid goes where the backend
  looks for it: `VISIVO_PYRAMID_DIR`, or else `.pyramids` in the backend's
  workspace exports (`<exports>/.pyramids`). The name comes from the cube's
  path, HDU, size and modification time. Build it with the **same**
  `VISIVO_PYRAMID_DIR` the backend will have, or the backend will not find it
  and build its own. A cube changed afterwards gets a new pyramid; the old one
  is no longer used.
- **Which HDU.** Without `--hdu`, the first HDU with an image, the one the
  viewer opens.
- **How many processes.** `--workers`, by default the CPUs available up to 8
  (see *Several processes* below).

On a cluster, run it in a job of its own with the backend's image, so it does
not take the backend's CPUs. For example, on PLEIADI-GPU:

```bash
srun -p gpu --gres=gpu:1 -c 8 --mem=32G --time=00:30:00 \
    apptainer exec --bind /scratch \
    --env PYTHONPATH=<clone>/backend,VISIVO_PYRAMID_DIR=<workspace>/exports/.pyramids \
    <dir>/backend-egl.sif python3 -m app.brick_pyramid build <data>/cube.fits
```

(`<workspace>` as given to the launcher with `--workspace`; the job needs no
GPU, but a partition may require one.) With the desktop's local backend, the
same command from `backend/` writes to `~/.visivo/exports/.pyramids` unless
`VISIVO_PYRAMID_DIR` is set.

### How it is built

The builder never holds the cube. It reads level 0 from the FITS file in
brick-aligned blocks of at most `--max-block-mb` and builds each level from
the one below, block by block, so the memory it uses is a small multiple of
the block size whatever the size of the cube. The FITS data is read without
astropy's whole-array scaling: uncompressed data is memory-mapped raw and
scaled block by block (BLANK to NaN, then BSCALE and BZERO, unsigned integers
restored exactly), and tile-compressed images are decompressed one tile at a
time. Two inputs cannot be read in bounded memory and are refused: a FITS file
compressed as a whole (`.fits.gz`, `.bz2`, `.zip`) and a tile-compressed image
whose tiles are larger than the block size.

**Several processes.** With `--workers N`, by default the CPUs available up
to 8, N processes read the cube and compute the averages and per-brick
statistics, while one process writes the file. The blocks pass between them
through shared memory, about twice `--max-block-mb` in all, and the result is
the same as with one process; with the workers' own temporaries a build with
8 processes uses about four times `--max-block-mb`. A tile-compressed cube is
built by one process, and so is any build when `/dev/shm` cannot hold the
buffers (Docker gives it 64 MiB unless `shm_size` is set). Builds of several
cubes at the same time each check the free space on their own, so give a
container's `/dev/shm` room for more than one build. On
PLEIADI-GPU (POWER9) a 23.4 GiB cube is built in 78 s with one process and in
45 s with four or more. Past four, writing the file is the limit, at about
600 MB/s to BeeGFS: more processes do not shorten it. The background builds
of the remote view use the same default.

The file is written under a temporary name and renamed when complete, so a
reader never finds a partial pyramid. A build that is stopped removes the
partial file; one killed outright leaves it, and a later build removes it. In float32, the levels add up to about 8/7 of the cube for a
cube with axes of comparable size (each level is about an eighth of the one
below), and HDF5 stores the edge chunks whole even
where the cube does not fill them, so the file is larger by an amount that
depends on how the cube's sizes fit the brick: 1.25 times the cube for the
1586 x 1586 x 793 test cube, 1.48 times for the 794 x 794 x 397 one.

On a shared filesystem that does not support HDF5's file locking, set
`HDF5_USE_FILE_LOCKING=FALSE` for the processes that read pyramids.

## Rendering from the pyramid

A remote 3-D view of a cube larger than `VISIVO_RENDER_OUT_OF_CORE_BYTES`
(6 GiB of float32 by default) does not load the cube: it draws it from the
pyramid (`backend/app/render_session/_ooc.py`, block choice in
`backend/app/brick_lod.py`). Smaller cubes, and every view on a local backend,
keep the paths they had.

- **Pyramid, built automatically.** The view uses the cube's pyramid from the
  default location. When it is missing or out of date (the cube changed), the
  backend builds it there in a process of its own (`backend/app/pyramid_builds.py`),
  with the same several processes as by hand. Meanwhile the remote view
  answers HTTP 202 with the fraction done, and the desktop keeps its preview
  and shows "Preparing the cube for full resolution… N%". The view switches
  to the pyramid when the build ends.
  - Every view of the same cube shares one build, also across backends sharing
    `VISIVO_PYRAMID_DIR` (a lock file next to the pyramid decides which builds;
    the others show its progress).
  - A failed build is reported with its reason and not retried for a minute.
  - A build stopped with the backend removes its partial file; one killed
    outright leaves it, and the next build removes it once nobody has written
    to it for ten minutes.
  On PLEIADI-GPU the build of a 23.4 GiB cube takes about 45 s. To skip the
  wait, build it beforehand with the command above, with `VISIVO_PYRAMID_DIR`
  set as the backend will have it.
- **Blocks.** For each view the session picks blocks of 256 voxels per side
  at the level each part of the view needs: fine near the camera, coarse far
  away or outside the window. The choice keeps within
  `VISIVO_RENDER_OOC_BUDGET_BYTES` of GPU data and `VISIVO_RENDER_OOC_MAX_BLOCKS`
  blocks; each block is one ray-casting pass, about 5 ms per frame on a V100.
  Blocks are chosen again when the view stops moving; the new ones are read a
  few at a time between frames, and they replace the old set only once all
  have arrived, so a frame never shows a hole. While you drag, the set stays
  and only its drawing order changes.
- **No seams.** Each block carries one extra sample on every side and is
  cropped to the region it owns, so blocks of different levels meet exactly.
  They are drawn back to front, which composites correctly in Composite mode.
- **MIP and MinIP.** An extremum along the whole ray cannot be assembled from
  separate passes, so in these modes the view draws the finest level that fits
  the budget as one block.
- **Threshold.** In Composite mode, blocks whose values are all below the
  transparent end of the opacity ramp are not read. The default threshold
  (from the noise, see [The default threshold](user-guide/cube-viewer.md#the-default-threshold))
  and the cutting plane's colour range come from a sample of full-resolution
  bricks.
- **Everything else.** The outline, the isosurface and the percentiles use a
  coarse copy (at most 256³ voxels) placed over the cube. The cutting plane is
  read from the FITS file at full resolution.

| Variable | Default | Effect |
|---|---|---|
| `VISIVO_RENDER_OUT_OF_CORE_BYTES` | 6 GiB | Cubes larger than this (as float32) are drawn from their pyramid. |
| `VISIVO_RENDER_OOC_BUDGET_BYTES` | 2 GiB | GPU data of the blocks drawn at once, extra samples included. |
| `VISIVO_RENDER_OOC_MAX_BLOCKS` | `12` | Blocks drawn at once. |
| `VISIVO_RENDER_OOC_NODE` | `256` | Voxels per side of a block; a multiple of the pyramid's brick. |

## Measurements

`benchmark/brick_pyramid/pyramid_bench.py` builds the pyramid of a cube and
times random brick reads per level; its README gives the commands, including a
build in a container whose memory limit is smaller than the cube.

`benchmark/remote_render/out_of_core_views.py` measures the view drawn from
the pyramid. On PLEIADI-GPU (one V100 16 GB), a 23.4 GiB cube opened in 2.6 s
with its pyramid already built, the views settled within 1.3 s, the view
turned at 54 frames per second while dragged, and the render process peaked
at 2.5 GB. On a 1.8 GB cube drawn both ways, the frames differ from the
in-core ones by 1.4 to 2.1 (of 255, on average) in the overall and MIP views;
zoomed in, the coarser blocks in front of the cutting plane show less noise. The
recorded runs are in `benchmark/remote_render/results`. Building the pyramid of
the 23.4 GiB cube took 253 s then; it takes about 45 s since issue #89 (see
Building).
