# Linked Cube Views (F3)

Compare two (or more) spectral cubes side by side by **synchronising** their
viewers: rotate one and the others follow, scrub the velocity slider and they
scan together, change the colour map and they all recolour. This is the
Encube/Fluke comparative-grid idea, implemented by linking the existing separate
cube viewers (`vtkWindowCube`) rather than building a new multi-pane window.

Toggle **Tools ▸ Link Views (sync camera / channel / colour)** on each viewer
you want in the group. `src/gui/vtkWindowCube_Link.cpp`.

---

## What is synced

| Aspect | Hooked at | Applied to peers |
|--------|-----------|------------------|
| **Camera** (3-D view) | `vtkCommand::InteractionEvent` on the cube interactor | shared **relative to each cube's data bounds** (see below), then re-render |
| **Velocity channel** | `requestRemoteSlice()` (the single convergence for all channel changes) | matched by **spectral value (velocity)** — see below |
| **Colour map** | `applyColorMapByName()` | `applyColorMapByName(name)` |

Enabling Link Views on a viewer immediately pushes its **camera + channel** to
the other already-linked viewers so they snap into alignment. The colour map is
left to adopt on the next change, so linking doesn't silently overwrite a peer's
chosen colours.

### Heterogeneous cubes (different size / spectral axis)

Cubes are rarely identical, so both camera and channel are synced **physically**,
not by raw coordinates/indices:

- **Camera** is shared *relative to each cube's own data bounds*. The sender
  encodes the view direction, view-up, and the focal offset / camera distance /
  parallel-scale as **fractions of its data diagonal**; each peer reconstructs
  the camera against *its own* centre + diagonal. So a 512×512×200 cube and a
  128×128×64 cube linked together stay correctly framed on their own data with
  the same orientation and relative zoom — copying absolute world coordinates
  would have thrown the larger cube off-screen.
- **Channel** is matched by **velocity**: the sender resolves its current
  channel's spectral value (`spectralAxisValue`) and each peer jumps to the
  channel whose spectral value is **closest** to it (linear scan). So if cube A
  (100 channels) and cube B (50 channels) are linked, B tracks the *velocity* of
  A's channel rather than its index — B no longer simply saturates at its last
  channel, and channel N of A is not naively equated with channel N of B. This
  requires the cubes' effective spectral **units to match** (`spectralAxisDescriptor().unit`);
  otherwise it falls back to clamped index matching.

---

## How it works

- A static registry `s_cubeWindows` holds every open cube viewer (added in the
  constructor, removed in the destructor; `QPointer` auto-nulls on destruction).
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

- Channel velocity-matching requires matching spectral **units** between cubes
  (no Hz↔m/s conversion yet); mismatched units fall back to index clamping.
- The **colour-map name** is synced, not the table range or stretch.
- Sync is peer-to-peer across open viewers; there is no single "grid" container.
