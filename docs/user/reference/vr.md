# Cubes in a VR headset

A cube, point table, catalogue or velocity field open on the desktop can be
explored in a Meta Quest 3. The backend serves a web page (WebXR) that the
headset opens; the headset draws a reduced (binned) copy of the data, so
moving your head or the cube never waits for the network, while everything
computed (spectra, the Kinematic Lasso, moments, PV slices, masks) runs on the
full data on the backend, in the same session as the desktop. Start it from a
cube window with *Tools ▸ Views ▸ Open in VR Headset…*.

For a walk-through, see [From the desktop to the VR headset](../use-cases/08-vr-from-the-desktop.md).

## What you need

- A Meta Quest 3 with its controllers (other WebXR headsets should work).
- VisIVO Visual Analytics on the computer, with the data open in a cube,
  point table, 3-D catalogue or velocity field window.
- For pairing by code: internet on both sides, and the headset and computer
  on a network that lets devices reach each other. On a Mac with Apple
  Silicon, macOS 14 or newer.
- For the USB way: adb on the computer (`brew install android-platform-tools`
  on a Mac) and the headset in developer mode (Meta Horizon app on the phone:
  *Devices ▸ Headset settings ▸ Developer mode*). No internet needed.

Short, seated sessions are the most comfortable; passthrough helps.

## Open a cube from the desktop

*Tools ▸ Views ▸ Open in VR Headset…* (also in Inspector ▸ Analysis) opens
the headset dialog. If a headset page is already open in this session, the
dialog offers *Show on Headset* instead of a new code, and *Pair Another
Headset* for a second headset.

### Pair by code

1. In the dialog, note the code (such as `K7Q-4MX`, valid five minutes) or
   its QR code.
2. On the Quest, open `https://vr.visivolab.oact.inaf.it/` in the Quest
   Browser (or your institute's address), type the code and press *Connect*,
   or scan the QR code.
3. When the cube has loaded on the page, press *Enter VR* (or *Enter
   passthrough* to see the cube in your room).

Once, install the page as an app from the Quest Browser's menu (*Install
app*): it gets an icon in the App Library and opens on the code form.

The dialog follows the pairing up to *Headset connected*. *Disconnect* ends
it; *New Code* replaces an expired or unused code. If the link drops, the
page reconnects by itself for up to three minutes, then offers *Enter a new
code*.

Pairing settings are in *Settings ▸ Backends ▸ Headset Pairing*:

- *Pairing service*, *Headset page*: empty uses the INAF service; fill both
  for your institute's server.
- *Pair headsets by code*: on by default; off opens the USB section directly.
- *Restore Pairing Defaults*: empties the fields and switches pairing on.

Restart the application after a change. Without a working pairing service
the dialog opens *Advanced (USB cable)* and says why.

### Advanced: USB cable

Works offline. Connect the Quest with a USB-C data cable, put it on, and in
the dialog open *Advanced (USB cable)*. The first time, accept *Allow USB
debugging* in the headset (tick *Always allow from this computer*). The
section checks every few seconds and names what is missing (adb not found,
developer mode off, the headset not authorised, offline) with the fix.

- *Open on Headset*: installs or updates the VisIVO VR app when needed and
  starts it straight in VR with the cube (no browser, no *Enter VR*). Without
  the app it opens the page in the Quest Browser.
- *Install*, *Update*: the VisIVO VR app on the headset (*not installed*,
  *older*, *installed and current*).
- *Open in Browser*: the page in the Quest Browser instead of the app.
- *Copy Link*: the page's address, for opening by hand after `adb reverse`.
- *Copy Error*: the error and the adb state for a support request (never the
  token).
- *Retry*: check the headset again now.

Repeat *Open on Headset* after unplugging the cable or restarting the headset.
The headset reaches the backend as `localhost` through adb: a remote backend
must be reached through a local tunnel, as a cluster connection is (see
[Headset with a cluster backend](../../technical/deployment/vr-cluster.md)).

**The link and *Open on Headset* carry the backend's token.** Use them only
with your own computer and headset.

### The VisIVO VR app

