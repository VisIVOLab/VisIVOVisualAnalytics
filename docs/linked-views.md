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
| **Camera** (3-D view) | `vtkCommand::InteractionEvent` on the cube interactor | position / focal point / view-up / parallel-scale / view-angle copied, then re-render |
| **Velocity channel** | `requestRemoteSlice()` (the single convergence for all channel changes) | `requestRemoteSlice(channel)`, clamped to each cube's own depth |
| **Colour map** | `applyColorMapByName()` | `applyColorMapByName(name)` |

Enabling Link Views on a viewer immediately pushes its **camera + channel** to
the other already-linked viewers so they snap into alignment. The colour map is
left to adopt on the next change, so linking doesn't silently overwrite a peer's
chosen colours.

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

- Cubes of **different depth** stay synced by channel index (clamped per cube);
  there is no spectral-axis (velocity) registration between cubes yet.
- The **colour-map name** is synced, not the table range or stretch.
- Sync is peer-to-peer across open viewers; there is no single "grid" container.
