# Polarimetry: methods and implementation

User reference: [Polarimetry](../../user/reference/polarimetry.md).
Routes: [Polarisation API](../backend-api/polarisation.md).

## Why the backend does the arithmetic

A single MeerKAT MGCLS field is 7500 × 7500 × 16 per Stokes plane, 3.4 GB each
uncompressed, so Q and U are never both in the viewer's memory. The cube
viewer finds the companion files, opens them on the backend, and tells the
backend which file holds which Stokes plane. The backend returns finished maps,
registered as ordinary 2-D products.

## Companion naming

The viewer lists the directory on the backend (with a remote backend, a
different filesystem from the desktop's) and tries these spellings in order:

| Open file | Candidates tried for Q |
|---|---|
| `…StokesI….fits` | `…StokesQ….fits`, and the `stokesq` / `STOKESQ` casings |
| `…_I.fits` / `…_I.fits.gz` | `…_Q.fits` / `…_Q.fits.gz` |
| `…_I_PB.fits[.gz]` | `…_Q_PB.fits[.gz]` first, then `…_Q.fits[.gz]` |

The last row matters for MGCLS: 32 of its 52 fields ship the intensity as
`FIELD_I_PB.fits.gz` while Q, U and V have no `_PB` variant. Anything found is
opened in the window's session and remembered for the rest of the session.

## Polarised intensity and angle

PI = sqrt(max(Q² + U² − σ²_QU, 0)), PA = ½ atan2(U, Q). σ²_QU is zero unless
line-free channels are supplied, in which case it is estimated from the
variance of Q and U over them. The channel is part of each product's identity,
so two channels give two products; recomputing a channel refreshes every pane
showing it. PA uses a diverging colour map because the wrap at ±90° would read
as a gradient on a sequential map.

When Q and U are separate files they must describe the same pixels: the same
sky grid (a pair rotated with respect to each other by PC or CD is refused,
however alike their keywords look; PC and CD forms of one grid are accepted)
and, for every channel used, the same frequency when both files state one. A
plain 2-D map without a spectral axis is only checked for having the channel.
A channel that one of the two files does not have is refused rather than
paired with that file's last plane.

## RM synthesis

F(φ) = (1/N) Σᵢ P(λᵢ²) exp(−2iφλᵢ²), P = Q + iU (Brentjens & de Bruyn 2005).
Both ends of the φ range are sampled; a range that is not a whole number of
steps stops at the last sample before φ max. The significance-masked depth map
uses σ = 1.4826 × the MAD of the peak map, recorded in the product; outside
the mask the "depth" is the φ of the largest noise excursion.

The dispersion cube is `n_φ × height × width` floats: for a 7500² image a
hundred φ samples is 22 GB. The server refuses a cube request above 512 MiB
and names the two ways out (the peak maps, which the viewer asks for, or a
smaller region).

λ² is derived from the frequency axis of the file that supplies Stokes Q, and
from its `FREQnnnn` keywords when it is an MFImage product. If Q and U come
from different files whose frequency grids disagree, the request is refused.

## MeerKAT Milky Way Bulge files

The MeerKAT 1.3 GHz Milky Way Bulge full-Stokes release (Cotton et al. 2025,
[doi:10.48479/f2a2-qw16](https://archive-gw-1.kat.ac.za/public/repository/10.48479/f2a2-qw16/index.html))
is the reference dataset. `G000.0+2.5` is one of the 20 fields whose intensity
file is `_I.fits.gz` rather than `_I_PB.fits.gz`:

```
G000.0+2.5_I.fits.gz    2512 MB   ->  3434 MB unpacked
G000.0+2.5_Q.fits.gz    2531 MB   ->  3433 MB
G000.0+2.5_U.fits.gz    2531 MB
```

Header pitfalls in these files:

- `NAXIS3 = 16` is not sixteen sub-bands. These are Obit MFImage products: the
  axis holds `NTERM = 2` fitted planes (total flux, spectral index), then
  `NSPEC = 14` sub-bands, whose frequencies are in per-plane `FREQ0001` to
  `FREQ0014` keywords, 908 to 1656 MHz. The WCS axis (`CRVAL3`/`CDELT3`)
  describes 1284 to 2147 MHz over 16 planes, neither the same grid nor the
  same planes. The keywords are read when present and consistent, with a
  fallback to the WCS axis otherwise. Deriving λ² from the axis, which also
  feeds the two fitted planes into the transform, recovers 63 rad m⁻² where
  the true value is 25, a factor of 2.5, in a map that looks plausible.
- `OBJECT` is unreliable: the Q/U/V cubes of `G000.0+2.5` say
  `OBJECT = 'SMC1R07C'`, another pointing.
- The V cubes carry `CRVAL4 = 1`, which decodes as Stokes I. The role assigned
  to a companion file wins over its header whenever the file holds a single
  Stokes plane, and the disagreement is logged.
- A sub-band can be blank: in G000.0+2.5 the 1199 and 1256 MHz sub-bands are
  NaN end to end (other pipelines fill flagged bands with zeros). RM synthesis
  leaves such planes out and records them as *Sub-bands dropped*; *Channels
  used* counts only the planes transformed; fewer than three usable sub-bands
  is refused. PI and PA on a blank plane are refused with a message naming the
  plane.
- `CTYPE3` is `SPECLNMF`, MFImage's spelling for a per-sub-band frequency axis,
  not in the FITS standard. It is recognised. The axis has no `CUNIT`, so the
  unit is inferred from the magnitude; values that are not plausible
  frequencies in Hz are rejected.

## Export

A 2-D product export projects the spectral and Stokes axes away (`NAXIS = 2`),
writes the celestial WCS of the source cube with `CRPIX` shifted by the crop
origin for a region map, the map's own `BUNIT`, and the parameters as
`HISTORY` cards. A moment map is exported differently: the service recomputes
it from its recipe at full precision instead of uploading the displayed
pixels.

## Tests

| Behaviour | Test |
|---|---|
| Naming rules, including `_I_PB` to `_Q` | `tests/test_stokes_companion.cpp` |
| Plane taken from a companion file | `backend/tests/test_polarisation.py::TestPerFileStokes` |
| Mislabelled single-plane companion | same, `test_mislabelled_single_plane_companion_is_trusted` |
| λ² taken from the Q file, not the primary | `TestPerFileRmSynthesis::test_peak_is_at_the_injected_rm` |
| Mismatched Q/U frequency grids refused | `test_mismatched_frequency_grids_are_refused` |
| Q/U rotated by PC or CD refused, PC vs CD of one grid accepted | `backend/tests/test_photometry_polarisation_axes.py` |
| PI/PA channel missing from U, or at another frequency, refused | same file |
| Oversized FDF cube refused with a way out | `test_oversized_cube_is_refused_with_a_way_out` |
| `SPECLNMF`, and unit inference | `TestSpectralAxisUnits` |
