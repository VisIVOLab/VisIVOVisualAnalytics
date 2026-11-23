# The particles of a cosmological simulation

You will draw the gas of a simulation box as a projected map of ray-traced
particles coloured by temperature, then cut a box around the largest cluster
of a second run and read the temperature and the motions of its gas.

## You need

- Two IPAC tables from the demo data, with the columns `x`, `y`, `z` (Mpc),
  `vx`, `vy`, `vz` (km/s), `LogMass` (M☉), `LogD`, `LogT` (K) and `LogMet`
  (no unit given):
  - `snap_p50n576_z0.tbl`: 3,931,546 particles in a cube 71.429 Mpc on a
    side (50 h⁻¹ Mpc for h = 0.7). `snap_p50n576_z0_SUBTABLES/` holds the
    same rows in 27 sub-boxes.
  - `snap_comaII_z0.tbl`: 3,656,447 particles in a region 140 to 160 Mpc
    across, around one massive cluster.
- A running backend; the local one is enough.

Nothing in the files, the demo-data notes or the repository history names
the simulation, its code, its cosmology or the meaning of `LogD` and
`LogMet`. About one row in seven has no `LogT` and a
`LogD` that is no gas density; these rows behave like star particles. The
masses of the first file add up to 1.8 % of the baryons its volume holds at
the Planck 2018 mean density, so the table keeps a small share of the run's
particles.

## Steps

### The cosmic web

1. Choose *File ▸ Open…* (or *Data ▸ Open 3D Catalogue (CSV)…*) and select
   `snap_p50n576_z0.tbl`. The 3-D catalogue viewer loads a random sample:
   *Loaded 100000 of 3931546 sources.* The toolbar shows an *IPAC · 3931546
   rows* tag, and the table opens with the particle defaults: *Gaussian
   splats*, coloured by `LogMass`.

2. Keep the gas. In the Inspector's *Analysis* tab, under *FILTERS*, press
   *Add field filter…*, choose *Field* `LogT`, *Operator* `>`, *Value* `0`, press *OK*,
   then *Apply*: *1 filter(s) active, 50000 of 3392027 sources.*

3. In *VISUAL ENCODING* set *Colour by* to `raw:LogT`. Under *SOURCE REPRESENTATION*
   tick *Ray-traced light*. In the *RAY-TRACED* card set *Mode* to *Projected
   map*, *Colour by* to *View's colours* and *Light* to `LogMass` with *log*
   ticked. Under *More settings* keep *Size* on *From neighbours* (N = 32).
   A pixel's brightness is the projected gas mass, its colour the
   mass-weighted mean `LogT` along the line of sight. The brightness of a
   particle no longer depends on how many particles the table holds. If some
   rows have no usable light, the status line at the bottom says how many were
   dropped or clipped.

   ```{figure} img/14-simulation-particles-1.png
   :alt: The 3-D catalogue window with the gas of the simulation box as a projected map, coloured by temperature, with the Ray-traced settings open in the left dock and a colour bar for LogT.
   :width: 100%

   The gas of the box as a projected map, coloured by temperature.
   ```

### A cluster

4. Open `snap_comaII_z0.tbl` and add the same filter (3,147,829 gas
   particles). With *Ray-traced light* cleared, click a particle in the
   bright knot at the centre (double-click): *SOURCE INFO*, on the
   Inspector's *Properties* tab, shows its row number and its distance from
   the origin, 603.7 Mpc for the core. It does not list the table's columns.
   The mass-weighted centre is at x = 348.63, y = 346.62, z = 350.83 Mpc.

   ```{figure} img/14-simulation-particles-2.png
   :alt: The 3-D catalogue window with the gas of the cluster simulation as Gaussian splats coloured by temperature, zoomed on the central cluster, with the Source Info section of the Inspector showing a selected row.
   :width: 100%

   The cluster at the centre of the second table.
   ```

5. Press *Select 3-D region…*, keep *Shape* *Box* and *Keep* *Inside*, and
   type *X min / max* 346.6 and 350.6, *Y* 344.6 and 348.6, *Z* 348.8 and
   352.8 (a box 4 Mpc wide). Press *OK* and *Apply*: *2 filter(s) active,
   50000 of 90721 sources.* Press *Load all*.

