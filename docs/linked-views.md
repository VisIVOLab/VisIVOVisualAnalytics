# Linked Cube Views (F3)

Compare two (or more) spectral cubes side by side by **synchronising** their
viewers: rotate one and the others follow, scrub the velocity slider and they
scan together, change the colour map and they all recolour. This is the
Encube/Fluke comparative-grid idea, implemented by linking the existing separate
cube viewers (`vtkWindowCube`) rather than building a new multi-pane window.

Toggle the **Linked** button in the viewer's toolbar (or **Tools ▸ Link Views**)
on each viewer you want in the group. `src/gui/vtkWindowCube_Link.cpp`.

---

## What is synced

| Aspect | Hooked at | Applied to peers |
|--------|-----------|------------------|
| **Camera** (3-D view) | `vtkCommand::InteractionEvent` on the cube interactor | shared **relative to each cube's data bounds** (see below), then re-render |
| **Velocity channel** | `requestRemoteSlice()` (the single convergence for all channel changes) | matched by **spectral value (velocity)** — see below |
| **Volume colour** (LUT) | `applyColorMapByName()` / 3-D LUT customizer | colormap **name** + volume **stretch** (scale mode + gamma) |
| **Representation** (render) | the Volume ↔ Isosurface toggle (`changeCubeRender()`) | peers switch representation via `applyLinkedRenderMode()` |
| **Volume blend mode** (blend) | the Composite / MIP / MinIP toggle (menu *and* sidebar) | `applyVolumeBlendMode(idx)` |

Enabling Link Views on a viewer immediately pushes its **camera + channel** to
the other already-linked viewers so they snap into alignment. The colour map is
left to adopt on the next change, so linking doesn't silently overwrite a peer's
chosen colours.

### Per-facet toggles

Next to the **Linked** master button, five chips — **camera · channel · LUT ·
render · blend** — let you enable or disable each propagation kind
independently (e.g. sync the camera and channel but keep independent colour
maps). Each chip is a toggle gated by its own flag (`m_linkCamera`,
`m_linkChannel`, `m_linkLut`, `m_linkRender`, `m_linkBlend`); a broadcast fires
only when both the sender **and** the receiving peer have that facet on. The
chips are live only when there is actually something to link — more than one
pane in the window **or** more than one open cube window. The **Linked** master
button itself is always clickable (it is a persistent preference that activates
as soon as a peer appears), and stays in sync with the *Tools ▸ Link Views* menu
item.

### Heterogeneous cubes (different size / spectral axis)

Cubes are rarely identical, so both camera and channel are synced **physically**,
not by raw coordinates/indices:

- **Camera** is shared *relative to each cube's own data bounds*. The sender
  encodes the view direction, view-up, and the focal offset / camera distance /
  parallel-scale as **fractions of its data diagonal**; each peer reconstructs
  the camera against *its own* centre + diagonal. So a 512×512×200 cube and a
  128×128×64 cube linked together stay correctly framed on their own data with
  the same orientation **and zoom** — copying absolute world coordinates would
  have thrown the larger cube off-screen. (Both `InteractionEvent` *and*
  `EndInteractionEvent` are observed so mouse-wheel **zoom** — which doesn't emit
  `InteractionEvent` — is broadcast too.)
- **Channel** is matched **physically when it makes sense, proportionally
  otherwise**:
  - If the cubes share a spectral **unit** *and* the sender's velocity falls
    **within** the peer's velocity range (their ranges overlap → likely the same
    object, e.g. data vs model), the peer jumps to the channel whose spectral
    value is **closest** to the sender's velocity.
  - Otherwise (different objects, non-overlapping ranges, or different units) it
    maps **proportionally**: the peer goes to `round(fraction × peerMaxChannel)`
    where `fraction` is the sender's channel position. So unrelated cubes scan
    their full velocity ranges **together** instead of one saturating at an end.

  This replaces naive index matching, which both saturated the shorter cube and
  wrongly equated channel N of one cube with channel N of another.

  Velocity matching also works across **convertible units** — `m/s ↔ km/s`, and
  within frequency `Hz/kHz/MHz/GHz` — so a data cube in m/s and a model in km/s
  of the same object still align by velocity. Frequency↔velocity is *not*
  converted (it needs a rest frequency), so those fall back to proportional.

---

## How it works

- A static registry (`LinkedWindowRegistry`, `src/gui/LinkedWindowRegistry.h`)
  holds every open cube viewer (added in the constructor, removed in the
  destructor; `QPointer` auto-nulls on destruction). Its `snapshot()` returns the
  peer list by value so a viewer closing mid-broadcast can't invalidate the loop.
- A change in a linked viewer is **broadcast** to the other linked viewers. Each
  broadcast **snapshots** the registry before iterating, so a viewer closing
  (`WA_DeleteOnClose`) mid-broadcast can't invalidate the loop.
- An `m_applyingLinkedUpdate` guard (RAII via `QScopedValueRollback`) plus the
  fact that programmatic slice/colour updates don't re-fire the user-facing
  slots (the slider/spin are set under `QSignalBlocker`) prevents echo loops.
- The camera observer is installed once (lazily, on first enable) and removed in
  the destructor from the *exact* interactor it was added to
  (`vtkWeakPointer`), only if still alive.
- Linked-driven slice requests skip the ±1 neighbour **prefetch**, so synced
  scrubbing across N windows doesn't multiply backend fetches.

---

## Notes / limitations (phase-1)

- Channel velocity-matching works for identical or **convertible** spectral
  units (m/s↔km/s, Hz/kHz/MHz/GHz); frequency↔velocity and unrelated units fall
  back to proportional mapping.
- The **3-D volume** colour syncs (colormap name + stretch); the **table range**
  is intentionally *not* shared (data-dependent — each cube auto-ranges to its
  own data), and the **2-D slice** plane's colour scale is managed separately
  and not synced.
- Colour sync is **local-render only**; the server-side-rendering (SSR) path
  mirrors only the colormap name, not the linked stretch.
- If the 3-D LUT customizer dialog is open on a peer when a linked stretch
  arrives, the model/LUT update but the dialog's controls may show stale values
  until reopened.
- Sync is peer-to-peer across open viewers; there is no single "grid" container.
