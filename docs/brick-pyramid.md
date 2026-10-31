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

```bash
cd backend
python -m app.brick_pyramid build cube.fits [out.pyr.h5] [--brick 64] [--max-block-mb 256]
python -m app.brick_pyramid info out.pyr.h5
```

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

The file is written under a temporary name and renamed when complete, so a
reader never finds a partial pyramid, and a build that is stopped leaves
nothing behind. In float32, the levels add up to about 8/7 of the cube for a
cube with axes of comparable size (each level is about an eighth of the one
below), and HDF5 stores the edge chunks whole even
where the cube does not fill them, so the file is larger by an amount that
depends on how the cube's sizes fit the brick: 1.25 times the cube for the
1586 x 1586 x 793 test cube, 1.48 times for the 794 x 794 x 397 one.

On a shared filesystem that does not support HDF5's file locking, set
`HDF5_USE_FILE_LOCKING=FALSE` for the processes that read pyramids.

## Measurements

`benchmark/brick_pyramid/pyramid_bench.py` builds the pyramid of a cube and
times random brick reads per level; its README gives the commands, including a
build in a container whose memory limit is smaller than the cube.