6. Press *Range and scale…*: the editor (*LUT Customizer*) draws the
   histogram of `LogT` over the rows loaded.

   ```{figure} img/14-simulation-particles-3.png
   :alt: The 3-D catalogue window with the Range and scale editor open over it, showing the histogram of LogT with a single peak between 7.5 and 7.8, and the two active filters listed in the Inspector.
   :width: 100%

   The `LogT` histogram of the cluster box.
   ```

7. Tick *Ray-traced light* again and set *Colour by* to *Velocity
   dispersion*: the spread of the velocities of each particle's 32 nearest
   neighbours, averaged along the ray.

   ```{figure} img/14-simulation-particles-4.png
   :alt: The cluster box as a projected map of ray-traced particles coloured by velocity dispersion in the Viridis map, with a colour bar from 68 to 252 km/s.
   :width: 100%

   The velocity dispersion of the cluster's gas.
   ```

## What you get

The box shows filaments with bright, hot knots where they cross; the
automatic *Range* spans 2.14 decades of projected mass. Of the gas mass, 49 %
is below 10⁵ K, 45 % between 10⁵ and 10⁷ K and 6 % above. Davé et al. (2001)
call the 10⁵ to 10⁷ K gas the warm-hot intergalactic medium and find 30 to
40 % of all baryons in it at z = 0.

In the cluster box the histogram has one peak between 10⁷·²⁵ and 10⁷·⁸ K.
Of the 90,721 particles, 98.6 % are above 10⁷ K; the median is 10⁷·⁶² K
(kT = 3.6 keV), against 10⁴·⁹¹ K for all the gas of the run. This is the
intracluster medium that X-ray telescopes observe.

The dispersion map runs from 68 to 245 km/s (1st to 99th percentile of the
lit pixels), median 106 km/s. The gas holds its random motion as
temperature: at 10⁷·⁶² K the thermal speed per axis, √(kT/μmₚ) with
μ = 0.59, is 767 km/s. The star particles in the box have a one-dimensional
dispersion of 417 km/s.

The particle sizes follow the local density. With the gas filter alone, the
median size in the inner 0.5 Mpc of the cluster is 0.036 Mpc, against
0.31 Mpc over the run: a factor of 680 in number density. The medians of the
table's own `LogD` (−26.83 and −29.45) give a factor of 420.

The app has no radial profile. In spherical shells around the centre, the
density of gas and stars falls with a logarithmic slope of about −1 between
0.2 and 0.5 Mpc, −1.5 near 1 Mpc and −3 to −4 between 1.5 and 2.5 Mpc, the
steepening that Navarro, Frenk & White (1997) found for dark-matter haloes
(from −1 to −3); gas and stars are only tracers here.

The counts, sizes, brightness range and dispersion map come from the
backend's own functions run on the two files; the shell profile, gas
fractions and star dispersion were computed separately with numpy.

## Caveats

- With no smoothing lengths in the tables, *From neighbours* sizes the
  particles from the table itself, so a sparse table gives wider kernels than
  the run's.
- Neighbours are searched among the filtered rows: within 0.1 Mpc of a box
  face the sizes grow by 13 %.
- Filter `LogT > 0` before using `LogD` as *Light*. Otherwise, in the cluster
  run, 1,018 star rows with `LogD` near −8.3 take all the light.
- The histogram covers the loaded rows (50,000 after a filter until *Load
  all*) and shows no median or percentiles.
- Filters and views do not wrap across a periodic box. The most massive knot
  of the first run lies 2.7 Mpc from the face at x = 0.

## Next

- [Ray-traced particles](../reference/raytraced-particles.md): all settings,
  stills, movies, *Save Light…*.
- [Gadget snapshots](../reference/gadget-snapshots.md): a run's own files,
  with smoothing lengths.
- [VBT tables](../reference/vbt-viewer.md) and [Cosmology](11-cosmology.md).

## References

- Davé R., Cen R., Ostriker J. P., et al., 2001, ApJ, 552, 473.
- Dolag K., Reinecke M., Gheller C., Imboden S., 2008, New J. Phys., 10,
  125006 (the ray-traced rendering method).
- Jin Z., Krokos M., Rivi M., Gheller C., Dolag K., Reinecke M., 2010,
  Procedia Comput. Sci., 1, 1775.
- Navarro J. F., Frenk C. S., White S. D. M., 1997, ApJ, 490, 493.
- Planck Collaboration, Aghanim N., et al., 2020, A&A, 641, A6.
- Springel V., White S. D. M., Jenkins A., et al., 2005, Nature, 435, 629
  (the look of the projected map).
