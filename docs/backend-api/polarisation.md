# Polarisation

Implemented in `backend/app/routers/polarisation.py`, computed in
`backend/app/compute/polarisation.py`. All endpoints require `X-Visivo-Token`
and `X-Visivo-Session`.

## Where the Stokes planes come from

Every endpoint here accepts data in either of the two layouts radio surveys
publish:

- **one file with a `STOKES` axis of length ≥ 2** — the planes are read out of
  the dataset named in the path, and nothing else is needed;
- **one file per plane**, each with a degenerate `STOKES` axis of length 1 —
  MeerKAT MGCLS, ASKAP/POSSUM, anything from Obit MFImage. The caller names the
  companion datasets, which must have been opened in the same session.

```json
"companions": {
  "i_dataset_id": "", "q_dataset_id": "ds_…", "u_dataset_id": "ds_…", "v_dataset_id": ""
}
```

A plane the primary dataset carries itself always wins; companions are consulted
only for what is missing. If a plane is neither present nor supplied, the error
names it:

> `Stokes Q, U required but not available: the dataset carries ['I'] and no
> companion file was supplied for Q, U. Load the Stokes companions first.`

Two rules are worth knowing because published data needs them:

- **A companion's own Stokes header is advisory.** When the file holds a single
  Stokes plane, the role the caller assigned it wins and the disagreement is
  logged. The MGCLS V cubes carry `CRVAL4 = 1`, which decodes as Stokes I;
  refusing them would reject a real dataset over one wrong keyword.
- **λ² comes from the file that supplies Q**, never from the primary. In MGCLS
  the I cube and the Q/U cubes of the same field are on different frequency
  grids (1.3426 vs 1.2838 GHz), and using the wrong one biases every rotation
  measure. Q and U on grids that disagree are refused.

## `GET /v1/stokes/{dataset_id}` · `GET /v1/stokes/{dataset_id}/binary`
One Stokes plane as a 2-D float32 image.

| Query | Meaning |
|---|---|
| `plane` | `I`, `Q`, `U` or `V` (default `I`) |
| `channel` | spectral plane to take; default 0 |
| `companion_dataset_id` | the dataset holding `plane`, when the primary does not |

JSON returns `{valid, plane, channel, width, height, range_min, range_max,
data_base64}`; `/binary` returns one Visivo binary frame.

## `POST /v1/polarisation/pi/{dataset_id}` · `POST /v1/polarisation/pi/result/binary`
Bias-corrected polarised intensity and position angle.

```json
{
  "dataset_id": "...",
  "channel": 0,
  "line_free_channels": [0, 1, 2],
  "companions": { "q_dataset_id": "ds_…", "u_dataset_id": "ds_…" }
}
```

`PI = sqrt(max(Q² + U² − σ²_QU, 0))`, `PA = ½·atan2(U, Q)` in degrees
(−90…+90). σ²_QU is estimated from `line_free_channels` when at least two are
given, and is 0 otherwise. The binary variant returns **two** frames, PI then
PA.
Channels that carry no data (a blank sub-band, NaN or zero-filled) are left out of the estimate and listed in `line_free_channels_dropped`; fewer than two usable ones is an error (422), and a `channel` outside the cube is refused rather than clamped.

## `POST /v1/polarisation/rmsynthesis/{dataset_id}` · `POST /v1/polarisation/rmsynthesis/{dataset_id}/binary`
RM synthesis (Brentjens & de Bruyn 2005).

```json
{
  "dataset_id": "...",
  "phi_range_rad_m2": [-100.0, 100.0],
  "dphi_rad_m2": 1.0,
  "reduce": "peak",
  "region": [x0, y0, width, height],
  "companions": { "q_dataset_id": "ds_…", "u_dataset_id": "ds_…" }
}
```

| Field | Meaning |
|---|---|
| `reduce` | `"cube"` (default) for the whole FDF, `"peak"` for the two peak maps |
| `region` | pixel box; the whole image when absent |

`reduce: "peak"` returns peak `|F(φ)|` and the φ at which each pixel peaks — the
RM map — as two binary frames, plus `phi_peak_min` / `phi_peak_max`. This is
what the desktop client asks for.

`reduce: "cube"` returns the whole dispersion cube as one frame of
`(n_phi · height)` rows × `width` columns. It is refused above **512 MiB**,
with the message naming the ways out:

> `The requested FDF cube is 6250 MiB (400000 φ × 64 × 64), over the 512 MiB
> limit. Ask for reduce='peak', a smaller region, or a coarser φ grid.`

The φ grid is `phi_min, phi_min + dphi, …` up to and including `phi_max` when a whole number of steps reaches it (a step below the numerical resolution of φ is refused with 422). Both variants always return `phi_grid`, `rmsf`, `n_phi` and `n_freq`, plus `dropped_planes` and `dropped_freq_hz`: sub-bands that carry no data in the requested region (a release can flag one out entirely, as NaN or zeros) are left out of the transform and listed by file-plane index and frequency; `n_freq` counts the planes actually used, and fewer than three is an error (422).
