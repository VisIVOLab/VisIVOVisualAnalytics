# Particle window: compare quantities in panes

The 3-D window of a simulation's particles (a Gadget snapshot, a VBT table,
an IPAC or CSV table treated as particles) compares quantities side by side
in one window, with the same panes the [cube viewer](cube-viewer.md) has.
Every pane shows the **same loaded particles**, coloured by a different
quantity.

## Add a pane

*Tools ▸ Compare Quantity…*, or *Compare another quantity* in the strip above
the panes, adds a pane. The layout grows from one pane to two and then to
four, and the new pane takes the next quantity that is not shown yet
(density, temperature, entropy, then other physical columns; coordinates and
velocity components are skipped). The new pane opens on the camera and the
selection of the pane you were looking at.

The *1 / 2 / 4* toggle in the strip changes the layout. Going back to one
pane keeps the active pane's view. With four panes, close one (see below)
before adding another.

## What a pane has

- **Header**: a ● that is filled and blue on the *active* pane, the
  quantity with a ▾ menu, and how it is drawn (points, splats or
  *ray-traced*, and *on backend* when it is streamed). Click anywhere in a
  pane to make it active. The left dock shows the controls of the active pane:
  colour map, range and scale, shape, size, render budget and ray-traced
  light.
- **▾ menu**: *Colour by* (the quantity), *Linked to the other panes*,
  *Pop out to a window* or *Dock back into the grid*, and *Close this pane*.
  The first pane belongs to the window; closing it closes the window.
- **Linked / Unlinked** button: a linked pane follows the camera and the
  selection of the others. Unlink it to explore on its own; link it again and
  it takes the others' view.
- **⤢ / ⤡**: fills the workspace with this pane, and restores it. The
  *1 / 2 / 4* toggle also leaves it.
- **⤴ / ⤵** (or a double click on the header): the pane moves to a window of
  its own, for a second screen, and the grid keeps its place with a
  *Pane N · detached* card. Closing that window, or *Bring back*, docks it
  again. A popped-out pane stays linked.

Quantity, colour map, colour range and scale belong to each pane. Camera and
selection are shared while panes are linked; *Match rendering* in the
comparison card also shares shape, size, render budget and ray-traced light
(see [Compare physical quantities](gadget-snapshots.md#compare-physical-quantities)).

## Session Data

*Session Data* in the left dock lists, for the particle window:

- the particle type being drawn, with its row count;
- one *quantity* row per pane. Click a row to make that pane active (a pane the
  layout hides comes back with a four-pane layout); the number on the row says
  which pane it is;
- the SubFind catalogue of the same session when its window is open: click the
  row to raise that window.

## Memory

The particles of a window are loaded once. A pane does not copy them: all the
panes of a window look at the same rows, the same positions and the same
vertex data, and only the arrays that differ per pane (the colour values and
the size vectors) are its own. In a test with a 3.9-million-row table, four panes
needed about 7 % more memory than one (the colour and size arrays of the three
added panes), where four separate copies would have needed 75 % more. After a filter
the panes still share: on the same table filtered to about half its rows, four panes
took about 7 % more than one (1351 MB against 1261 MB), where four panes that each
asked for the filtered rows took 3.4 times as much (4339 MB). A pane whose view is not on screen
draws nothing, and a ray-traced pane renders only when it is shown.

What a pane does cost is mostly on the graphics card: each pane's view has its own
buffers, whatever the rows are. Two things keep that small. Panes side by side, and
Gaussian splats in any layout, are drawn without hardware anti-aliasing (a lone pane of
solid shapes keeps it): every anti-aliasing sample multiplies the colour and depth
buffers of the view. And a view that stays off screen for a couple of seconds gives its
graphics memory back and makes it again when it is shown: a pane the layout hides (the
other panes of a four-pane window shown as one), the local view while the ray-traced or
streamed one is on, and the ray-traced view's particles and float image once ray tracing
is turned off or its pane is hidden (they are fetched again from the backend when it is
shown). A streamed pane (*View ▸ Rendering ▸ Remote*) that stays hidden for a couple of
seconds also gives its backend session back and asks for a new one, with its camera
and settings, when it is shown again. A pane that is hidden does not draw when the
camera of the group moves; it shows the group's camera when it is shown.

In Gaussian splats and Point shapes the per-row size vectors are not filled, since
those shapes read none; they are filled when a glyph shape is chosen.

Axis mapping is per pane: a pane whose axes differ from the first pane's
computes its own positions; panes with the same axes share them.

## Filters

The filters are the group's. A filter applied in any linked pane gives every
other linked pane of the table the same rows, from one query; a pane that is
still loading takes them as soon as it is free, and a pane that you link again
takes the filters of the others. An unlinked pane keeps its own filters, and a
filter set in it does not reach the others.

The filter controls of the Inspector (*Analysis ▸ Filters*: Add, Remove, Clear All,
Apply) belong to the active pane and change with it. Applying or clearing from an
unlinked pane filters that pane only; from a linked pane it filters every linked
pane.

## Saved layout

The layout, the quantity of each pane (by column name), the slot each pane sat in,
the linked flags and the active pane are remembered for each table: the next window on it offers
the same comparison, with every pane back in its slot, and says so in the status bar.
Choosing another pane to work in is remembered too, not only layout changes. A two-pane
layout keeps whether it was one above the other or side by side; it is laid out again for
the width of the window it is opened in (side by side in a wide window, one above the
other in a narrow one), with each pane keeping its quantity. The first pane in the
saved list is the one of the window itself, wherever it sat in the grid. A layout saved
by an earlier version has no slots and comes back with the panes in the order they were made.
A table opened with fewer than two panes remembers nothing.

## Known limits

- The headset shows the quantity of the first pane; following the pane you
  make active on the desktop is not done yet.