The headset page as a Quest app, carried by the release packages. Started
from its icon it shows *No cube yet* until the desktop sends one. Leaving VR
quits it (*Quit VisIVO VR*, or the Quest's system menu). Messages on the
desktop:

- *Put the headset on, then press Open again.*: the headset was asleep.
- *VisIVO VR started but could not connect to this computer.*: check the
  cable and the backend, then *Open on Headset* again.
- *Another VisIVO VR is installed*: *Yes* replaces a build signed by another
  key.

### One page for the whole session

Once a headset page is open, every further *Open in VR Headset…* or *Show on
Headset* goes to it, in VR if it is in VR, with no new code. *Enter VR* is
pressed once per session. A second window with nothing on show pauses and
names the page in use; *Use this window* takes over.

## The headset from the desktop

- *Tools ▸ Views ▸ Show on Headset*: switches the connected headset to this
  window's data. Enabled while a headset is connected; with several, the
  newest is used. For a file whose name ends in `mask` it asks: *Mask Layer*
  (a layer over the headset's cube) or *Show as Cube*.
- *Tools ▸ Views ▸ Headset Activity*: lists in Inspector ▸ Parameters what was
  done in the headset, newest first. Each product has *Open*: moment maps in
  the image viewer, PV slices in this cube's PV view, spectra as a spectrum
  product, Isolate and Remove cubes and refined masks in a new cube window,
  a classified catalogue in the cube's Catalogue table. Products are saved
  in the Workspace Exports and recorded in Session Data ▸ Provenance.
- *Follow the headset's look* (in that panel, on by default): the window
  takes the headset's colour map, threshold and clip box (including the
  velocity range) for its 3-D view. Untick it to keep the window's own look.
- *Watch the Headset* (in that panel): a window with the headset's left-eye
  view, live, about 15 frames a second; the headset shows a *Desktop
  watching* badge. *Snapshot* saves a PNG in the Workspace Exports.
  Passthrough shows as black.
- *Tools ▸ Views ▸ Show Selection in VR Headset* (or *Show in VR Headset* in
  the Kinematic Lasso panel): sends the desktop's lasso selection to the
  headset.

The cube window's status bar says *Headset: in VR · m33.fits* or *Headset:
connected*.

## Controls

The primary hand (right, or left with *Left-handed*) points and acts; the
secondary hand opens the menu. Both grips together move, turn and scale the
cube.

### Your hands

| Control | Primary hand | Secondary hand |
|---|---|---|
| Trigger | the current tool at the ray | none |
| Grip | grab the cube | grab the cube; both grips move, turn, scale |
| Lower button | *A*: the tool palette | *X*: open or close the menu |
| Upper button | *B*: undo the last change of the look | *Y*: reset the view |
| Stick | the mode's control (below) | ←/→ next tool; ↕ colour max |

Undo keeps up to 30 changes of the look. Reset view brings the cube and the
result windows back in front of you.

### Modes

- Explore (Display and Measure tabs): stick ↕ threshold, ↔ opacity; the
  trigger pins the readout card at a point.
- Select (Select tab): the trigger uses *Kinematic Lasso*, *Spectrum probe*
  or *Box region*; stick ↕ threshold.
- Range (Range tab): with *Velocity range*, the trigger centres the range on
  the emission it meets, stick ↕ moves it, ↔ widens it. With *Spatial crop*,
  the trigger drags a face dot, the inside, or (elsewhere) draws a new crop;
  stick ↕ shrinks or grows it. With *Clipping plane* (simulation volumes),
  hold the trigger to carry and tilt the plane, stick ↕ moves it along its
  normal, ↔ sets the slab thickness.

### The tool palette

*A* opens a panel of large icons: *Explore*; *Kinematic Lasso*, *Spectrum
probe*, *Box region*; *Velocity range* (or *Clipping plane*), *Spatial crop*;
*Moment* (computes the M0 map), *PV slice* (starts a path). The trigger
picks; *A* or × closes. Greyed tools do not apply to the data shown.

### The hand menu

*X* (or palm up with tracked hands) opens the menu about a metre in front of
you; it stays until *X* or its × closes it. Drag its title bar to move it.
Controls that do not apply are greyed with a one-line reason. The footer has
*Undo* and *Reset view*.

Display, for a cube:

- *Colour map*: swatches.
- *Threshold*: slider in the cube's unit and in σ; *3σ*, *5σ* (on a
  simulation volume *90%*, *99%*); *Reset*.
- *Colour max*; *Levels ▸*: a histogram (drag to set the threshold),
  *1σ* to *10σ*, percentiles *90%* to *99.99%*, *Stretch* (*Linear*, *Log*,
  *Sqrt* default, *Square*, *Power*) and *γ −*, *γ +*.
- *Opacity*.
- *Rendering*: *Emission* or *MIP*; *Surfaces ▸*; on a simulation volume also
  *Overlay ▸* and *Explode ▸* (below).
- *Catalogue*: *Overlay*, *Labels*, *Through velocity* (once a catalogue is
  placed). On a simulation volume with several fields this row is *Variable*.
- The info line: rest frequency, channel width, beam.

