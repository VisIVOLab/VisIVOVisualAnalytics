# Polarimetry (Stokes Q, U, V)

Radio surveys publish full-Stokes data in two shapes, and VisIVO-next handles
both — but by two different routes, because they are not the same problem.

| Your data | Where to work | Why |
|---|---|---|
| One 2-D image per Stokes plane | [Image viewer](image-viewer) → *Stokes Analysis* | Small enough to combine in the viewer, pixel by pixel |
| One **cube** per Stokes plane (or one cube with a Stokes axis) | **Cube viewer → Tools** | Too large to hold; the backend does the arithmetic |

This page is about the second case. A single MeerKAT MGCLS field is
7500 × 7500 × 16 per Stokes plane — 3.4 GB each uncompressed — so Q and U are
never both in the viewer's memory. Instead the cube viewer finds the companion
**files**, opens them on the backend, and tells the backend which file holds
which Stokes plane. What comes back are finished maps, registered as ordinary
2-D products: they mount in panes, carry statistics, and export like a moment
map.

## 1. Load the Stokes companions

Open the cube you have — normally Stokes I — then
*Tools → **Load Stokes Q/U/V Companions…***.

The viewer lists the directory **on the backend** (not on your machine: with a
remote backend they are different filesystems) and looks for siblings of the
file you opened, trying these spellings in order:

| Your file | Candidates tried for Q |
|---|---|
| `…StokesI….fits` | `…StokesQ….fits`, and the `stokesq` / `STOKESQ` casings |
| `…_I.fits` / `…_I.fits.gz` | `…_Q.fits` / `…_Q.fits.gz` |
| `…_I_PB.fits[.gz]` | `…_Q_PB.fits[.gz]` first, then `…_Q.fits[.gz]` |

The last row matters for MGCLS: 32 of its 52 fields ship the intensity as
`FIELD_I_PB.fits.gz` while Q, U and V have no `_PB` variant at all, so looking
only for `_Q_PB` would find nothing.

Whatever is not found by name, you are asked to locate yourself, one file at a
time, through the backend's file browser. Anything found is opened in this
window's session and remembered for the rest of the session.

```{note}
If the cube you opened carries its own Stokes axis with two or more planes,
there is nothing to load: the menu entry says so and the analysis tools are
already enabled.
```

## 2. Polarised intensity and angle

*Tools → **Polarised Intensity & Angle (current channel)*** computes, for the
channel currently on screen:

$$PI = \sqrt{\max(Q^2 + U^2 - \sigma_{QU}^2,\ 0)} \qquad
  PA = \tfrac{1}{2}\arctan_2(U, Q)$$

and registers two products, *Polarised intensity (ch N)* and *Polarisation
angle (ch N)*. The channel is part of each product's identity, so two channels
give two products rather than one silently overwriting the other; recomputing
the same channel refreshes every pane already showing it.

PA is drawn with a diverging colour map because it is an angle in −90…+90°: with
a sequential map the wrap at ±90° reads as a real gradient.

σ²_QU is the de-bias term. It is zero unless line-free channels are supplied,
in which case it is estimated from the variance of Q and U over them.

## 3. RM synthesis

*Tools → **Rotation Measure Synthesis…*** computes the Faraday dispersion
function over the cube's frequency axis (Brentjens & de Bruyn 2005):

$$F(\varphi) = \frac{1}{N}\sum_i P(\lambda_i^2)\, e^{-2i\varphi\lambda_i^2},
  \qquad P = Q + iU$$

You choose φ min, φ max and the φ step; the dialog shows how many samples that
gives, over how many pixels, and how large the full dispersion cube would be.
If a region is drawn on the 2-D slice you can restrict the computation to it.

Two maps come back:

- **Peak |F(φ)|** — how polarised each pixel is at its strongest Faraday depth;
- **Faraday depth (RM)** — the φ at which it peaks, i.e. the rotation-measure
  map. Diverging colour map, because the sign is the physics.

```{important}
The whole dispersion cube is **not** transferred. It is `n_φ × height × width`
floats: for a 7500² image a hundred φ samples is 22 GB. The server
refuses a cube request above 512 MiB and says so, naming the two ways out — the
peak maps (what the viewer asks for) or a smaller region.
```

λ² is derived from the frequency axis of the file that supplies **Stokes Q**,
not from the cube you opened, and from its `FREQnnnn` keywords when it is an
MFImage product. Both rules matter for MGCLS, in different ways. The sub-band
keywords of a field's I, Q and U cubes agree with each other, but none of them
agrees with the WCS axis: the keywords say 908–1656 MHz over 14 planes, the
axis says 1284–2147 MHz over 16. Deriving λ² from the axis — which also drags
the two fitted planes into the transform — recovers **63 rad m⁻² where the
true value is 25**, a factor of 2.5, and the map looks perfectly plausible. If Q and U come from
different files whose frequency grids disagree, the request is refused rather
than silently combining measurements taken at different frequencies.

## 4. Keeping the results

