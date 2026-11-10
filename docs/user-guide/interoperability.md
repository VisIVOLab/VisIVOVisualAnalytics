# Working with TOPCAT, Aladin and DS9 (SAMP)

VisIVO Visual Analytics exchanges data with the other Virtual Observatory
tools on your machine through **SAMP**, the protocol TOPCAT, Aladin, DS9 and
most VO tools speak. Tables, images, cubes, sky positions and selections of
rows go both ways, and a table that went from one tool to the other stays
linked row by row.

This page is the whole workflow. The reference for what each message does in
the catalogue viewer is in [Catalogues · SAMP](catalogues-hips.md#samp); the
backend routes are in the [SAMP API reference](../backend-api/samp.md).

## Connect

SAMP needs a **hub**, a small program the tools register with. TOPCAT and
Aladin start one when they run (DS9 connects to a running one); a standalone
`samp_hub` (astropy) works too. Start one of them first, then:

1. In the Data Hub window, choose **SAMP ▸ Connect To Hub**. Every viewer
   window also has a **SAMP** menu, with *Connect To Hub* while VisIVO is not
   connected.
2. The first line of the Data Hub's SAMP menu says *Connected as VisIVO [c3],
   receiving*. VisIVO now appears in the other tools' send menus (TOPCAT's
   *Send table to…*, *Send Sky Coordinates*).

If no hub is running yet, VisIVO says so once and keeps trying every few
seconds; start TOPCAT and it connects by itself. If the hub goes away (TOPCAT
quit), VisIVO reconnects when one is back. *Disconnect From Hub* stops this.

## Send from a viewer

Each viewer's **SAMP** menu sends that window's data, not the last dataset
opened. Every *Send … To* item opens a list: **All Tools**, or one tool by
name. Only the tools that declared they take that kind of data are listed;
two copies of one tool are told apart by their hub id (*topcat [c4]*).

| Window | SAMP menu | Goes out as |
|---|---|---|
| Image viewer | *Send Image To* | `image.load.fits` of the image's file |
| | *Send Clicked Position To* | `coord.pointAt.sky` of the last pixel you clicked |
| Cube viewer | *Send Cube To* | `image.load.fits` of the cube's file |
| | *Send Clicked Position To* | `coord.pointAt.sky` of the last pixel clicked on the channel map or the moment map |
| 3-D catalogue | *Send Catalogue To* | `table.load.votable` (or `table.load.fits` for a FITS table) |
| | *Send N Selected Rows To* | a new VOTable with those rows only |
| | *Send the N Rows Shown To* | a new VOTable with the rows loaded in the window (after a filter, a sample, a partial load) |
| | *Select These Rows in Other Tools* | `table.select.rowList` on the table the other tools already hold |
| | *Send Source Position To* | `coord.pointAt.sky` of the selected source's RA/Dec |

The Data Hub's own SAMP menu keeps *Send Current FITS Dataset* and *Send
Current Catalogue*, which send the last dataset opened.

Before preparing a file, VisIVO asks the hub whether any tool takes it, and
says so at once if none does. The file is then staged on the backend and
offered to the tools; each tool's answer (*SAMP: topcat loaded
NKATZ_2.3M.vot*, or its error) is shown in the status bar of the window you
sent from and in **Diagnostics**. A large table takes a while to prepare
(about 40 s for 2.3 million rows); above a million rows VisIVO asks first.

Which tool takes what depends on the tool: TOPCAT takes tables, not images;
Aladin and DS9 take images and sky positions. The *Send … To* list shows what
each running tool has declared.

## Send a selection of rows

In the 3-D catalogue window, select rows in the **Catalogue Data** table: a
click, Shift-click for a range, Cmd-click (Ctrl-click on Linux and Windows)
for more, Cmd/Ctrl-A for all. With no rows selected in the table, the source
selected in the view is the selection. Then, from the SAMP menu or the
table's context menu (right-click):

- **Send N Selected Rows To** sends a new VOTable holding those rows, in the
  order of the original table, with the original's columns (a VOTable or FITS
  table keeps its units, a VOTable its UCDs too). It is named after the catalogue and the row
  count (*cat_12rows.vot*).
- **Send the N Rows Shown To** (SAMP menu) sends every row the window holds,
  when that is fewer than the table: after a filter, for a large table loaded
  as an even sample ("100000 of 3931546 rows"), or before *Load all*.
- **Select These Rows in Other Tools** selects the same rows in the tables the
  other tools already hold of this catalogue (the one it came from, and every
  copy VisIVO sent). Nothing new is sent; TOPCAT applies it as a row subset.

A subset VisIVO sent stays linked to the catalogue: clicking its row 3 in
TOPCAT selects the source it came from in VisIVO, and clicking a source in
VisIVO highlights its row in the subset too, when the subset has it.