σ is the full cube's noise as *Estimate Noise* measures it by default. The
drawn cube is binned, so the menu also gives the threshold in σ of the drawn
cube.

*Surfaces ▸* (any cube): up to four isosurfaces computed by the backend on
the original data.

- *+ Add*, *Remove*; the numbered chips pick the surface to set.
- *Level* (in the field's unit) and *Threshold* (back to the volume's).
- *Colour*, *Opacity*, *Shown*/*Hidden*.
- *Show*: *With the volume* or *Surfaces only*.
- *Export*: *PLY* or *glTF* in the Workspace Exports (`isosurfaces/`).

Range:

- *Velocity range* (*Spectral range* without a velocity axis) and *Full band*.
- *Trigger*: *Velocity range* or *Spatial crop*.
- *Crop x*, *Crop y*, *Crop z*, *Reset crop*.
- On a simulation volume: *Plane* (*On*/*Off*, *Flip*, *Snap*, *Slab*,
  *Reset*) and the *Clipping plane* trigger tool.

Select:

- *Tool*: *Kinematic Lasso*, *Spectrum probe*, *Box region* (*Box select* on
  a point table).
- *Lasso*: *New*, *Add*, *Subtract* (what the next seed does).
- *Selection*: *Clear*.
- *Result*: *Isolate*, *Remove*, *Show result cube* (on a point table *Show
  inside*, *Hide inside*, *Show all*).
- *Sources*: *List*, *◂ Prev*, *Next ▸*, *+ Found*.
- *Mask*: *Mask*, *Pick*, *Undo*, *Save*.

Measure:

- *Moment*: *M0*, *M1*, *M2*, *Hide*.
- *PV slice*: *Draw*, *Undo*, *New*, *Hide*; *PV width −*, *PV width +*.
- *Pin spectrum* / *Unpin spectrum*: keeps a spectrum to compare with the
  next ones.

More: *Handedness* (*Right-handed*, *Left-handed*), *Button labels*,
*Haptics*, *Wrist readout*, *Guide* (*Show the guide*).