Every map here is an ordinary 2-D product, and
*Tools ▸ Export ▸ **Export Displayed Map as FITS…*** writes the one currently
shown in a pane into Workspace Exports as a standalone 2-D FITS. It works for
any map the cube viewer produces — polarised intensity, position angle, peak
|F(φ)|, Faraday depth, line-width — not only for moments.

What ends up in the file:

- **a proper 2-D image** (`NAXIS=2`), with the spectral and Stokes axes
  projected away rather than left degenerate, so it opens straight in the image
  viewer;
- **the celestial WCS** of the cube the map derives from. If the map covers a
  drawn region, `CRPIX` is shifted by the crop origin — without that the file
  would claim the whole field's astrometry and every position read off it would
  be wrong by the offset;
- **the map's own `BUNIT`**, not the cube's: `deg` for a position angle,
  `rad m-2` for a Faraday depth, whatever the cube was measured in;
- **the parameters, as `HISTORY` cards** — the φ grid, the channels, which file
  supplied Q and U. The same rows the Inspector shows, so a result whose
  provenance lived only in a screenshot becomes one somebody else can check.

A moment map is exported differently and deliberately: the service recomputes
it from its recipe at full precision, instead of uploading the displayed
pixels.

```{note}
The action reports "no 2-D map is on screen" rather than doing nothing if no
product is mounted in a visible pane. Show the map first.
```

## Worked example: MeerKAT MGCLS

The MeerKAT Galactic-plane full-Stokes release
([doi:10.48479/f2a2-qw16](https://archive-gw-1.kat.ac.za/public/repository/10.48479/f2a2-qw16/index.html))
is the reference dataset for this workflow.

**Pick a field whose intensity file is `_I.fits.gz`, not `_I_PB.fits.gz`** — both
work, but the plain one exercises the simplest naming rule. `G000.0+2.5` is one
of the 20 such fields. Download I, Q and U (V only if you want circular
polarisation):

```
G000.0+2.5_I.fits.gz    2512 MB   →  3434 MB unpacked
G000.0+2.5_Q.fits.gz    2531 MB   →  3433 MB
G000.0+2.5_U.fits.gz    2531 MB
```

**Unpack them all, into one directory.** The backend only accepts `.fits`,
`.fit` and `.fts`, and gzip would defeat memory mapping anyway: every read would
decompress 3.4 GB.

Then open `G000.0+2.5_I.fits`, load the companions, and the two tools above are
enabled. The archive also ships `G000.0+2.5_RM.fits.gz`, an independently
produced Faraday cube, which is a ready-made check on your own RM map.

```{warning}
Four things in these files are worth knowing before you trust a header:

- **`NAXIS3 = 16` is not sixteen sub-bands.** These are Obit MFImage products:
  the axis holds `NTERM = 2` *fitted* planes (total flux, spectral index) and
  then `NSPEC = 14` sub-bands, whose real frequencies are in per-plane
  `FREQ0001`…`FREQ0014` keywords — 908 to 1656 MHz. The WCS axis
  (`CRVAL3`/`CDELT3`) describes 1284 to 2147 MHz, which is neither the same
  grid nor the same planes. VisIVO-next reads the keywords when they are
  present and consistent, and falls back to the WCS axis when they are not.
  Without this, RM synthesis would use the wrong λ² *and* feed two fitted
  planes into the transform, producing a plausible-looking and wrong RM map.

- **`OBJECT` is unreliable.** The Q/U/V cubes of `G000.0+2.5` say
  `OBJECT = 'SMC1R07C'`, a different pointing entirely.
- **The V cubes carry `CRVAL4 = 1`**, which decodes as Stokes I. VisIVO-next
  therefore trusts the role *you* assigned a companion file over what its
  header claims, whenever that file holds a single Stokes plane — and logs the
  disagreement rather than refusing a published dataset over a wrong keyword.
- **`CTYPE3` is `SPECLNMF`**, Obit MFImage's spelling for a per-sub-band
  frequency axis and not in the FITS standard. VisIVO-next recognises it. The
  axis has no `CUNIT`, so the unit is inferred from the magnitude; a header
  whose values are not plausible frequencies in Hz is rejected rather than
  guessed at.
```

## What is checked, and where

| Behaviour | Test |
|---|---|
| Naming rules, including `_I_PB` → `_Q` | `tests/test_stokes_companion.cpp` |
| Plane taken from a companion file | `backend/tests/test_polarisation.py::TestPerFileStokes` |
| Mislabelled single-plane companion | same, `test_mislabelled_single_plane_companion_is_trusted` |
| λ² taken from the Q file, not the primary | `TestPerFileRmSynthesis::test_peak_is_at_the_injected_rm` |
| Mismatched Q/U frequency grids refused | `test_mismatched_frequency_grids_are_refused` |
| Oversized FDF cube refused with a way out | `test_oversized_cube_is_refused_with_a_way_out` |
| `SPECLNMF`, and unit inference | `TestSpectralAxisUnits` |
