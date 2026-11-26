# An adaptive-mesh simulation

You will open a RAMSES cosmological run as it was written, draw its gas cells
as a projected map, resample the density onto a uniform cube, see where the
mesh is refined, and pull out the densest knot at a finer level.

## You need

- The RAMSES output `DemoData/Ramses/output_00185`: a cosmological zoom with 16
  files per kind (`amr_`, `hydro_`, `part_`), 2,097,152 dark-matter particles
  and a mesh of 15,974,645 leaf cells on levels 7 to 13 (the run allows
  levels up to 15, but none of its cells reached 14 or 15). Its box is about
  147,400 kpc across. It has no `*_file_descriptor.txt`, so its hydro and
  particle fields are read by their position, the layout RAMSES used before
  descriptors.
- A running backend; the local one is enough. No yt is needed for RAMSES.

## Steps

### Open the output

1. Choose *File ▸ Open…*, go to `DemoData/Ramses/output_00185` and open
   `info_00185.txt`. Any `info_`, `part_` or `amr_` file of the folder names
   the whole output. The *Open simulation snapshot* dialog says what the output
   holds:

   > RAMSES output 00185 (16 files) at z = -0.00107: 2097152 dark matter,
   > 15974645 gas cells.

   The redshift is slightly negative because the run's last scale factor,
   `aexp` = 1.00107 in `info_00185.txt`, is just past today.

   ```{figure} img/15-amr-simulation-1.png
   :alt: The Open simulation snapshot dialog for output_00185, listing 2097152 dark matter and 15974645 gas cells, with a Component to open menu set to dark matter.
   :width: 60%

   Each particle family, and the gas cells of the mesh, opens as a table of points.
   ```

2. Set *Component to open* to *gas cells* and press *OK*. The 3-D points window
   opens on a random sample of the cells: the status bar reads
   *Local · sample 70,000 of 15,974,645 rows*. Each row is one leaf cell:
   `x`, `y`, `z`, its size `dx`, its `level`, its `mass`, and the hydro fields
   `density`, `vx`, `vy`, `vz`, `pressure` and `metallicity`.

### The gas as a projected map

3. Under *SOURCE REPRESENTATION* tick *Ray-traced light*. In the *RAY-TRACED*
   card set *Mode* to *Projected map*; *Colour by* is *Density*, *Map* is
   *Magma* and *Log scale* is ticked. Each cell is drawn as wide as itself
   (its `dx`), so the small cells of the filaments stay sharp and the large
   cells of the voids stay smooth.

   ```{figure} img/15-amr-simulation-2.png
   :alt: The 3-D points window with the gas cells of output_00185 as a projected density map in the Magma colour map: filaments and knots across the box, the Ray-traced settings in the left dock.
   :width: 100%

   The gas cells of the run, projected along the line of sight.
   ```

### A uniform cube

4. Choose *Data ▸ Uniform Cube of an AMR Output…* and open `info_00185.txt`
   again. Set *Field* to `density` and *Level* to *Level 9*. The plan under the
   fields updates as you choose. With the default *Limit* of 64 million voxels,
   level 9 is refused:

   > Too large: 134,217,728 voxels is more than the 64,000,000 allowed:
   > level 8 fits, or ask for a larger limit (up to 1,073,741,824).

   Type `256` in *Limit*. The plan now reads:

   > Level 9: 512 × 512 × 512 voxels (134217728), voxel 288 kpc
   > Per field: 512.0 MiB Peak memory: 400.0 MiB

   ```{figure} img/15-amr-simulation-3.png
   :alt: The Uniform Cube of an AMR Output dialog with field density, level 9 and a limit of 256 million voxels; the plan reads 512 × 512 × 512 voxels, voxel 288 kpc, finest level in the region 13.
   :width: 60%

   The plan shows the size and memory before anything runs.
   ```

5. Press *Resample*. After about ten seconds the cube opens in a cube window.
   It is 0.5 GB, so a preview comes first; press *Load full resolution*.
   Each voxel holds the volume-weighted mean of the cells inside it, so the
   cube holds the same mass as the cells. The values are in g/cm³ and the axes
   in kpc, from the run's units. Untick *Physical* under *UNITS* in the
   Inspector to see the run's own code units instead.

### Where the mesh is refined

6. Switch to *2D* and move the slice to *Z* 326 (z = 93737 kpc), through the
   densest knot of the box. In the Inspector, under *AMR MESH* (it reads
   *density at level 9 of 13, from ramses.*), tick *Show AMR blocks*. The cells
   this slice cuts are outlined, coloured by their level, and the status line
   says for a few seconds how many cells the slice cuts and on which levels
   (here *48844 cells cut by this slice, levels 7 to 13.*). The blocks follow
   the slice as you move it.

   ```{figure} img/15-amr-simulation-4.png
   :alt: The cube window with a 2-D slice of the level-9 density cube at Z 326 in the Inferno colour map, log scale; the AMR blocks drawn over it, small around the filaments and knots, large in the voids; the Inspector shows the AMR MESH section with Show AMR blocks ticked.
   :width: 100%

   The refinement follows the gas: fine cells along the filaments and in the
   knots, coarse cells in the voids.
   ```

### The densest knot, finer

7. Press *Refine this box…* in the *AMR MESH* section. The *Refine a Box of the
   AMR Mesh* dialog takes a box in the voxels of this cube; set it to about
   32 voxels round the knot (*X* 209 to 241, *Y* 305 to 337, *Z* 309 to 341)
   and press *Refine*. The plan comes first:

   > Level 12: 264 × 264 × 264 voxels (18399744), voxel 36 kpc
   > Finest level that fits the limit: 12 Finest level in the region: 13

   The box holds cells down to level 13, but a level-13 cube of it would exceed
   the limit, so the plan offers level 12. Answer *Yes*. The refined cube opens in a new
   window and the level-9 cube stays open behind it, so the two can be compared.

   ```{figure} img/15-amr-simulation-5.png
   :alt: A 2-D slice of the refined level-12 cube around the densest knot, in the Inferno colour map: a bright core with an envelope and a smaller clump to its right, axes in kpc.
   :width: 100%

   The knot at level 12: voxels of 36 kpc, eight times finer than in the
   level-9 cube.
   ```

## Read the result

- The projected map of step 3 is drawn from the cells themselves, so nothing
  was resampled; the uniform cube of step 5 is a resampling, and its level
  sets the smallest structure it can show. A smaller box (step 7) can afford a
  finer level within the same limit.
- In the cube, a voxel that holds cells finer than itself shows their mean
  density. Velocities and metallicity are averaged with the gas mass as the
  weight, so the bulk motion of a voxel is that of its mass.
- The blocks of step 6 are the mesh RAMSES built. A cosmological run refines
  where the mass gathers, so the small cells trace the filaments and the
  knots.

## Read more

- [RAMSES outputs](../reference/ramses-outputs.md)
- [Ray-traced particles](../reference/raytraced-particles.md)
- [The particles of a cosmological simulation](14-simulation-particles.md)