A catalogue combined from several files cannot be sent, whole or in part: its
rows are numbered across all its files, which no single table is.

## Send a sky position

- **Image and cube:** click a pixel (a press and release without dragging;
  not while drawing a region, a PV path or editing a lasso mask), then
  *SAMP ▸ Send Clicked Position To*. The status bar says what was sent:
  *SAMP: sent pixel 512, 384 (RA 150.012345°, Dec +2.204000°)*.
- **3-D catalogue:** select a source, then *Send Source Position To*. The
  catalogue's RA/Dec columns are sent; a catalogue of X/Y/Z or l/b columns has
  no RA/Dec to send.
- **Send Positions of Clicked Pixels** (image, cube) and **Send Positions of
  Clicked Sources** (catalogue) are switches, off by default: while on, every
  click sends its position to all tools, so Aladin or DS9 follow you around
  the field. They apply to that window only.

Positions go out in ICRS degrees. The backend places the pixel with the
file's full WCS: its projection, rotation (CD or PC matrix), distortions
(SIP) and frame, so galactic and ecliptic images are rotated into ICRS too.
The viewer's own coordinate readout uses the linear axes, so it can differ
from the position sent away from the image centre; the status bar says what
was sent. A backend older than this client cannot do this: VisIVO then
places the pixel itself, from the reference value, reference pixel and pixel
size of each axis (gnomonic for TAN, orthographic for SIN, the linear axes
otherwise), without rotation or distortion, and the status bar says so.

## Receive

| From the other tool | What VisIVO does |
|---|---|
| a table (`table.load.votable`, `table.load.fits`) | opens it in the 3-D catalogue viewer |
| an image (`image.load.fits`) | opens it in the image or cube viewer |
| a sky position (`coord.pointAt.sky`) | centres the HiPS viewer on it |
| a row highlight (`table.highlight.row`) | selects that source, as a click would |
| a row selection (`table.select.rowList`) | marks those sources with orange circles |

Received data is shown under the name the sender gave it, and is not added
to *Recent Datasets* (the backend keeps it as a temporary copy).

## Link rows with TOPCAT

A table that went from one tool to the other, in either direction, stays
linked: a click on a row in TOPCAT selects the source in VisIVO and centres
the view on it, a click on a source in VisIVO highlights its row in TOPCAT,
and a subset TOPCAT transmits is marked in VisIVO. Rows are matched by their
place in the original table, so filters, sorts and samples do not break the
link; a row not loaded in VisIVO is reported in the status line rather than
guessed. The details are in [Catalogues · SAMP](catalogues-hips.md#samp).

## With a remote backend

Everything above works with a backend on a cluster:

- **Receiving:** the hub hands out addresses on your machine
  (`http://127.0.0.1:2525/…`), which a remote backend cannot reach. VisIVO
  downloads the file itself and uploads it to the backend (up to 512 MB).
- **Sending:** the other tools fetch the file from the backend's address as
  VisIVO knows it. Through the SSH tunnel VisIVO opens for a cluster that
  address is on your machine (`http://localhost:…`), so the tools reach it
  while the tunnel is up. The file stays available for an hour.

## Troubleshooting

**"Not connected: waiting for a hub".** No hub answers. Start TOPCAT or
Aladin. A hub that crashed can leave a stale `~/.samp` file behind; remove it
if nothing connects even with TOPCAT running.

**"Connected …, cannot receive".** VisIVO is registered but the hub cannot
call it back; the tooltip of that line says why. VisIVO can still send.
Disconnect and connect again.

**"No running tool takes …".** No tool declared that kind of data: TOPCAT
alone does not take images; start Aladin or DS9.

**An item is greyed out.** Its tooltip says why: not connected, no pixel
clicked yet, no sky axes, no rows selected, a file still being prepared, or a
combined catalogue.

**"This backend cannot send a selection of rows".** The backend is older than
the client and would send the whole table; update it, or send the whole
catalogue.

**A position was sent twice, or late.** Messages the backend queues for SAMP
are delivered at least once: one that timed out is sent again after a minute.
They also wait for a connected VisIVO to send them.

**The other tool says it cannot load the file.** With a remote backend, the
tool must reach the backend's address: check that the tunnel is still up
(the status rail's backend chip), and that the file was sent within the hour.

**A highlight from TOPCAT is "not shown here".** The row is filtered out or
not loaded yet in VisIVO: *Load more* or *Load all* may bring it.

**Spectra.** Spectra (`spectrum.load.*`) are not exchanged yet, in either
direction.

Every connection, message sent or received and failure is listed in
**Diagnostics** (*View ▸ Diagnostics*), under *SAMP*.