Velocity stretch, binning and Kinematic Lasso parameters are set on the 2-D
page before entering VR (see
[The settings on the page](../../technical/vr/reference.md#the-settings-on-the-page)).

### Wrist cards, readout and windows

- Wrist cards (look at a wrist as at a watch): the primary wrist lists what
  each control does now, the secondary the cube, mode and selection.
- The cursor readout at the ray: sky position, velocity and channel, value
  (*binned* on the reduced cube).
- Result windows (*Spectrum probe*, *Kinematic Lasso*, *Box region*, *Moment*,
  *PV diagram*, *Sources*, *Mask*): drag the title bar to move, × to close.

### Hand tracking

Put the controllers down and the page uses your hands: pinch (primary hand)
is the trigger, a fist is the grip, palm up (secondary hand) opens and closes
the menu. Stick and button actions are on the menu. Hand tracking has only
been tested on synthetic hands so far.

## Analysis in the headset

The backend computes everything on the full cube with the desktop's
methods.

### Selections in the headset

- *Spectrum probe*: the spectrum at the picked pixel, against velocity.
- *Kinematic Lasso*: a selection grown from the picked seed (desktop
  defaults), drawn green. The window gives voxels, flux in Jy km/s with its
  uncertainty, peak and integrated S/N, W20, W50, Vsys, centroid and size, as
  SoFiA-2 defines them.
- *Box region*: two corners; statistics of every voxel in the box. A corner
  placed with the controller tip inside the cube has depth; one pointed from
  outside spans the velocity range.
- *Isolate* (outside blanked) and *Remove* (selection blanked) write a new cube
  to the Workspace Exports; *Show result cube* shows it.
- *M0*, *M1*, *M2*: the moment of the lasso selection (whole band, no
  threshold mask), or with no selection of the whole field over the velocity
  range (the desktop's automatic mask).
- *PV slice ▸ Draw*: each trigger adds a path point; the diagram updates from
  two points on. Width in pixels and arcsec.

Each result is recorded as a `visivo-recipe/1` record (*Download recipes
(.json)* on the page).

### Mask layers

A mask cube on the same grid as the cube (SoFiA-2's `*_mask.fits`, for
example) is sent with *Show on Headset ▸ Mask Layer*. *Select ▸ Mask ▸ Mask*
opens its window: *Outline*, *Mask only*, *Residuals*, *Off*, *Labels*.
*Pick* then the trigger picks a source; the Kinematic Lasso edits it
(*Replace*, *Add*, *Subtract*), *New* starts a new source, *Undo* takes back
an edit. *Save to desktop* writes `<mask>_vr_mask.fits` with the same grid
and IDs.

### Selections shared with the desktop

After *Open in VR Headset…* in a cube window, a lasso or box made in the
headset appears in that window as its Kinematic Lasso selection, with the
same voxel count and flux. One selection per cube is shared, the latest from
either side. Received selections are not regrown by the desktop's sliders.

## Data types

### Cubes

The axes are R.A., Dec. and velocity in km/s, from the full WCS. By default
one channel is drawn as deep as one spatial pixel; the label over the box
gives the scale and the binning in use.

### Simulation volumes

A gridded field (a VBT volume field, a PLUTO variable, a FITS cube with no
sky or spectral axes) opens as a simulation volume. A positive field spanning
more than three decades is shown on a log scale; the box is drawn to the grid
spacing; spectral tools are greyed; the default threshold is the 99th
percentile.

- *Variable*: one chip per field on Display; each keeps its own look.
- *Overlay ▸*: two or three variables of the grid in one view. *Fields* adds
  or removes one; *Edit* picks the one set below (colour map, *Log*,
  threshold, colour max, opacity, *Shown*/*Hidden*); *Sum* (default) or
  *MIP*; *On*/*Off*. One colour bar per field; the readout gives each value.
- Clipping plane (Range tab): hides one side of the volume and draws the
  section on it; *Slab* keeps a slab between two parallel planes.
- *Explode ▸*: pulls the model apart. *Pieces*: *Fields* (the overlaid
  fields) or *Value ranges* (*2*, *3* or *4* ranges between threshold and
  colour max); *Layout*: *Row*, *Column*, *Ring*; *Spread*; *Shown* per piece;
  *Explode*/*Assemble*; *Reset*. A grip inside a piece moves that piece.
- Isosurfaces: *Surfaces ▸*, as for any cube; exports are in the simulation's
  own coordinates (code units of a PLUTO run).

### Point tables

Point tables (VBT, 3-D catalogues, snapshots) open with *Tools ▸ Open in VR
Headset…* in their window; the headset draws a sample of the rows, the backend
refines a still view with all of them. Display has *Colour by*, *Log* and
the colour window, *Colour map*, *Points*/*Splats*/*Ray-traced*, *Size −*,
*Size +*, *Intensity*, *Particles shown*, *Again*, *Box*, *Data ▸*.
*Data ▸* has *Snapshot* (*◂ Previous*, *Next ▸* through numbered files),
*X*/*Y*/*Z* columns, *Splat size* (*Uniform*, *By hsml*, *Column ▸*),
*Default axes*. Select has *Box select*.

### Catalogues over a cube

On the page, *Catalogue over the cube* places a catalogue file, VLKB compact
sources or filaments, or a cross-match on the cube with its full WCS. A source
with a spectral column is placed at its channel. *Select ▸ Sources ▸ List*
opens the source list: sort, visit (*◂ Prev*, *Next ▸*), classify with flags
and notes, add sources found by eye (*+ Found*), and *Send* a VOTable with a
`vr_flag` column to the desktop.

### Velocity fields

From the velocity viewer's *Open in VR Headset…*. Display has *Streamlines*,
*Arrows*, *Shorter*/*Longer*, *Fewer*/*More* and *Reseed streamlines*, the
speed colour window, *Colour map*, *Box and ticks*. Lines are one pixel wide
and not refined.

## Limits

- One dataset at a time; the reduced cube is at most 2048 voxels a side and
  64 Mi voxels: choose a larger binning.
- No hand editing of a mask channel and no kinematic fit in the headset.
- A shared selection is at most 16 Mi voxels in its bounding box.
- Refined frames are off in passthrough.

## Troubleshooting

- The page and the dialog name each problem and its fix; the list is in
  [Troubleshooting](../../technical/vr/troubleshooting.md).
- *Backend not reachable* over USB: press *Open on Headset* again (the port
  forwarding ends when the cable is unplugged).
- *This cube is too large for this device*: a larger spatial or spectral bin.
- *The graphics were reset* or the page crashes at the first lasso on a
  Quest 3: see
  [GPU context loss](../../technical/vr/troubleshooting.md#gpu-context-loss-at-the-first-lasso-on-a-quest-3)
  and [Diagnosing a GPU context loss](../../technical/vr/troubleshooting.md#diagnosing-a-gpu-context-loss).
- For support, *Copy diagnostics* on the page.

Details: [VR headset: methods and implementation](../../technical/methods/vr.md),
[VR technical reference](../../technical/vr/reference.md),
[Hosting headset pairing](../../technical/deployment/vr-pairing.md).
