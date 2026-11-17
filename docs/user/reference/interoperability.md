# Working with TOPCAT, Aladin and DS9 (SAMP)

VisIVO Visual Analytics exchanges tables, images, cubes, sky positions and row
selections with TOPCAT, Aladin, DS9 and other Virtual Observatory tools on your
machine through SAMP. The *SAMP* menu is in the Data Hub and in every image,
cube and 3-D catalogue window. A summary is also on
[Catalogues, HiPS, and SAMP](catalogues-hips.md).

## Connect

SAMP needs a hub. TOPCAT and Aladin start one when they run; DS9 connects to a
running one; astropy's `samp_hub` also works.

- *SAMP ▸ Connect To Hub*: registers with the hub. VisIVO then appears in the
  other tools' send menus (TOPCAT's *Send table to…*, *Send Sky Coordinates*).
  The first line of the menu gives the state: *Not connected*, *Connected as
  VisIVO [c3], receiving*, or *Connected as VisIVO [c3], cannot receive* (the
  tooltip says why).
- *SAMP ▸ Disconnect From Hub*: unregisters and stops reconnecting.

With no hub running, VisIVO says so once and retries every few seconds; it
also reconnects when a hub that went away comes back. It does not reconnect
after *Disconnect From Hub*, or when the hub itself disconnects it.

## Send from a viewer

A viewer's *SAMP* menu sends that window's data. Each *Send … To* item offers
*All Tools* or one tool by name, listing only the tools that take that kind of
data (two copies of a tool are told apart by hub id, *topcat [c4]*).

| Window | SAMP menu | Goes out as |
|---|---|---|
| Image viewer | *Send Image To* | `image.load.fits` of the image's file |
| | *Send Clicked Position To* | `coord.pointAt.sky` of the last pixel clicked |
| Cube viewer | *Send Cube To* | `image.load.fits` of the cube's file |
| | *Send Clicked Position To* | `coord.pointAt.sky` of the last pixel clicked on the channel or moment map |
| 3-D catalogue | *Send Catalogue To* | `table.load.votable` (`table.load.fits` for a FITS table) |
| | *Send N Selected Rows To* | a new VOTable with those rows |
| | *Send the N Rows Shown To* | a new VOTable with the rows loaded in the window |
| | *Select These Rows in Other Tools* | `table.select.rowList` on the tables the other tools already hold |
| | *Send Source Position To* | `coord.pointAt.sky` of the selected source |

The Data Hub's menu has *Send Current FITS Dataset* and *Send Current
Catalogue*, which send the last dataset opened, not the window in front.

If no running tool takes the data, VisIVO says so (*No running tool takes …*).
Otherwise each tool's answer is shown in the status bar and in *View ▸
Diagnostics* (*SAMP: topcat loaded NKATZ_2.3M.vot*). TOPCAT takes tables,
not images; Aladin and DS9 take images and sky positions. Above a million rows
VisIVO asks before sending, and preparing a large table takes a while (about
40 s for 2.3 million rows).

## Send a selection of rows

In the 3-D catalogue window, select rows in the *Catalogue Data* table (click,
Shift-click for a range, Cmd-click or Ctrl-click to add, Cmd/Ctrl-A for all).
With no rows selected, the source selected in the view is the selection. Then,
from the SAMP menu or the table's right-click menu:

- *Send N Selected Rows To*: a new VOTable with those rows, in the original
  order and with the original columns (units and UCDs kept where the source
  had them), named after the catalogue and the count (*cat_12rows.vot*).
- *Send the N Rows Shown To*: every row the window holds, when that is fewer
  than the table (after a filter, an even sample of a large table, or before
  *Load all*).
- *Select These Rows in Other Tools*: selects the same rows in the copies the
  other tools already hold. Nothing new is sent; TOPCAT applies it as a subset.

A subset sent this way stays linked to the catalogue. A catalogue combined from
several files cannot be sent.

## Send a sky position

- Image and cube: click a pixel (without dragging, and not while drawing a
  region, a PV path or a lasso mask), then *Send Clicked Position To*. The
  status bar shows the pixel and the RA and Dec sent.
- 3-D catalogue: select a source, then *Send Source Position To*. Needs RA and
  Dec columns.
- *Send Positions of Clicked Pixels* (image, cube) and *Send Positions of
  Clicked Sources* (catalogue): switches, off by default, that send every click
  to all tools, so Aladin or DS9 follow you. They apply to that window only.

Positions go out in ICRS degrees, computed by the backend with the file's full
WCS (projection, rotation, SIP distortion, frame), so away from the image
centre they can differ from the viewer's own linear readout. The status bar
says what was sent.

## Receive

| From the other tool | What VisIVO does |
|---|---|
| a table (`table.load.votable`, `table.load.fits`) | opens it in the 3-D catalogue viewer |
| an image (`image.load.fits`) | opens it in the image or cube viewer |
| a sky position (`coord.pointAt.sky`) | centres the HiPS viewer on it, opening one if needed |
| a row highlight (`table.highlight.row`) | selects that source, as a click would |
| a row selection (`table.select.rowList`) | marks those sources with orange circles |

Received data is shown under the name the sender gave it and is not added to
*Recent Datasets*, because the backend keeps only a temporary copy: send it
again from the other tool.

## Linked rows

A table that went from one tool to the other, in either direction, stays
linked: a click on a row in TOPCAT selects and centres the source in VisIVO, a
click on a source in VisIVO highlights its row in TOPCAT, and a subset TOPCAT
transmits is marked in VisIVO. Rows count from 1, as in TOPCAT, and filters,
sorts and samples do not break the link. A row not loaded in VisIVO is
reported in the status line. While a large table is drawn on the backend,
selections show only with *View ▸ Rendering ▸ Local*.

## With a remote backend

- Receiving: the hub's addresses point to your machine, so VisIVO downloads the
  file and uploads it to the backend (up to 512 MB).
- Sending: the other tools fetch the file from the backend through the SSH
  tunnel VisIVO opened, so the tunnel must be up. A file stays available for
  one hour.

## Troubleshooting

- *Not connected: waiting for a hub*: start TOPCAT or Aladin; if that does not
  help, remove a stale `~/.samp` file left by a crashed hub.
- *Connected …, cannot receive*: the hub cannot call VisIVO back (see the
  tooltip). Sending still works; disconnect and connect again.
- A greyed-out item: its tooltip gives the reason.
- *This backend cannot send a selection of rows*: update the backend, or send
  the whole catalogue.
- A position sent twice or late: queued messages are delivered at least once,
  and one that timed out is sent again after a minute.
- The other tool cannot load the file: with a remote backend, check the tunnel
  (the backend chip in the status rail) and that the file is less than an hour
  old.
- Spectra (`spectrum.load.*`) are not exchanged.

Every connection, message and failure is listed in *View ▸ Diagnostics* under
*SAMP*. Details: [SAMP API](../../technical/backend-api/samp.md).
