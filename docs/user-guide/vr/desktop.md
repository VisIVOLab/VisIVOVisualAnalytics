# The headset from the desktop

Once a headset page is open in the desktop's backend session (paired by code,
over the USB cable or on the local network), the desktop sees it: it can send
it another cube, it lists what was done in the headset, and it opens what was
made there in its own viewers.

## Show on Headset

**Tools → Views → Open in VR Headset…** first asks the backend whether a
headset page is already open in this session. When one is, the dialog names
it (how it is connected, whether it is in VR, the cube it shows) and offers
**Show on Headset**: the headset switches to this window's cube, in VR if it
is in VR. No new code, no adb, no new browser window.

**Pair Another Headset** brings back the pairing code and the USB section, for
a second headset. With no headset page open, or with a backend older than this
feature, the dialog is the pairing dialog described in
[Quickstart and pairing](quickstart.md). It keeps asking while it is open: a
headset page that connects later (by code or over the USB cable) switches it
to the connected-headset view by itself, and a code nobody used stops working.

The dialog grows with its text but stays on the screen: it moves up when its
bottom would go past the screen's, and on a short screen its sections scroll
while **Close** stays at the bottom.

The cube window's **Tools → Views → Show on Headset** does the same in one
click. It is enabled while a headset is connected to the session; its tooltip
names the headset. With several pages open, the newest one is used.

The status bar of a cube window says when a headset is connected: *Headset:
in VR · m33.fits*, or *Headset: connected* when it is not in VR (hover it for
the details).

## Headset Activity

**Tools → Views → Headset Activity** opens a list in Inspector ▸ Parameters
of what was done in the headset of this session, newest first, each line with
its time and a summary (wrapped to the panel's width): the cube it switched to, changes of the look and of
the clip box (the latest of each per cube), and the products made there.

Each product has an **Open** button, which opens it in the desktop's own
viewer:

| Made in the headset | Opens in |
|---|---|
| Moment map | the image viewer (its FITS file) |
| PV slice | this cube's PV view, along the headset's path; from another cube, its FITS file |
| Spectrum | a spectrum product of its cube's window (pane and Session Data), from its CSV or FITS table |
| Isolate or Remove | a new cube window (the masked cube's FITS file) |
| Catalogue or flow layer | its file, when it is a FITS file; otherwise it is named, to open from the Data Hub's Workspace Exports |

A product opened here is recorded in Session Data ▸ Provenance with the
headset's own record: its parameters, the requests that made it (under the
API's names) and its replay caveat, if any. It is recorded as a product of the
cube it was made from, so by that cube's window: opened from another cube's
window it is shown but not recorded there. Opening it again does not add a
second row. A PV slice opened in this cube's PV view is recorded once, in the
view's PV diagram row, which carries the headset's record (*Made in: the VR
headset*, its file, its requests). When the view moves on to another cut (a
path drawn here, the headset's path edited, another PV from the headset), the
headset's PV gets a row of its own.

A moment map the headset measured on a selection says so under *Mask*
(*the Kinematic Lasso selection (outside blanked); no threshold*): the page
records it as *Mask: none*, the desktop's word for "no threshold mask".

### Follow the headset's look

**Follow the headset's look**, in the same panel, is off by default. On, the
window applies the headset's colour map, threshold and clip box to its 3-D
view whenever they change in the headset for this cube:

- the colour map when the desktop has one of the same name (*grey* is *Gray*;
  the headset's *hot* has no counterpart, and the status bar says so);
- the threshold, in the cube's units, as if typed in the threshold field;
- the clip box crops the 3-D volume, also when the backend renders the view
  (the slice views are not clipped).

Off again, the clip box is removed; the colours and threshold stay, now the
window's own.

## Notes

- The headsets and the activity belong to the backend session, so every cube
  window of the session shows the same list.
- **Show on Headset** sends the dataset, its path and its kind (cube, point
  table, velocity field). A velocity field's box in Mpc is not part of the
  command: to see a field with its box, pair it from its window as before.
- The token is never in what the desktop sends or reads here: it travels in
  the request headers only.
- The routes are `GET /v1/vr/headset`, `POST /v1/vr/headset/command` and
  `GET /v1/vr/activity` (see the backend's API reference).
