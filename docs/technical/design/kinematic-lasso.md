# Kinematic Lasso — interactive 3-D object selection & masking (F2)

Select a source you can **see** in the volume render with a single double-click,
and the analysis backend grows the whole connected object in
position–position–velocity (PPV) space — following its real morphology and
velocity structure, not a box or a coordinate list. The selection can be refined
live (reach / velocity coupling), extended or trimmed with extra clicks, hand-
edited channel-by-channel on the 2-D map, and finally applied to write a new
masked cube (isolate the object, or remove it and keep the residual).

This is VisIVO's answer to the 3-D selection problem that VR tools (iDaVIE) and
coordinate-typing dialogs solve awkwardly: the interaction is a click on the
thing you want, and the *analysis backend* — not a hand-drawn box — decides the
extent.

Backend: `backend/app/compute/selection.py` + endpoints in
`backend/app/routers/cube.py`. Desktop: `src/gui/vtkWindowCube_Lasso.cpp`.

---

## What one double-click does

1. A `vtkVolumePicker` ray-casts the click through the volume to the **seed
   voxel** (the visible surface of whatever you clicked).
2. The backend takes the connected component of the ~`nσ` emission that contains
   the seed — i.e. **the whole object**, however large and however it bends
   through velocity — and grows a geodesic, velocity-weighted region over it.
3. The selection comes back as a bounding-box binary mask, contoured as a green
   isosurface in the 3-D view and outlined on the 2-D channel map.

Because the grow is bounded by the *object* (its connected ~`nσ` component), not
by a fixed ROI box, a single click on a galaxy grabs the entire disk in one shot
— including its rotating extent across channels.

### The algorithm

Given the seed and a robust noise scale `σ` (1.4826·MAD, from a strided sample so
it stays cheap on huge memmapped cubes):

- **Candidate emission** = finite voxels above the `nσ·σ` contour that defines the
  object. Below that the soft-threshold traversal cost blows up anyway.
- **Connected component** (6-connectivity) containing the seed → the object.
  Isolated noise specks above `nσ` are their own tiny components and are dropped.
- **Faint shell**: the component is dilated by a margin so `reach` / `velocity
  coupling` can bleed the selection a little past the `nσ` contour into the
  object's faint outskirts (the geodesic, not the dilation, decides the final
  extent).
- **Geodesic grow**: a Dijkstra over the component's voxels. Entering a voxel
  costs `exp(nσ − I/σ)` — bright voxels are ~free (the grow flows through the
  object), voxels near `nσ` cost ~1, fainter ones blow up (a barrier). NaN voxels
  are hard barriers. Steps **along the velocity axis** are weighted by `λv` — the
  *kinematic knob*. Growth stops at the `reach` cost budget.

All work runs inside a seed-centred window, so the connected-component and graph
temporaries are bounded by the object, not the whole cube — it stays out-of-core
on large datasets.

### Parameters

| Control | Meaning |
|---------|---------|
| **Reach** | geodesic cost budget — larger selects further along the object, into its fainter (near-`nσ`) margins. |
| **Velocity coupling** (`λv`) | weight of a step along the spectral (velocity) axis. `λv < 1` → cheap velocity steps, the selection follows the source further *along velocity* (a rotating disk, an outflow); `λv > 1` → stays spatially compact. `1` = isotropic. |
| `nσ` | soft-threshold contour that defines the object (default `2.5`, backend default). |
| `roi_radius` | `0` = object-bounded (grab the whole connected source, the GUI default); `> 0` = confine the grow to a hard half-box around the seed. |

---

## Refining the selection

- **Reach / Velocity coupling sliders** re-grow the selection live.
- **⇧-double-click** adds a *separate* source to the selection (union of grows).
- **⌥-double-click** carves a region out (subtract a grow).
- **Double-click** elsewhere starts a fresh selection.

One double-click already grabs the whole connected object; the multi-seed clicks
are for combining genuinely *distinct* sources, not for tiling one object.

### Per-channel 2-D refinement

For pixel-level control the auto-grown mask can be hand-edited channel-by-channel
— the professional mask-editing step (SoFiA / kvis / SlicerAstro) that the
automatic grow can't always nail on blended or low-S/N emission.

- The selection is drawn as a **green outline on the 2-D channel map**, following
  the channel (CH) slider so you can see exactly which pixels are selected at each
  velocity.
- Toggle **Refine on 2-D map…** and pick **Add** or **Subtract**.
- **Click** points on the channel map to trace a polygon (the in-progress outline
  is drawn live); **right-click**, or click near the first point, to **close** it.
- The enclosed pixels are added to / removed from the mask **on the current
  channel**; the 3-D isosurface and the 2-D outline update immediately.

While 2-D refinement is active it owns the channel-map pointer (probe / PV /
region interactions are suspended and restored on exit); the window/level image
style is restored when you leave the mode or switch views.

```{note}
Phase-2 edits are clamped to the current selection's bounding box — you can trim
and add within the object's extent, but adding pixels far outside it (growing the
bounding box) is not yet supported. Draw a fresh ⇧-double-click seed there
instead.
```

---

## Applying the selection

Two buttons turn the selection — exactly as shown, including any 2-D edits — into
a **new cube dataset** in the Workspace Exports (open it from the Data Hub to run
moment maps, profiles, etc. on just the selection):

| Button | Effect | Result |
|--------|--------|--------|
| **Isolate → new cube** | blank everything *outside* the selection | the object on a blank (NaN) background |
| **Remove → new cube** | blank everything *inside* the selection | the cube with the object removed — the *residual* |

Blanked voxels are written as NaN (full dimensions + WCS preserved) and render as
transparent in the 2-D channel map, so a "Remove" residual reads cleanly.

The apply sends the **exact mask** the isosurface shows — it is not re-grown from
the seed — so a hand-refined mask is written verbatim.

---

## Backend endpoints

| Endpoint | Purpose |
|----------|---------|
| `POST /v1/cube/kinematic_select_bin` | grow the selection; returns the bounding-box binary mask (little-endian float32 `0/1`, x-fastest) + its origin/dims + selected count, for the isosurface preview and the 2-D overlay. |
| `POST /v1/cube/mask_selection` | grow from seeds **and** apply (isolate / remove) → new dataset. |
| `POST /v1/cube/mask_apply_bin` | apply an **explicit** mask (raw float32 body + origin/dims as query params) verbatim → new dataset. This is what applies a hand-refined mask; no re-grow. |

Request fields (`kinematic_select_bin` / `mask_selection`): `dataset_id`,
`seed_z/y/x`, `add_seeds` / `sub_seeds` (`[z,y,x]` each), `reach`, `lambda_v`,
`sigma` (`0` → robust auto), `nsigma`, `roi_radius` (`0` = object-bounded). A seed
outside the cube or on a blank/NaN voxel is user error → HTTP 422.

---

## Limits & future work

- **SKA-scale cubes.** The connected-component + geodesic is bounded to a
  seed-centred window (safety radius 256), so it is safe on cubes far larger than
  the object, but a single grow can still peak around a GB on a 512³ cube, and the
  window caps objects wider than ~512 voxels. Truly gigantic cubes want a
  blocked / streaming connected-component + geodesic (memory ∝ object).
- **Bounding-box growth** for 2-D edits (adding pixels beyond the current
  selection extent) is not yet implemented.
- **VRAD vs VOPT** velocity conventions are both treated as "velocity".
