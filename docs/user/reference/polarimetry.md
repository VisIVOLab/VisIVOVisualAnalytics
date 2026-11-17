# Polarimetry (Stokes Q, U, V)

The cube viewer's *Tools ▸ Polarimetry* submenu works on full-Stokes cubes:
one cube per Stokes plane, or one cube with a Stokes axis. The Q and U cubes
are not loaded into the viewer; the backend reads them and returns finished
2-D maps, which mount in panes, carry statistics and export like a moment map.
For one 2-D image per Stokes plane, use *Tools ▸ Polarisation* in the
[image viewer](image-viewer.md).

For a walk-through, see [Polarimetry: RM synthesis](../use-cases/07-polarimetry.md).

## Tools ▸ Polarimetry

- *Load Stokes Q/U/V Companions…*: looks for the Q, U and V files next to the
  open cube (normally Stokes I), in the backend's directory, and opens them in
  this window's session. A file not found by name you pick yourself in the
  backend's file browser. A cube with its own Stokes axis needs no companions;
  the entry says so. The naming rules it tries:

  | Open file | Tried for Q |
  |---|---|
  | `…StokesI….fits` | `…StokesQ….fits` (also `stokesq`, `STOKESQ`) |
  | `…_I.fits`, `…_I.fits.gz` | `…_Q.fits`, `…_Q.fits.gz` |
  | `…_I_PB.fits[.gz]` | `…_Q_PB.fits[.gz]`, then `…_Q.fits[.gz]` |

  A companion holding a single Stokes plane is taken as the role you gave it,
  even if its header says otherwise.

- *Polarised Intensity & Angle (current channel)*: for the channel on screen,
  makes *Polarised intensity (ch N)* and *Polarisation angle (ch N)*:

  $$PI = \sqrt{\max(Q^2 + U^2 - \sigma_{QU}^2,\ 0)} \qquad
    PA = \tfrac{1}{2}\arctan_2(U, Q)$$

  PA is in degrees, −90 to +90, on a diverging colour map. σ²_QU is the
  de-bias term, zero unless line-free channels are given. Each channel is a
  separate product. Q and U must share the sky grid (a rotated pair is
  refused) and, where both state it, the channel's frequency; a channel
  missing from either file, or blank, is refused.

- *Rotation Measure Synthesis…*: computes the Faraday dispersion function over
  the frequency axis (Brentjens & de Bruyn 2005),
  F(φ) = (1/N) Σ P(λ²) e^(−2iφλ²) with P = Q + iU. The dialog fields:

  | Field | Meaning |
  |---|---|
  | *φ min*, *φ max* | Faraday depth range, rad/m². Both ends are sampled. |
  | *φ step* | Sample spacing; −80 to 80 at step 1 gives 161 samples. |
  | *Restrict to the drawn region* | Only the region drawn on the 2-D slice. |

  The dialog shows the number of samples and pixels and the size of the full
  dispersion cube. Three maps come back:

  - *Peak |F(φ)|*: the polarised intensity at the strongest Faraday depth.
  - *Faraday depth (RM)*: the φ of that peak, in rad/m², on a diverging map
    centred on zero.
  - *Faraday depth (peak |F| > 6σ)*: the same, only where the peak exceeds 6σ
    of the peak map (σ = 1.4826 × MAD). Read rotation measures from this one,
    for example as the median in a box region.

## Frequencies and requirements

- λ² comes from the file that supplies Stokes Q, not from the cube you opened.
- For Obit MFImage products (MeerKAT), the frequencies are read from the
  per-plane `FREQnnnn` keywords, and the fitted planes before the sub-bands
  are left out. The WCS axis of these files does not describe the sub-bands.
- If Q and U come from files with different frequency grids, the request is
  refused.
- Blank sub-bands are left out and listed as *Sub-bands dropped* in the
  product's parameters, next to *Channels used*. Fewer than three usable
  sub-bands is refused.
- The full dispersion cube is not transferred; requests for more than 512 MiB
  are refused. Use the peak maps or a smaller region.

## Export

Focus the pane holding a map and choose *Tools ▸ Export ▸ Export <map> as
FITS…* (for example *Export Peak |F(φ)| as FITS…*). It writes a 2-D FITS into
Workspace Exports with:

- `NAXIS = 2`, opening directly in the image viewer;
- the cube's celestial WCS, with `CRPIX` shifted for a map cut to a region;
- the map's own `BUNIT` (`deg` for an angle, `rad m-2` for a Faraday depth);
- the parameters (φ grid, channels, the Q and U files) as `HISTORY` cards.

If no map is shown in a visible pane, the action reports that no 2-D map is on
screen.

## Reference data

The MeerKAT 1.3 GHz Milky Way Bulge release (Cotton et al. 2025,
[doi:10.48479/f2a2-qw16](https://archive-gw-1.kat.ac.za/public/repository/10.48479/f2a2-qw16/index.html))
works with these tools. Keep I, Q and U in one directory. The backend opens
`.fits.gz` files directly, decompressing each once into its cache (about
3.4 GB per file). The archive's `_RM.fits.gz` cube is an independent check of
your RM map.

Details: [Polarimetry: methods and implementation](../../technical/methods/polarimetry.md)
and the [polarisation API](../../technical/backend-api/polarisation.md).
