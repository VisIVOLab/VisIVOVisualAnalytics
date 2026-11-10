# Changelog

Development history for VisIVO Visual Analytics.

These notes were kept in the README until the README became a front door for
people who want to *use* the software rather than follow its development. They
are preserved here unedited; the branch names they refer to are historical.

## Unreleased

- **Cubes in a VR headset from a Mac (#167, first step of #166).** The backend
  serves a WebXR page at `/vr/` that a Meta Quest 3 opens in its browser
  (over `adb reverse`, so the page is `localhost` and needs no certificate;
  over Wi-Fi with adb connected wirelessly). The headset's GPU ray-marches a
  reduced copy of the cube (WebGL2, front to back with early termination,
  empty-space skipping, a frame-rate governor), with threshold, colour map,
  clipping box and a velocity slab on the controllers, and grab to move,
  turn and scale. Axes in world coordinates and the velocity axis in km/s
  with its scale against the sky written out; the binning in use is written
  over the cube. A panel shows FPS, frame times, GPU time, steps per ray and
  the eye-buffer size; a built-in benchmark measures four cube sizes at three
  sampling rates. The same page works in a desktop browser without a
  headset. The token is read from the URL fragment, which is never sent to
  the server.
  - New routes `POST /v1/cube/reduced_info` (factors, default threshold, world
    coordinates of the reduced voxels) and `POST /v1/cube/reduced_bin` (the
    cube binned by mean or max, in 8 or 16 bits, read in bounded memory).
  - `validation/vr_reduction_fidelity.py` measures, on HCG 44 and WALLABY
    NGC 3137 against their reference masks, the flux and voxels each binning
    still draws above the threshold. See *Cubes in a VR headset* in the user
    guide.

- **SAMP from the viewer windows (#130).** The image, cube and 3-D catalogue
  windows have a **SAMP** menu that acts on that window's data, not on the
  last dataset opened, with a *Send … To* choice of recipient: all the tools
  that take it, or one of them by name (from the hub's list, two copies of one
  tool told apart by id; a call to one tool is a `samp.hub.call`). The Data
  Hub's SAMP menu is unchanged.
  - *Send Image To*, *Send Cube To*, *Send Catalogue To*; the progress and
    each tool's answer show in the status bar of the window sent from.
  - **Subsets:** rows selected in the catalogue table (it now takes several:
    Shift, Cmd/Ctrl, Select All), or every row the window holds after a
    filter, a sample or a partial load, go out as a new VOTable
    (`row_indices` on `POST /v1/samp/send-catalogue`, which keeps units and
    UCDs and says `subset: true`; an older backend that would send the whole
    table instead is refused). The subset stays linked: its rows are mapped
    to the catalogue's in highlights and selections, both ways. *Select These
    Rows in Other Tools* sends `table.select.rowList` to the tables the other
    tools already hold of the catalogue. Also in the table's context menu.
  - **Positions:** *Send Clicked Position To* sends the last pixel clicked on
    an image, a cube's channel map or its moment map as `coord.pointAt.sky`
    in ICRS, placed by the backend with the file's full WCS (new `POST
    /v1/datasets/pixel_to_sky`: rotation, CD/PC, SIP, every projection, the
    frame); an older backend gets the client's own TAN/SIN/linear conversion,
    without rotation or distortion, and the status bar says so. *Send Source
    Position To* sends the selected catalogue source's RA/Dec. *Send
    Positions of Clicked Pixels/Sources*, off by default, sends every click
    that no tool (region, PV, lasso, probe, measurement, annotation,
    catalogue pick) takes.
  - **The backend's outbound queue no longer loses messages.** `GET
    /v1/samp/pending?lease=true` keeps each message until the client
    acknowledges it (`POST /v1/samp/pending/ack`) once the hub has taken it;
    one the client fetched and failed to send comes back after 60 s, so
    delivery is at least once. Without `lease` the queue drains as before,
    for older clients.
  - New user-guide page, [Working with TOPCAT, Aladin and
    DS9](docs/user-guide/interoperability.md). Spectra
    (`spectrum.load.*`) are not exchanged yet.
- **Quitting cancels a ray-traced render (#149).** A still or movie being
  rendered by the backend (`POST /v1/tasks/volume_render`,
  `/v1/tasks/particle_render`) kept running after the application quit: its
  DELETE was sent from the viewer's destructor, after the event loop had
  ended, and never left, so on a remote or cluster backend the render ran to
  its end. The render task is now held by a handle registered with the quit
  drain (`src/app/RenderTaskHandle.h`; `TaskQuitDrain` gains `QuitCanceller`
  for followers that run on the GUI thread's event loop): on ⌘Q or the Dock's
  *Quit* the drain sends `DELETE /v1/particle_render/{id}` from a thread of
  its own, bounded to 3 s, before the application exits, exactly once; a
  completed still held for regrading is removed the same way. The create now
  runs on a worker, so a create still answering at quit is waited for and its
  render deleted, and no render is created once quitting has begun. The
  render creates had the 5-minute timeout of a task status read (their
  paths look like one), which a quit lets run out; every task create now
  keeps 10 s, checked against the backend's routers. Tests:
  `TestTaskQuitDrain` (fake backend) and `VisIVOQuitExitRender` /
  `VisIVOQuitExitRenderWhileQuitting` (a process of their own, `exit()` while
  the render is polled on the application thread).
- **Cosmology and skeleton fixes from the scientific review (#129).**
  `GET /v1/cosmology/distances` under de Sitter (Ωm = 0, ΩΛ = 1) answered
  HTTP 500 because its age is infinite; a non-finite quantity is now `null`
  and the rest of the answer is kept, on every cosmology route. A request body
  carrying `NaN`/`Infinity` into a field that refuses it now gets its 422
  instead of a 500. `POST /v1/cosmology/distance` names the proper transverse
  scale `scale_kpc_per_arcsec`; `proper_motion_kpc_per_arcsec` is a deprecated
  alias with the same value for one release. The skeleton's
  `persistence_threshold` (an intensity cut, not a topological persistence,
  as the API docs now say) is honoured at 0 and below instead
  of being ignored, and the response echoes the cut applied (a recipe
  recorded by an earlier desktop with a ticked cut ≤ 0 now replays with that
  cut applied). `POST /v1/cosmology/distance` and its batch refuse z above
  10 000, where the integrals returned 0 Mpc. The skeleton's
  smoothing was already normalised over the finite pixels (#140); a test now
  pins that a blank area draws no false void along its edge. The desktop
  reads the new scale field (falling back to the old one), computes de Sitter
  instead of refusing it, and its skeleton tooltips no longer say that blanks
  count as 0 or that cuts ≤ 0 are ignored.

- **Saying when a backend is older than the client.** A backend that predates
  a feature answers its routes with FastAPI's bare `404 {"detail":"Not Found"}`,
  which the ray-traced views showed as it was ("No camera for the ray-traced
  view: Not Found" on a cluster, "Ray-traced view: Not Found" with the local
  backend, the check left on over a black view). They now say *The backend on
  <name> does not have ray-traced views (it is older than this client); update
  it.*, with the backend's name as the status bar gives it, and turn the view
  off. Covered: the camera (`GET /v1/render/{id}/camera`), the grid and the
  particles for the GPU view and the frames without one
  (`/v1/volume_render/grid|preview`, `/v1/particle_render/particles|preview`)
  in the cube and point viewers, and the start of a still or movie
  (`/v1/tasks/volume_render`, `/v1/tasks/particle_render`). A route's own 404
  (an unknown render session, a missing file) is not taken for an old
  backend. The point viewer now shows the ray-traced view only once its
  camera has arrived, as the cube viewer does, so a failed camera request no
  longer puts back the camera of an earlier time the view was on; in both
  viewers only the latest camera request counts. One shared, tested check (`src/app/MissingRoute.h`), which the file
  preview's fallback now uses too.

- **A preview for every file in the file browser (#159, item 3), backend.**
  `POST /v1/files/preview` describes any selected file from its first bytes:
  the HDU summary and header of a FITS file (also `.fits.gz`, read off the
  stream, and `.fz`), the first lines of a text file with the separator and
  column count of a delimited table (sniffed from the content, UTF-8 or
  Latin-1), the `.head` of a VisIVO Binary Table (for a `.bin`, the one beside
  it), the group/dataset tree of an HDF5 file (at most 200 objects, no data
  read), the start of a gzip/bzip2/xz/zip that is not FITS, a hex dump of
  anything else, and the entries of a directory. The file is read at most
  `max_bytes` (16 KiB by default, 64 KiB at most), so it works the same through
  a remote backend.
- **Desktop findings from testing (#159, client).**
  - *SKAVA as an archive.* SKAVA Discovery was a Data Hub tab made at start-up,
    and once closed there was no way back to it. It is now an entry of the Data
    Hub's *Archives* section, beside HiPS and VLKB, and of *Data* and the
    command palette; it opens as a tab on demand, and a closed tab reopens
    with its filters and results.
  - *Open starts on All supported files.* The main *Open…* remembered the type
    last chosen, so a type picked once hid every other kind of file the next
    time. Every entry point (File menu and ⌘O, Data Hub, command palette)
    now goes through one call that starts on *All supported files*; the
    single-purpose opens still remember their last type.
  - *A preview for every file.* The file browser said "Header preview is
    available only for FITS files". It now asks the backend's
    `POST /v1/files/preview` for the start of any file (FITS HDUs and cards,
    the first lines of a text table, a VBT's `.head`, an HDF5 file's tree, a
    folder's contents, type and size otherwise), monospace, with the format
    as a caption and a note when only the start is shown. A reply for a file
    no longer selected is dropped; on an older backend without the route a
    FITS file still shows its header.
  - *Feedback during opens.* Reopening a large CSV from *Recent Datasets* showed
    nothing for a while: the catalogue window reads the table and builds its
    scene before it appears. Every open (Open and Recent, for cubes, images,
    catalogues, VBT tables, velocity fields and HDF5 files, local or remote)
    now shows a busy indicator in the main window's status bar, a moving bar
    with *Opening name…* and a busy cursor, and says *Opened* or *Could not
    open* before any error box. Publication figure, image quality, pixel math,
    mosaic and the vector field's backend divergence show it too, in the
    window that asked. One helper (`BusyStatus`) does it for all of them.
  - *Ray-traced light is not a shape.* In the 3-D catalogue viewer it was the
    last item of *Shape*. It is now a separate **Ray-traced light** check under
    the shape, as in the cube viewer; the shape is kept for when it is cleared,
    and the controls that do not apply to the ray-traced view are greyed out
    rather than hidden. A table not open on the backend clears the check again.
  - *No box behind the colour bar.* The 3-D colour bar (catalogue and cube 3-D
    view, ray-traced overlay) was drawn on a dark box; it is now drawn straight
    on the view like the image and cube viewers' colour bars, with a dark halo
    under its labels, ticks and outline so that it also reads on a white
    background (saved stills included).
  - *Selection marker and deselection.* Selecting a point could show a sphere
    covering much of the view. The marker now starts from the data (the glyph
    it wraps, or a fraction of the mean particle spacing) and is kept to a few
    pixels on screen, re-sized as the camera moves; Esc and *View ▸ Clear
    Selection* deselect, besides a click on empty space.

- **Three small desktop defects (#150).**
  - *Preview statistics.* On a cube opened as a preview (above the
    full-resolution threshold) the Inspector's *3D cube* rows showed Mean and
    RMS as `0`: the preview path passed placeholders for the two numbers the
    strided preview does not measure, and wrote them over the backend's
    full-resolution statistics whenever the preview landed after them (Std,
    Median and Sum, which only the backend fills, were right). The rows now
    show the backend's whole-cube scan once it has answered, and a preview
    refresh or a sub-cube load no longer overwrites it; until it answers,
    Mean and RMS of a preview read `-`, with a tooltip saying they are being
    computed (or that the backend could not compute them), and a camera-ROI
    load no longer puts its own numbers in rows that stand for the whole cube.
  - *Deleted Recent Datasets file.* Reopening an entry whose file had been
    deleted sent the path to the open route, whose supported-type check
    answered "Unsupported file type…". The file is now looked for first (on
    this machine with the local backend, through the backend's classifier
    with a remote one); when it is gone the message says it is not found,
    offers to remove that one entry (*Keep* is the default), and the attempt
    is listed in *Last 5 jobs* as a failed open. A file behind a folder that
    cannot be opened is reported as unreadable, not deleted. SODA cutouts and
    node products are now recorded as living on their SKAVA node (sources
    `soda`, `node`) and are not looked for on the window's backend.
  - *Second launch with a path.* A second launch started its own instance
    when its only paths did not exist (they were not counted as files to hand
    over), and whenever it ran with another `$TMPDIR` than the running
    instance (the lock and the socket were looked for in the temporary
    folder, which a terminal, a script or a tool may set differently): the
    lock and the socket now live in the per-user folder macOS assigns
    (on Linux `/run/user/<uid>`, else a private `/tmp/visivo-va-<uid>`,
    whether or not `$XDG_RUNTIME_DIR` is set), and missing paths are handed
    over too,
    reported and listed in *Last 5 jobs* by the running instance. A window on
    a remote backend or a cluster used to take no part in the hand-over; there
    is now one seat per backend, so a second launch with the same `--backend`
    URL or `--connect-cluster` profile hands its paths to that window, while a
    launch on another backend (or on the local one) still starts its own
    instance rather than being merged into one on a different backend.

- **Tab- and semicolon-separated catalogues (#155).** The classifier took a
  text table separated by commas, tabs or semicolons as a catalogue, but the
  3-D catalogue viewer, the catalogue overlays and the SAMP table export then
  read it with a comma-only reader, so a `.tsv` opened as one column or failed
  with no useful message. One reader (`backend/app/text_table.py`) now serves
  all of them and the classifier: the separator is judged on the same first
  8 KB the classifier looks at, so what is called a catalogue is read with the
  separator that made it one. Quoted fields may contain any separator; blank
  lines and `#` comment lines are skipped, also between rows, except that a
  header written as a comment (`#id,ra,dec` above the first row) is the header
  and a `#` line with the table's field count is a row (a source named `#S5`);
  a semicolon table with decimal commas in two columns or more reads as
  numbers; a comma file with uneven rows opens as it always did; and a text
  file that is not a table is refused with a message saying so. The
  classifier no longer strips the leading whitespace of the text it judges, so
  a table whose header starts with a tab (a pandas index column) is a
  catalogue for both. `.tsv` is listed again in the file browser,
  under *Catalogues* and under *Delimited text tables* (formerly
  *Comma-separated tables*), where the HiPS catalogue overlay starts.
  *Load Catalogue Overlay* in the image and cube viewers, which reads the file
  in the client, sent `.csv` to its table reader and every other name to the
  DS9 region reader, so a `.tsv` found "no supported entries". It now decides
  by content with the same rule (`src/gui/DelimitedText.h`): a `.reg` file is
  regions; a file whose first lines make a comma, tab or semicolon table is
  that table, whatever its name, with the usual RA/DEC columns (unless every
  one of those lines is a region statement such as `fk5;point(…)`); a file
  that is no table but looks like DS9 is regions; and a `.csv` or `.tsv` that
  is neither is refused with a message saying why instead of being tried as
  regions. Fields follow the csv rules the backend reads with (a `"` opens a
  quoted field only at the start of a field, so `3"` is three arcseconds),
  lines may end in a lone `\r`, and comment lines (and a `#` header or `#`
  row), decimal commas (column by column) and uneven comma rows are handled
  as in the backend. Both file pickers now list `.tsv` and
  `.txt` beside `.csv`.
- **A file-type drop-down in the file browser (#153).** The backend's file
  browser, used by *Open…* with the local backend as well as a remote one, had
  a single *FITS only* checkbox from when the application opened only FITS;
  callers that wanted another kind relabelled it, and their six lists of
  extensions had drifted apart (the Stokes companions missed `.fz`). It is now
  a **File type** drop-down beside *Cancel* and *Open*, as in the native
  dialogs: *All supported files*, *FITS images and cubes* (`.fits .fit .fts`,
  their `.gz`, `.fz`), *Catalogues* (`.csv .txt .vot .votable .xml .tbl
  .speck`, FITS tables), *VisIVO Binary Tables* (`.bin.head .head .bin`),
  *HDF5* (`.h5 .hdf5 .hdf .bfdata`) and *All files*. The lists come from one
  table in the client (`src/app/SupportedFormats.h`), following what the
  backend classifies and opens. Folders are always listed; hidden files and
  folders such as `.DS_Store` only under *All files*. Each entry that opens
  one kind of file starts on that kind (velocity field, Stokes companions,
  image layer, catalogue, galaxy catalogue overlay, VBT, AREPO snapshot, HiPS
  catalogue overlay) and remembers the type chosen last. The title says what
  the browser opens and on which backend (*Open Dataset — Backend: Local*, or
  the cluster's name), instead of "Browse Remote FITS Files".

## 1.0.12 (2026-11-09)

- **Small desktop fixes from the GUI tests (#133).**
  - *Live spectrum on a preview.* On a cube above the full-resolution
    threshold the live spectrum under the cursor stayed empty and *Pin
    Spectrum* greyed out: the curve was read out of the decimated preview at
    the slice's full-resolution pixel, outside the preview's extent for most
    of the image, and from the wrong line of sight in its lower-left corner.
    The line of sight is now asked of the backend (`/v1/spectral/probe`),
    asynchronously, one request at a time with only the latest pixel queued,
    and a reply for a pixel the cursor has left is dropped. A pick in the 3-D
    view of a preview is converted to the cube's voxels (preview voxel i is
    the cube's i × stride), and its marker stays on that voxel when the full
    resolution replaces the preview.
  - *Galactic latitude axis.* The 2-D views read every tick of both axes on
    the image diagonal. On HCG 44 shown in Galactic coordinates the latitude
    axis therefore printed the latitude of points far to the right of it
    (54.59 at the top to 55.54 at the bottom, where astropy gives 56.02 and
    55.58 along the left edge), against a hover readout that is right at the
    reference pixel and, measured against astropy over the field, off by up
    to 0.07° in b and 0.09° in l towards the corners (the viewer's WCS is
    linear per axis and ignores the NCP projection; not changed here). Each
    axis is now read along its own edge, in the cube's slice and moment
    panes, the image viewer and the pane views; the same linear WCS applies
    to the labels.
  - *Inspector statistics.* The 3-D cube's Min, Max, Mean and RMS lost their
    left side ("00124432 JY/BEAM" for "-0.00124432 JY/BEAM"): the read-only
    fields kept the width of 17 characters and scrolled a longer value to its
    end. They are now sized to their text.
  - *⌘W* (Ctrl+W, Ctrl+F4) closes the cube window and every other viewer
    window, as its close button does.
  - *Quit during a long task.* The moment, moment FITS export and moment-map
    power spectrum / skeleton progress sheets were window-modal, which on
    macOS greys out the application menu: Quit did nothing until the task
    ended. Like the decompression window (#57) they are now non-modal tool
    windows, and quitting cancels the task on the backend. Every quit path
    (the Quit menu and Cmd+Q, the Dock) cancels every followed task and waits
    for its worker while the application still exists (as long as the
    cancellation takes, three seconds at most for one sent by the quit; no
    new task is started meanwhile):
    a worker left for the application's destructor sent its DELETE with no
    event loop to run it, and the exit hung. The Dock's Quit now asks "Do you
    want to exit?" as the Quit menu does: it used to close every window
    first, and a cube computing a moment, which defers its close, cancelled
    the whole quit after its moment had been stopped. The moment's progress window
    belongs to the cube window, so closing the Moment dialog no longer takes
    its Cancel away.
  - *Last 5 jobs.* A failed open is answered by a message box, and the Data
    Hub does not poll while a dialog is up: closing the box left the failure
    out of the list until the next poll, up to four seconds later. The list is
    now refreshed as soon as an open returns. Failed opens read "1ms" because
    the backend times only its `datasets/open` request, which refuses a bad
    file at once. The client now records its own opens (in memory) and the
    panel shows an open's end-to-end time, from the request to the viewer,
    the error or the cancellation, matched to the backend's job by time, with
    the backend's part in the tooltip. Opens refused before reaching the
    backend (file not found, unsupported, backend unreachable) are listed as
    failed entries too, as is a path given on the command line at launch
    that is not a file on this machine.
- **Imported regions are moved between sky frames.** A region was handed to
  the image's WCS in whatever frame the file used, so a galactic circle at
  (l, b) = (0, 0) on an FK5 map centred on the Galactic centre came back at
  [NaN, NaN] instead of the centre pixel [32, 32], and an explicit FK5 to
  galactic conversion left both frame and coordinates as they were. Positions
  now go through astropy from the region's frame to the image's (and to any
  requested sky frame, with no dataset needed for sky to sky), and ellipse
  and box angles turn with the frame: by 58.6° between FK5 and galactic at
  the Galactic centre, and with the pixel grid's rotation, mirror,
  unequal scales and skew. `fk4` (B1950) and `ecliptic` are new frames; CRTF
  `B1950`, read as J2000 before, is FK4, and `global coord=` is honoured. A
  region off the WCS projection is a 422 instead of NaN.
- **Each region size keeps its own unit.** `ellipse(10,20,1',30",0)` had
  semi-axes 1 and 30 arcmin (the second size took the first's unit, a factor
  of 60); it is now 1 and 0.5 arcmin, and the same holds for boxes, annuli
  and CRTF. Pixel sizes in a sky frame (`6i`) and angular sizes in the image
  frame (`10"`) are read and converted; a region mixing the two is refused.
- **ds9 files written in degrees reimport.** The exporter wrote `0.01d` and
  the parser rejected it ("Cannot parse size token"); every ds9 suffix (`d`,
  `r`, `p`, `i`, `'`, `"`), `fk5;circle(...)` on one line and exponents such
  as `1e-05` are now read. Export then import is exact to 10⁻⁹ for all six
  shapes in every frame and unit, in both formats; positions are written
  with 12 significant digits (6 before: an RA above 100° was rounded to 0.001°, up to 1.8").

- **PSRFITS and HDF5 dynamic spectra open again.** Both converters wrote a
  `→` into the sidecar's `ORIGIN` card, which astropy refuses (FITS values
  are ASCII), so every conversion failed before writing the file. The cards
  are ASCII now, and a test scans the backend for non-ASCII literals written
  to any FITS header.
- **PSRFITS samples are converted to physical values.** The converter used
  `DATA` as stored; it now applies `DATA × DAT_SCL + DAT_OFFS` per row,
  channel and polarisation (`(DATA − ZERO_OFF) × DAT_SCL + DAT_OFFS` for
  search data), so a sample of 10 with scale 2 and offset 100 reads 120, not
  10. Channels with a zero `DAT_WTS` weight become NaN, which the averages,
  the pulse profile and the RFI mask already treat as flagged; Stokes I is
  `AA + BB` for `AABB`/`AABBCRCI` data instead of `AA` alone.
- **PSRFITS layout comes from the headers.** Fold or search is read from
  `OBS_MODE` and the row shape from `NBIN`, `NCHAN`, `NPOL`, `NSBLK` and
  `NBITS`, instead of guessed from which axis has length 1, 2 or 4. A search
  file of 2 sub-integrations × 16 samples × 1 polarisation × 3 channels was
  refused (`Unrecognised PSRFITS DATA shape: (2, 16, 1, 3)`) and now gives 32
  time samples of `TBIN`; a search row of 2 samples × 1 pol × 2 channels was
  read as a folded row and gave one wrong pixel. 1-, 2- and 4-bit packed
  search samples are unpacked and `SIGNINT = 1` samples read as signed
  (byte 255 is −1); other widths, or an `NBITS` that disagrees with the
  stored type, are refused by name. A folded file's time step is its
  `TSUBINT` (10 s in the test file), not `TBIN × NBIN` (0.002 s, one pulse
  period). Rows are placed by `OFFS_SUB`, so times count from the start of
  the observation and a dropped sub-integration is a blank column: rows
  centred at 5 s and 25 s used to become two adjacent columns from 0 s.
- **Folding and dedispersion respect a decreasing time axis.** Both tools
  took `|CDELT1|`, so the same observation stored backwards folded to
  `[0, 0, 1, 0]` instead of `[1, 0, 0, 0]` and was dedispersed in the wrong
  direction; sample times and dispersion shifts now keep the sign of
  `CDELT1`.
- **Pixel Math reprojects maps that are on different sky grids**, not only
  maps of different size. Two 64×64 maps of the same source, one with its grid
  shifted by 4 pixels, used to be subtracted pixel by pixel (residual 0.86 of
  the peak); B is now reprojected onto A's grid (`reproject_interp`,
  bilinear, as `/v1/image/reproject` does at order 1) and the residual is 0.
  Maps whose WCS agree within 0.01 pixel are combined exactly as before. The
  response says so in `info.reprojected` and the output carries a `HISTORY`
  card. A map with a sky WCS and one without are refused instead of combined
  blindly, and so is a map whose sky WCS cannot be read. SIP cubes keep their
  sky WCS when loaded (astropy refuses SIP on a 3-D header, and they used to
  look WCS-less), and lookup-table distortions are read and carried into the
  output.
- **A spectral index from two negative fluxes is now blank.** Pixel Math's
  `spectral_index` took the log of the ratio, so −2 and −1 at 2 and 1 GHz gave
  α = 1; α is now NaN unless both fluxes are finite and positive, and equal
  frequencies are refused.
- **Regridding keeps SIP astrometry.** The ×f block average now rescales the
  SIP coefficients (`A`/`B`/`AP`/`BP` by f^(p+q−1)): on a cubic SIP, block
  centres were 0.44 pixel off at ×2 and 0.61 at ×4, now under 10⁻³.
- **Image Quality flags perfectly uniform stripes.** An image of pure
  horizontal bands had `striping_metric` 0 and no `striping` flag, because one
  of the two variances was zero; it is now the metric's ceiling, 10⁶ (all
  ratios are capped there), and flagged. A flat image stays 0.
- **Cached products follow the file on disk.** Moment, PV, isosurface and
  line-width products (and Export Moment as FITS) were cached by path and
  parameters only, so a cube replaced or rewritten under the same name came
  back with the old values and `cache_hit: true`. The cache key now carries
  the file's version (device, inode, size, modification and change times, and
  a VBT's `.head`); the cube statistics, catalogue overlay and ray-traced
  preview caches use the same version.
- **Weighted spectral stacking ignores blank channels correctly.** The
  weights were normalised once over all cubes, so a channel blank in one cube
  was pulled towards zero (equal weights, values [1, NaN] gave 0.5) and a
  channel blank in every cube came out as 0. Each channel is now divided by
  the weights of the cubes that have a value there, and a channel without any
  is NaN. Explicit weights must be one per cube, finite, non-negative and not
  all zero (a list of the wrong length used to fall back to automatic
  weights silently).
- **3-D masks of cubes stored in a FITS extension.** Box masking and the
  Kinematic Lasso mask writers edited the primary HDU of the copy, so a file
  with an empty primary and the cube in `DATA_EXT` failed with "3D masking
  requires a 3D cube dataset". They now mask the HDU the dataset reads.
- **SAMP: VisIVO receives, and links rows with TOPCAT** (#130, steps 1, 2 and
  6). VisIVO is now a callable SAMP client: a loopback XML-RPC endpoint, run
  with the hub traffic on a thread of its own, takes tables, images, sky
  positions, row highlights and row selections, and answers calls once the
  data opened or failed. TOPCAT lists VisIVO as a target. A received file is
  downloaded by the client and staged with `/v1/samp/upload-file`, so remote
  backends work; `/v1/samp/import-url` now uses the guarded fetch. Rows are
  linked both ways by their place in the original table (new `row_index` in
  `/v1/catalogue/query`, new `table_id` in `/v1/samp/send-catalogue`). The
  SAMP menu shows whether VisIVO is connected and receiving, and a failed
  callback or subscription is reported in the status bar and Diagnostics.
  Sending a file is now a SAMP call: VisIVO reports what each tool answered
  instead of assuming success. Staging runs off the backend's event loop
  (`deliver`, `rows`, `size_bytes` on `/v1/samp/send-*`), and a converted
  table is a BINARY2 VOTable. A single click now selects a source in the 3-D
  catalogue view (it needed a double-click), and its first page of rows knows
  the table's total, so *Load more* appears. A received table is shown under
  the sender's name (title, path bar, Session Data, Inspector) and, being a
  temporary copy, is not added to Recent Datasets. Before staging a file
  VisIVO asks the hub whether any tool takes it (`getSubscribedClients`), and
  reports answers under the name actually sent. A click highlights the row in
  the table VisIVO received and in every copy it sent back. Unnamed sources
  and the table's row header use the original table's row number, also after
  *Load more*, and the table scrolls to the selected row. Connect with no hub
  running, or a hub going away, leaves VisIVO waiting and retrying every few
  seconds until one runs (Disconnect stops it). Received images get the same
  naming and Recent Datasets treatment as tables. Repeated peer lists are no
  longer logged twice. SAMP connect no longer logs two 401 warnings (a stale
  settings token was used instead of the local backend's token file).
- **Compressed FITS open directly** (#57). Gzipped (`.fits.gz`) and
  tile-compressed (fpack, `.fz`) images and cubes open like their uncompressed
  versions: the first open decompresses the file once into the workspace cache
  (`VISIVO_DECOMPRESS_CACHE_DIR`, capped by `VISIVO_DECOMPRESS_CACHE_MAX_BYTES`),
  and every viewer, moment, spectrum, SODA cutout and velocity field reads that
  copy. A large file is decompressed as a task with progress and cancellation
  (`POST /v1/tasks/decompress`); a truncated or damaged file is refused with
  its reason. Recent files and provenance keep the original path.
- **Region flux on multi-beam cubes.** Region statistics now divide by the
  selected channel's beam from a CASA `BEAMS` table instead of the
  primary-header beam (a 4" channel beam under a 2" header beam returned a
  flux 4x too large), and give a flux when only the table defines the beam.
- **Reprojection keeps unit and beam.** A reprojected image now carries the
  source's `BUNIT`, `BTYPE` and beam (`BMAJ`/`BMIN`/`BPA`), so a Jy/beam map
  stays measurable; the target WCS is never overridden. A 4-D cube is
  reprojected as one plane, as documented.
- **Skeleton and void smoothing respects blanks.** Pre-smoothing is normalised
  over the finite pixels and NaN pixels stay invalid; before, an unobserved
  area was filled with 0 and `void` mode traced false voids inside it.
- **Power spectrum and skeleton read one plane.** Only the selected plane is
  cast to float64; peak memory for a 128x128 plane went from 17 MiB on a
  128-channel cube to 1 MiB, the same as on a 2-channel one.
- **Photometry and polarisation no longer return wrong numbers silently**
  (#137). Aperture photometry and the 2-D Gaussian fit find a 4-D cube's
  spectral and Stokes axes from the header and measure Stokes I of the
  requested channel (a (spectral, Stokes, y, x) cube used to give a Stokes
  plane of the first frequency; a multi-plane Stokes axis without I is now
  refused, and a single-plane Q, U or V file is measured as labelled), and
  report `resolved_channel` and `stokes`.
  Their ICRS RA/Dec are converted to the image's frame, so Galactic and FK4
  images work. Polarised intensity/angle and RM synthesis compare the Q and U
  pixel → sky mapping, refusing a pair rotated by `PC`/`CD` and accepting the
  same grid written either way; PI/PA refuses a channel that U does not have
  or that sits at a different frequency in U.
- **Faster out-of-core moment maps** (#116, #117, #119). Large cubes are
  read with explicit plane reads and read-ahead, and the number of worker
  threads is chosen from the CPUs and the memory budget instead of a fixed
  four. On a PLEIADI-GPU node M0, M1 and M2 on the dask path are 1.6 to 2.6
  times faster, with the same maps. Export Moment as FITS now uses the same
  out-of-core path as the moment itself. Measurements are in
  `benchmark/RESULTS.md`.

## 1.0.11 (2026-11-06)

- **Linux: the release opens again** (PR #131). The Linux AppImage crashed
  at start, inside QtWebEngine, when run from its FUSE mount (Ubuntu 22.04
  and 24.04, releases up to 1.0.10). The Linux package now holds the
  application in `./app` with a launcher: extract the tarball and run
  `./VisIVOVisualAnalytics`. It no longer needs `libfuse2`. The VLKB
  panoramic view is loaded when its tab is first shown, so a web-engine
  failure can no longer keep the application from opening, and each release
  now starts the packaged client before it is published.
- **SODA `BAND` on MeerKAT sub-band cubes** (#115). Obit MFImage cubes with a
  `SPECLNMF` spectral axis (the MeerKAT Milky Way Bulge and MGCLS releases)
  now accept `BAND`: it returns the sub-bands whose centre frequency
  (`FREQnnnn`) falls in the interval, and 204 when the interval holds none.
  The cutout's header lists the frequencies of the planes it holds, as a
  standard `FREQ` axis when they are evenly spaced, and the DataLink
  descriptor advertises the cube's `BAND` range.
- **One VLKB service URL for queries and batch cutouts** (#76). With
  `VISIVO_VLKB_URL` unset, the TAP query composer and the multi-cutout batch
  now both use the public VLKB at `https://vlkb.ia2.inaf.it`; the batch used to
  answer 503 ("No VLKB service configured") and the query used
  `https://vlkb.ia2.inaf.it/vlkb`, where the service answers 404. A request's
  own `service_url` still wins, then the variable. The rule lives in
  `app/vlkb_service.py`.
- **Self-hosted copilot: no silent `gpt-4o`, and the environment URL is
  checked** (#75). With `VISIVO_LLM_BASE_URL` set and no
  `VISIVO_ASSISTANT_MODEL`, the backend asks the server for its models at the
  first question and uses the only one; with none or several it answers which
  names the server serves and which variable to set. `VISIVO_LLM_BASE_URL` now
  gets the client URL's checks (http(s), no credentials, no query or
  fragment); a value that fails them keeps the copilot off with the reason in
  its answers and in `/v1/assistant/status` (`problem`), and is never
  contacted, while the rest of the backend starts as usual.
- **The desktop says why the copilot is off** (#75 follow-up). When the
  backend reports a `problem` (a self-hosted server with no or several
  models, a refused base URL), the copilot panel shows it above the
  conversation and Settings ▸ Scientific Copilot shows it under the provider
  fields, in place of the generic "No LLM provider is configured" hint. It
  goes once the backend stops reporting it, for example after a model is set.
  A backend without the field shows what it showed before.
- **The desktop keeps the server's copilot configuration by default.**
  Settings ▸ Scientific Copilot has a new first choice, "Use the server's
  configuration", and it is the default: the desktop no longer replaces what
  the backend's operator configured through `VISIVO_ASSISTANT_PROVIDER`,
  `VISIVO_LLM_BASE_URL` and `VISIVO_ASSISTANT_MODEL`, and a refused
  `VISIVO_LLM_BASE_URL` now reaches the user. Claude, ChatGPT, Your server and
  Off still replace it for the user's session. A "None" saved by an earlier
  release reads as the server's configuration, or as Off when a key or URL
  was saved with it. `POST /v1/assistant/config` accepts `reset: true`, which
  returns the session to the environment's configuration. The panel's status
  line says which configuration is active, e.g. "Provider: server's
  configuration (self-hosted, model qwen) · ready".

- **Cosmology tools in the interface** (#29). Every cosmology route of the
  backend now has a desktop entry or a stated reason:
  - **Tools ▸ Cosmology Calculator** (main window and 3-D catalogue viewer):
    one or many redshifts under Planck18, Planck15, Planck13, WMAP9 or a custom
    ΛCDM (H₀, Ωm, ΩΛ; flat by default, and said in words when it is not).
    Comoving, luminosity and angular-diameter distances, lookback time, age and
    kpc per arcsecond, all from the backend; copy, or export as CSV. The
    custom-model route `GET /v1/cosmology/distances` has its client binding.
  - The 3-D catalogue viewer's **Model** selector gains **Custom**, the model
    set in the calculator; the status line names the model the sources were
    placed with.
  - **Angular Power Spectrum** and **Skeleton / Voids** on 2-D images
    (Tools ▸ Structure) and on a cube's moment map (Tools ▸ Maps): P(ℓ) on log
    axes with its fitted slope, the skeleton as an overlay or a mask map, each
    a product with its replay record. Until now only the Copilot reached them.
  See the user guide page *Cosmology tools*.
- **Truncated FITS files are refused by name** (#118). Opening a FITS file
  shorter than its headers declare now fails at once with "The file … is
  truncated: 697.9 GB on disk, 961.4 GB declared (5644 of 7776 planes
  present).", instead of "buffer is too small" at the first read. The check
  reads headers only, covers extensions and tile-compressed HDUs, and is the
  same on the local and remote backends: HTTP 422 from the open routes and the
  node cutout, a 422 `Error:` from SODA, `valid: false` from the filter and
  velocity routes. The desktop shows the backend's message for a refused open
  instead of an HTTP status phrase. A file cut only inside its final padding
  block still opens.

## 1.0.10 (2026-11-06)

- **Suggested SoFiA-2 parameters** (#64). The cube viewer's SoFiA-2 dialog
  now starts from parameters suggested from the cube itself, each with the
  reason it has its value. The backend reads the cube once on a sample (about
  10 s on a 44 GB cube) and measures:
  - the noise as SoFiA reads it;
  - the beam;
  - the rest-frame channel width;
  - the channel-to-channel noise correlation;
  - continuum residuals and the memory SoFiA needs.

  It asks only when the data type or the line widths are ambiguous. Values
  stay editable, and parameters can be added, each with SoFiA's default
  shown. The dialog used to send parameter names SoFiA-2 rejects and could
  not run; it now runs SoFiA as a backend task and opens the mask in MIP. The
  backend checks every value as SoFiA reads it, since SoFiA itself checks
  none: `bogus` for a statistic used to run silently with another one. A run
  with no sources left is an empty result, not a failure. On the held-out
  SKA SDC2 development cube, scored with the challenge's own scorer, the
  suggestion passed every criterion fixed before the data
  (`validation/sofia_suggest_spec.md`, `validation/out/sofia_suggest/RESULTS.md`).
- **Remote views wait for a rendering slot** (#4). When a backend already
  renders as many views as `VISIVO_RENDER_MAX_SESSIONS` allows, a new
  remote cube, point or catalogue view waits in a queue instead of being
  refused. The view shows its place in the queue, and "Give up place" leaves
  it. Places are served in order and fairly across users. A view that stops
  waiting, or whose window closes, gives its place back. While others wait,
  a view nobody is connected to frees its slot after a grace period.
- **POL in the SODA service, and DataLink links** (#7). SODA cutouts accept
  `POL` (Stokes I, Q, U, V and the rest, several at once). `/soda/links`
  answers a DataLink request with service descriptors whose BAND, TIME and
  POL ranges come from the dataset, so TOPCAT and pyvo offer the cutout form
  directly.
- **Long moments and PV diagrams follow their task** (#3). Moment maps,
  Export Moment as FITS (now a task too: `POST /v1/tasks/moment_fits`) and
  PV diagrams are followed until they end, with no time limit on the
  desktop. The client used to give up after 30 s and start the same
  computation again synchronously. Cancel stops the task on the backend. A
  PV whose path is edited while it runs is replaced by the newest path, and
  closing the cube window stops a PV under way.
- **Moment 0 on large cubes blanks empty lines of sight** (#3). Above
  512 MB a moment is computed out of core in blocks, and there a masked M0
  reported 0 where no voxel passed the threshold; it is now blank, as on
  smaller cubes. Measured on the SDC2 development cube: 707 of 1920 pixels of
  a 3-sigma M0 over 160 channels changed from 0 to blank.
- **Out-of-core moments stay within memory on SKA-size planes** (#3). The
  block-wise moment used for large ranges read a 6082² plane (148 MB) as one
  block. Its temporaries and partial sums then reached 14 GB on four
  threads, and M1 and M2 of a 450 GB cube failed under a 16 GB cap. Planes
  larger than `VISIVO_MOMENT_CHUNK_BYTES` are now read in bands of rows, and
  the centroid map of M2 and up is split the same way. The maps are
  bit-identical.
- **Moment and PV tasks have no time limit** (#3). The 900 s ceiling
  (`VISIVO_HEAVY_TIMEOUT_S`) now applies only to synchronous requests, and
  their 504 points to the task routes. Before, a moment task on an SKA-size
  cube failed at 900 s while its worker ran on. `VISIVO_TASK_TIMEOUT_S` sets a
  ceiling for tasks if wanted. Cancelling a moment task now stops the
  block-wise computation used above 512 MB within one block; before, it ran
  to its end.
- **Out-of-core moment benchmark for HPC nodes** (#3):
  `benchmark/moment_ooc_bench.py` and `benchmark/remote/run_hpc_moment_bench.sh`
  time M0, M1 and M2 under a memory cap on SLURM/Apptainer or any Linux host,
  and check the streamed and block-wise maps against the in-memory ones.

## 1.0.9 (2026-11-05)

- **Ray-traced particles** (#101). A point table can be drawn as a
  continuous medium: each particle is a Gaussian kernel the size of its
  smoothing length, and along each ray the kernels emit and, optionally,
  absorb, with the light kept in floating point and graded by a tone curve
  (Dolag et al. 2008). Two styles: *3-D light* in perspective, and a
  *Projected map* of the column density, coloured by density, a column or
  the local velocity dispersion. The view is drawn on the desktop's GPU and
  stays interactive on millions of particles; stills up to 8K, orbit movies
  and floating-point FITS of the light are rendered by the backend. The
  3-D volume view also gains gradient shading and ambient occlusion.
- **Ray-traced volumes** (#106). A cube can be drawn as light in the same
  way, with *Ray-traced light* in the cube viewer's *Display, 3D* section:
  faint diffuse emission and bright cores together, with no opacity curve to
  tune. The colour can come from a second quantity: another field of a VBT
  volume, or, for a spectral cube, each channel's velocity, so that a
  projected map seen along the spectrum is the intensity-weighted mean
  velocity. Values below the threshold emit nothing, so a radio cube's noise
  does not veil the source. On a remote backend the grid is gzipped (a
  WALLABY grid through a tunnel: 34 MiB in 8 s became 0.4 MiB in 0.4 s), and
  a desktop without a GPU view gets frames drawn by the backend.
- **VBT volumes open as cubes** (#83). Each field of a VisIVO Binary Table
  volume is read in place and opens in the cube viewer, locally and on any
  backend, with every tool that applies to a cube whose third axis is
  spatial. The 3-D view gains a colour bar, a scale chosen in the LUT
  editor (Linear, Log, Sqrt, Square, Power), and the share of voxels above
  the threshold; the isosurface is bounded (a 512³ field in 1-2 s instead of
  200 s). Log colour tables were drawn linearly and are now log-spaced.
- **Point tables and sky catalogues drawn on the backend** (#83). VBT point
  tables open in the 3-D catalogue viewer, which gains Gaussian splats,
  named colour maps, the LUT editor and the colour bar. A table or
  catalogue larger than what the window loads can be drawn on a backend
  that renders server-side (*View ▸ Rendering*: Auto, Local, Remote). Sky
  catalogues are placed there exactly as the desktop places them (frame,
  distance convention, cosmology).
- **Command-line datasets on remote backends** (#108). A path given after
  `--backend` or `--connect-cluster` is the backend's and is opened there;
  it was dropped because it did not exist on the local machine. A path the
  backend does not find is reported as such, and missing local paths are
  reported in a warning.
- **Faster exact region statistics** (#99). The exact median and MAD of a
  large region take three passes instead of five, with the same results: a
  1.1 billion-pixel box on PLEIADI went from 94 s to 50 s of computation.
  The local copy of the region is no longer written to a temporary directory
  held in memory (tmpfs) on a cluster.
- **The sign-in is kept on Linux** (#56). The Linux release is built with
  libsecret, so the VLKB sign-in survives a restart through the Secret
  Service; without one the client asks for the login, as before.
- **Backends started by the desktop exit with it**, instead of being left
  running after a crash. Values written into a SLURM job script are quoted
  and validated.

## 1.0.8 (2026-11-02)

- **2-D images larger than memory** (#85). An image above
  `VISIVO_IMAGE_PYRAMID_BYTES` (256 MiB as float32) gets an averaged tile
  pyramid, built out of core in the background by the backend, local or on a
  cluster. Until it is ready the tiles are sampled from the file as before;
  then the view reloads, averaged, without moving the camera. The data-state
  panel shows the build's progress and says what a displayed pixel is ("each
  pixel the mean of N×N", or "sampled: 1 pixel in N×N"), and so does the
  cursor readout. On a 16 GiB image the build takes under a minute with 8
  processes, and a tile comes in about 27 ms instead of 100.
- **Region statistics from the full-resolution data** (#95). Below full
  resolution, or when a region reaches beyond the pixels loaded, the
  statistics of a box, circle, ellipse, annulus or polygon are computed by
  the backend on the file, out of core and exactly (median and MAD
  included), instead of on the displayed block means or samples. The dialog
  says what they were computed on, and the live readout says when they are
  being computed. A region of 1.1 billion pixels takes about 25 s on a
  laptop and 100 s on PLEIADI (BeeGFS), in about 1 GB of memory; its values
  are copied once to local disk (`VISIVO_REGION_SPILL_DIR`) so that the file
  is read only once.
- **Readable region readout.** The live Σ / μ box over the image takes its
  background from the theme: in the light theme it was dark text on black.
  Pixel counts in the statistics are grouped by thousands.
- **Cluster jobs that fail at start-up are reported.** The SLURM launchers
  stop at once with the job's state and the tail of its log, instead of
  waiting five minutes and saying that no node was assigned.

## 1.0.7 (2026-10-29)

- **The local token stays local.** The token in `VISIVO_TOKEN` or
  `~/.visivo_token` belongs to the local backend, the one set in Settings. It
  was also sent to any other backend on this machine (another port), and
  used as the retry after a `401` from any backend, remote ones included.
  Now only the configured local backend receives it; every other backend
  keeps the token it was registered with.
- **Large cubes on a machine with little memory.** The size above which a
  cube opens as a preview, with full resolution on request, is 4 GiB or a
  quarter of the backend's memory (its container's limit, or the machine's)
  when that is less, so an 8 GB machine no longer runs out of memory loading
  a 3 GB cube at full resolution. `VISIVO_FULL_RES_THRESHOLD_BYTES` still
  sets it outright.
- **Moment maps follow the free memory.** A channel range is integrated in
  memory only when it is below `VISIVO_MOMENT_STREAM_BYTES` (2 GiB) and
  twelve times its size is free; otherwise it is streamed from disk, in slabs
  of at most `VISIVO_MOMENT_CHUNK_BYTES` (64 MiB) rather than a fixed number
  of channels, so wide planes no longer exhaust a small machine.
- **Credits.** The About dialog and the backend's API page name VisIVOLab at
  INAF - Osservatorio Astrofisico di Catania, with links to the source code
  and the documentation.

## 1.0.6 (2026-10-27)

- **VLKB cutouts for a source table** (*Data ▸ VLKB Cutouts for a Source
  Table…*), the legacy client's *Load User Table* brought back: a table of
  sources (id, l, b) is searched source by source, the Images and Cubes tabs
  preview which surveys cover each source (green, yellow, red), and the ticked
  sources and products go as one asynchronous archive job. Beyond the legacy:
  comma-, semicolon- and space-separated tables and `#` comments, drag and
  drop, All/None ticks, a coverage legend, a progress bar with Stop, and a
  live count of the cutouts that will be requested.
- **VLKB sign-in works again.** `AuthKeys.h` was committed with empty
  endpoints, so *Sign in* did nothing. The IA2 endpoints are now CMake
  defaults and the client credentials come from `VISIVO_VLKB_CLIENT_ID` /
  `VISIVO_VLKB_CLIENT_SECRET` (release: repository secrets); a build without
  them says sign-in is not available. The login page opens above the Settings
  dialog instead of behind it.
- **The VLKB sign-in survives a restart.** The refresh token is kept in the
  system's secure store (macOS Keychain, Windows Credential Manager, libsecret
  on Linux when installed), never in the settings file; at start the session
  is renewed silently. A token the SSO refuses is forgotten; an SSO that cannot
  be reached is retried at the next start; *Sign out* removes it.
- **The VLKB batch window.** *Download* keeps the cutouts without opening them
  (*Show folder* reveals them); *Open…* opens them with the images of each
  source as layers of one viewer and each cube in its own window, asking first
  above twelve files. *Abort job* stops the job on the archive, and closing the
  window while it runs asks whether to stop it or leave it running. The status
  shows the elapsed time (the archive reports a phase, not a progress). The
  results no longer include the archive's `.tar.gz` and its `response_*.json`,
  which were offered to the viewers and failed with "Unsupported file type".

## 1.0.5 (2026-10-26)

- **New-version notice.** Once a day at startup the client asks GitHub for the
  latest release and, when a newer one is out, offers to open its download
  page (*Download…*, *Skip This Version*, *Later*). *Help ▸ Check for
  Updates…* asks on demand and always answers. The startup check can be
  switched off in *Settings ▸ UI*; the request tells GitHub only the user's IP
  address and the version they run.
- **Velocity fields reopen from Recent Datasets.** They are now recorded in the
  list and reopen in the vector-field viewer. An older entry that had recorded
  a velocity field as an image no longer opens a blank image viewer: cube and
  image entries are re-identified when reopened.
- **Moment maps of large cubes are out-of-core in fact, not only in name.**
  The dask path handed the memory-mapped cube to `dask.array.from_array`,
  which copies an ndarray before chunking it: the whole selected channel range
  was read into memory. A service capped at 8 GB was killed computing a masked
  M0 of an 8 GB cube. The cube is now read block by block from the file, blocks
  are sized in bytes (`VISIVO_MOMENT_CHUNK_BYTES`, 64 MB by default) and the
  moments that need the centroid first (M1 to M5) are computed in two passes
  instead of keeping the first pass's blocks. Results are unchanged. A file
  replaced on disk during the computation is reported instead of read half old,
  half new.
- **Basins and full-resolution regions do not freeze the window.** In the
  velocity-field viewer both are computed in the background under the same
  progress sheet as the other analysis tools, instead of a blocking wait
  cursor.

## 1.0.4 (2026-10-26)

- **Left dock no longer clipped.** The cube viewer's sidebar sized itself to
  the widest page of its *Display* section, hidden ones included, so the 2-D
  page (266 px) pushed the 3-D controls past the 272 px dock and Volume,
  MinIP and the threshold field were cut on the right. Only the page shown
  counts now (content 234 px).
- **Channel strip.** The slider takes the width of the strip; the spectral
  readout takes only what its value needs (compacting to GHz/MHz in a narrow
  pane and widening back), instead of half the strip as blank space. The
  symbol follows the axis: ν for a frequency, λ for a wavelength, v for a
  velocity.
- **2-D layer list does not scroll sideways.** Layer cards are as wide as the
  list and follow it when the dock is resized or a scrollbar appears.
- **Docs publish through the `VisIVO-doc` mirror again.** The sync workflow
  and `scripts/sync-docs.sh` are back, now keyed to this repository: a push
  that touches `docs/` copies the tree into `VisIVOLab/VisIVO-doc`, which is
  what the Read the Docs project builds (`VISIVO_DOC_DEPLOY_KEY` secret
  required here).

## 1.0.3 (2026-10-22)

- **Recipe replay steps are typed.** *Export recipe as JSON…* now writes each
  replay call with its `request` as the service takes it, under the API's
  field names with numbers as numbers and seeds as `[z, y, x]` (`seed_z`,
  `reach`, `lambda_v`, `reach_max`, …; the apply's `mode`; the fit's `sigma`
  and `min_ring_pix`), plus a `body` line naming what travels as bytes — the
  float32 mask the grow returns, posted to the apply with the origin and
  dimensions of that response — so a script posts the record back instead of
  parsing the Provenance labels. The request recorded is the one the mask on
  screen was grown with, kept beside the mask, not the sliders' values at the
  time of the apply (a keyboard nudge of λv invalidates the field without
  regrowing it). The schema name stays `visivo-recipe/1`; the display
  `parameters` are unchanged. A C++ test reads the fields of the service's
  request model out of the router and holds the recipe to them.
- **The lasso says when its domain was cut.** A source larger than the grow's
  graph budget (four million nodes) is cut to a seed-centred box, and the
  selection is exact within that box only. The service now reports it
  (`X-Visivo-Trim-Box`, the largest box half-width over the seeds), the
  completion message says so, and the product's Provenance (*Domain trimmed*)
  and the cube's FITS `HISTORY` (`domain_trimmed=`) record it; nothing is
  added when no grow was cut.
- **Validation harness.** `usecase_ngc3137.py` reports how the default
  selection splits between the 2.5σ component and the sub-threshold shell;
  the interface figure adds enlarged crops (`figure_interface.py` lays panels
  out in rows); Figure 1's storyboard drops the API strip, keeps its panel
  notes on two lines and draws the navigation step (a)→(b) dashed, apart from
  the transitions that leave a product; the client's complete recipe export
  quoted in the paper ships as `validation/out/recipe_ngc3137_lasso.json`;
  the RM-synthesis test that pins the 25 → 63 rad m⁻² number states its
  conventions (per-plane frequencies, λ² = (c/ν)², sign, grid maximum).
- **Docs build from this repository.** Read the Docs builds from the
  `docs/` tree here, so the `VisIVO-doc` mirror and its sync workflow are gone.

## 1.0.2 (2026-10-21)

- **Public repository: `VisIVOLab/VisIVOVisualAnalytics`.** Documentation,
  issue links, the paper and the build/HPC examples point there; the SLURM
  tunnel scripts take `--repo-dir` (or `VISIVO_REMOTE_REPO`) for a clone with
  another name on the frontend.
- **Full-resolution load: a bounded transient, and no JSON fallback.** Loading
  a cube at full resolution held it three times over while it arrived (the
  four Z-slabs, a stitched copy, the image) before the GPU copy; the slabs are
  now copied straight into the image and released one by one. Worse, a slab
  that failed for any reason — a timeout, a connection the backend dropped
  under load — made the client re-request the whole volume as base64 JSON and
  hold it as the reply, its copy, the parsed document and the decoded bytes,
  while the backend kept serving the aborted slabs (the "5 running" of a
  45 GB client on a laptop). That fallback now exists only for a backend
  without the binary route (404); a failed slab fails the load and says so.
  Before fetching, the client compares three times the region with the
  machine's physical memory and refuses, with the numbers, when it does not
  fit.
- **Windows build fixed.** Three lambdas in the kinematic-model dialog used
  the C++20 capture `[=, this]`, which MSVC rejects in C++17 (clang only
  warns); the release workflow's Windows job stopped there. The sources were
  checked with clang's C++20-extension warnings turned into errors.
- **Multi-extension FITS cubes open.** An ESO phase-3 or APEX product keeps
  the primary HDU empty (`NAXIS = 0`) and the cube in an image extension
  (`DATA_EXT`, with the variance in `STAT_EXT`). The classifier called it
  "unknown" and the opener read an image of shape `()`. Every reader now takes
  the first image HDU with a real array (`image_hdu_index`): the opener, the
  classifier, the cube reader, server-side rendering, moment and PV headers,
  region WCS, the velocity-field and dynamic-spectrum loaders, and the header
  viewer, which lists the extension's header followed by the descriptive cards
  only the primary carries; header edits go to the HDU that owns the card.
- **Validation harness: the methods on discs of known truth.**
  `validation/lasso_synthetic.py` runs the lasso on discs built with the
  application's tilted-ring model (S/N, extent, a neighbour along the minor
  axis); `validation/kinematic_fit_synthetic.py` recovers a flat and a warped
  disc with the comparator's fit; `validation/lasso_sensitivity.py --grid`
  maps Dice over the (λv, reach) plane. Figure 4 of the paper gains the two
  residual maps; Figure 1's panels are drawn at the size they print at.
- **Exported recipe is a record, not a list.** *Export recipe as JSON…* now
  writes the product (id, kind, label, the service routes that replay it, in
  order: the grow from the parameters, then the apply — and a `replay_caveat`
  when the parameters alone do not suffice: a hand-edited mask, a fit that
  needs its selection), the
  dataset (name, path, backend ids), the parameters, the geometry it was taken
  from, the application version and the export time, under
  `"schema": "visivo-recipe/1"`. Before, it was the label→value map of the
  Provenance tab alone.

- **Large IPAC catalogues open in seconds.** Opening the 2.3-million-row
  `NKATZ_2.3M.tbl` used to take minutes and more than 18 GB, and the whole
  backend stopped answering meanwhile (the parse ran on its event loop, so
  `/v1/health` timed out). All-numeric IPAC tables are now read by pandas' C
  parser (0.6 s, 165 MB), giving the same frame as astropy, whose reader is
  still used for tables with string columns or declared null markers; the
  catalogue open runs in a worker thread.
- **Multi-file catalogue offer, narrowed.** Picking a `.tbl` used to offer every
  same-extension file in the folder as one catalogue, so a folder of unrelated
  tables ended in "Column mismatch". Only pieces whose names differ by a
  trailing partition index (`…_1_1_1.tbl`) are offered now, and the question
  lists them.
- **Region spectrum across a blank channel.** A channel that is entirely NaN
  in the cube (channel 91 of the WALLABY Hydra cube) showed as a deep negative
  spike in the box/circle/polygon mean spectrum, because the sanitised cube
  stores blanks as a value below the data minimum. Blanks are now skipped and
  the channel plots as a gap.
- **Windows build instructions rewritten** from the release workflow: Qt 6.10.1
  for MSVC 2022 with its add-ons, zlib → cfitsio → VTK built from source (vcpkg's
  VTK has no Qt support), `-DVISIVO_ENABLE_WEBRTC=OFF` as in the release, the
  DLLs the executable needs, and the backend environment. The docs no longer
  imply that only macOS and Linux build. When run from the build folder on
  Windows, the backend's `requirements.txt` is now copied too, so the first-run
  environment bootstrap works there.

- **PV diagram: fixed a mirrored spectral axis.** On a cube whose spectral axis
  decreases (`CDELT3 < 0`, most HI cubes) the diagram was drawn mirrored against
  its own axis, so emission appeared at the wrong velocity. Channels are now
  placed by their spectral value, taken from the backend; non-linear axes
  (e.g. `VOPT-F2W`) are placed correctly too. PV diagrams of such cubes made with
  earlier versions should be redone. Reported by P. Serra.
- **PV path, editable and live** (requested by P. Serra, in the manner of
  kpvslice): the finished path stays on the image and can be edited (drag a
  vertex, move it, Shift-drag to rotate, double-click to add a vertex,
  right-click to remove one); the diagram is recomputed as you edit. Width,
  *Smooth curve* and *New path* are controls of the diagram; the width dialog is
  gone. A colour bar shows `BUNIT`.
- **PV as FITS:** *Save as FITS…* (`POST /v1/products/pv_fits`): offset axis in
  arcsec and the cube's own spectral WCS, beam, `BUNIT`, path in `HISTORY`.
- **PV sampling:** sub-pixel vertices; a uniform step along the path measured on
  the sky (non-square and skewed pixels); NaN-aware bilinear interpolation;
  only the pixels around the path are read. Cubes whose spectral axis is not the
  last one are refused instead of being cut along a spatial axis.
- **Draw on a moment map:** arming PV, a region or the probe with a moment pane
  active draws on the map (it keeps its title and colours); the pane that showed
  the 2-D Slice keeps showing the channels, with the channel bar and the PV path.
  The PV path is drawn on every map pane of the field. *2D Slice* in a pane's
  menu restores the layout.
- **Zoom while drawing:** wheel zoom and middle-button pan stay available while a
  PV path or a region is being drawn.
- **Product panes showed native RA/Dec under a Galactic title:** their axis
  ticks now use the same frame conversion as the 2-D Slice.
- **Channel bar on the 2-D Slice:** the channel scrubber was only on the 3-D view,
  so a layout without it (slice + moment + PV) had no way to change channel; the
  2-D Slice now has its own, kept in step with the other.
- **One colour range for every channel:** *Same range on every channel* and
  *Cube range* for the 2-D Slice; a range chosen by hand is no longer undone by
  the next channel.
- **Fixed a crash on quit** when a PV diagram or a probe/profile plot was open.
- **PV diagram placement:** a first diagram with every pane taken grows the layout
  instead of being parked out of sight; live updates refresh it in place without
  moving the focus, the selection or the Inspector away from the path.
- **PV on full-Stokes cubes** (RA/DEC/FREQ/STOKES) works again (plane 0, as
  before); the FITS `PVSTEP` card is in arcsec; HISTORY carries one vertex per card.
- **Narrow panes (4-pane layout):** margins shrink with the pane, so a map keeps
  room; crowded axis labels are thinned and the axis title gives way; the channel
  bar keeps its slider and shows a long frequency as GHz/MHz. Overlay labels no
  longer reappear on every redraw.
- **Buttons inside a pane work on the first click** (they used to only activate
  the pane).
- **Readout and DISPLAY on a drawn-on map:** the cursor readout names the map
  instead of a channel; with the PV diagram active, the DISPLAY section says where
  its controls are instead of showing colour controls that do not apply.
- **Status bar:** "queued" counts jobs that are actually waiting (it counted every
  finished job still in the backend's task registry).
- **Kinematic Lasso, threshold and smoothing in the panel:** *Threshold* (1.0–5.0σ)
  and *Smoothing* (Off / Light / Medium / Strong, smooth-before-threshold) sit next
  to Reach and Velocity coupling; the field computation now honours the smoothing.
- **Lasso results are products:** *Isolate* / *Remove* add a LASSO entry to Session
  Data with how it was made (seed, reach, velocity coupling, threshold, smoothing,
  selected voxels, file); the new cube's FITS `HISTORY` carries the same recipe.
- **Lasso pick:** a double-click selects the emission under the cursor even with
  the cutting plane, a kinematic-model overlay or a catalogue in front of it (they
  used to stop the pick at their own position); isolines mode picks the surface.
- **Kinematic Model controls in the Inspector:** level, opacity, representation,
  colour and visibility of the model overlay are in Inspector › Parameters, like
  every other live tool, instead of a dock that overflowed the window. Tools on
  together stack: switching one off brings back the one still on.
- **Inspector › Parameters scrolls:** a tool's controls and a long recipe no longer
  squeeze each other; long values (file names) wrap, and copying them gives the
  plain text.
- **Axes above the channel bar:** the 2-D Slice and the Channels pane keep room
  for the channel bar below the x axis, whose labels and title it used to cover.
- **Polarimetry colour maps:** the polarisation-angle and Faraday-depth maps are
  drawn with the diverging CoolWarm map they ask for (a misspelt name left them
  sequential, with a header swatch that did not match).
- **RM synthesis samples φ max:** the φ grid includes φ max when the step reaches
  it (−80…80 at 1 is 161 samples; it stopped at +79). An axis tick at the origin reads `0.000`, not
  `-0.000`.
- **Pick Spectrum on Plane Click works:** a click on the cutting plane, the volume
  or the isosurface extracts the spectrum there. It never fired in the 3-D view
  (the camera style holds the mouse between press and release, and the pick was
  not listening that way). A press that moves is a rotation, not a pick.
- **Spectrum CSV provenance:** the file names the dataset, the pixel and its WCS
  position (to 10⁻⁵ degree in decimal format) of the spectrum actually shown;
  it said `pixel: (0, 0)` with no dataset or position. A probed spectrum's
  intensity column carries `BUNIT`, and the default file name the real pixel.
- **Spectrum statistics** include the median and σ(MAD), next to mean and RMS.
- **Channel maps keep square pixels:** a 196 × 280 cube was stretched into
  landscape panels (and in the enlarged view).
- **Export Movie at full size:** a channel scan started with only the 3-D view on
  screen came out 200 × 60 pixels; the needed view is now shown for the export
  (or the user is asked to show it), and movies default to the Movies folder
  rather than the program's working directory.
- **Channels are numbered from 1 everywhere:** Channel Maps, Estimate Noise and
  the Z range of Export Sub-Cube took 0-based channels while the channel bar,
  Compute Moment, Line-width and Export Movie counted from 1, and Channel Maps'
  own provenance printed the other numbering (30–80 became "31..81"). File names,
  status messages and polarimetry labels follow.
- **σ from Estimate Noise is used by the other tools:** the moment mask offers
  3σ of it (chosen by default), the line-width mask is set to 3σ of it, and the
  Kinematic Lasso counts its threshold in units of it; each records σ and the
  region it came from. Without an estimate nothing changes. The median reported
  for an even number of channels is now the true median.
- **Line-width map provenance** lists method, channels, mask and rest frequency
  (it had only the map type); the rest frequency is prefilled from `RESTFRQ`.
- The noise dialog's pixel count no longer uses the system's thousands
  separator ("54.880" read as a decimal).
- **Kinematic fit in the app:** *Fit from the lasso selection* in the Kinematic
  Model dialog (`POST /v1/cube/kinematic_fit_bin`) measures the selection on the
  raw cube (flux, w20/w50, V_sys, an S/N-cut velocity field), reads the geometry
  from moment 0, fits a flat and a per-ring disc to the velocity field, and fills
  in the parameters and the ring table. Velocity field, model and residual maps,
  the integrated spectrum and the source's measures become products with their
  provenance; *Save rings as JSON…* exports the rings. The validation script of
  the paper's use case now calls these same functions.
- **One velocity convention for model and fit:** a frequency axis is converted to
  optical velocity (cz), as HI catalogues quote it; the model used the radio
  convention (≈ 4 km/s apart at 1100 km/s).
- Residual and Faraday-depth maps are shown on a range symmetric about zero, so
  the diverging colour map's neutral colour is zero.
- **Residual of the model on screen:** after *Overlay*, with a lasso selection
  present, the residual v − v_model of the model actually in the fields (fitted or
  edited rings) is computed (`POST /v1/cube/kinematic_residual_bin`), reported in
  the dialog and added as a product — dropping or changing a ring is judged on the
  next overlay.
- **Scrolling no longer edits values:** a wheel over a spin box, combo box or
  slider inside a scrolling page (the Inspector, a tool window) scrolls the page;
  a control takes the wheel only once clicked. A fitted V_sys was changed from
  1120 to 1081 that way.
- **Velocity fields open from *Open…*:** a FITS with a length-3 component axis
  is recognised and opened in the velocity-field viewer; it opened as a blank 2-D
  image.
- **RM synthesis: Faraday depth above 6σ.** The service reports the noise of the
  peak |F| map, and the viewer adds *Faraday depth (peak |F| > 6σ)*, the map to
  read rotation measures from.
- **Lasso panel: per-channel profile and extent** of the selection, and the
  voxel count is recounted after a 2-D hand refinement (provenance and the FITS
  recipe recorded the count from before the edit).

## 1.0.1 (2026-10-16)

- **Polarimetry: blank sub-bands.** A release can flag a sub-band out entirely
  (two of the fourteen of the MeerKAT Bulge field G000.0+2.5 are NaN end to
  end). RM synthesis now leaves such planes out instead of returning an empty
  map, reports them (`dropped_planes`, `dropped_freq_hz`; the product records
  *Sub-bands dropped* by frequency) and refuses fewer than three usable
  sub-bands. PI/PA keeps NaN where there is no data instead of a measured zero,
  refuses a blank plane or an out-of-range channel, and drops blank line-free
  channels from the noise estimate. The Faraday transform is a BLAS matrix
  product (about 20× faster on a 900² cutout).
- **Kinematic lasso: optional smooth-before-threshold** (`smooth_v`, `smooth_xy`
  on the select request; off by default).
- **Remote backends:** a remote URL is never auto-started locally, and the local
  token is never sent to it.
- **Containers:** SoFiA-2 installed robustly in the Apptainer images
  (`ENABLE_SOFIA=1`); local artifacts excluded from the image staging.
- **Docs and CI:** documentation refreshed; release workflow triggers on plain
  numeric tags; docs sync on the right branches.

**next-server branch — spectral analysis improvements (2026-04)**

- **Moment orders 3, 4, 5 fully supported** — M3 (skewness = μ₃/σ³), M4 (excess kurtosis = μ₄/σ⁴ − 3), and M5 (standardised 5th moment = μ₅/σ⁵) are now correctly computed in both the numpy and Dask paths. All three are dimensionless and normalised consistently with one another. The C++ `isSupportedMomentOrder()` guard was extended to include these orders (previously returned "Unsupported moment order" at runtime).
- **Unified moment dialog** — the Moment submenu (six individual order actions) is replaced by a single **Tools → Compute Moment…** action that always opens a configuration dialog. The dialog lets the user select any supported order, channel range, and optional threshold mask in one step.
- **Surface real backend error text** — `MomentMapController` stores the backend error message with the `"MomentMap M<order>: …"` prefix and exposes it via `lastError()`. The status-bar deferred-error path in `vtkWindowCube` now shows the actual backend message instead of a generic string.
- **Line-width map guard** — `LinewidthDialog` validates `width > 0 && height > 0` before constructing the `ScienceMapWindow`, preventing a crash on all-NaN output (when all pixels fail to fit).
- **Session ID plumbing** — `LinewidthDialog`, `BaselineDialog`, and `StackDialog` all accept a `sessionId` parameter and call `client->setSessionId()` before making any request, fixing the double-slash URL and 401 errors that occurred when session ID was not forwarded.
- **StackDialog redesign** — replaced the plain list with a checkable `QListWidget`. A live counter label shows "N selected" in red when fewer than 2 cubes are chosen; the Compute button is disabled until at least 2 are selected.
- **Backend spectral endpoints** — `/v1/spectral/linewidth`, `/v1/spectral/linewidth/binary`, `/v1/spectral/baseline/{session_id}/{dataset_id}`, `/v1/spectral/stack`, `/v1/spectral/stack/binary` added. These support per-pixel Gaussian FWHM and equivalent-width fitting, polynomial/median baseline subtraction, and mean/median/weighted-mean cube stacking. Source-finder integration via `/v1/spectral/sourcefind` (SoFiA-2 backend).
- **next-server branch**

- **Guided startup flow** — a `StartupDialog` shown before `MainWindow` drives backend health-check, optional auto-start, and token setup. `BackendLauncher` manages the backend process lifetime and captures all stdout/stderr for the log panel.
- **Smart Python resolution** — `BackendLauncher` walks a six-level priority chain (`VISIVO_BACKEND_PYTHON` → active venv → project `.venv` → bundled Python → Settings Python → PATH fallback) to find the right interpreter without manual configuration.
- **Remote file browsing for image layers** — "File → Add New FITS File" in the image viewer now opens the backend-side `RemoteFileBrowserDialog` instead of a local file picker. The selected path is loaded through the same WCS-aware pipeline used by all other layer sources.
- **Unified VLKB layer alignment** — VLKB survey layers added from the inventory tree route through `addLayerImage()` → `loadImageLayer()` → `AstroUtils` / libwcs, the same pipeline as the standard "Add New Layer" path. All layer sources are now aligned consistently regardless of how they were added.
- **Loading placeholder in all viewers** — `vtkWindowImage`, `vtkWindowVbt`, and `vtkWindowVbtVolume` now display a centred "Loading…" message (font 16 pt, colour `#B8D6E8`) while data is being fetched, matching the existing pattern in `vtkWindowCube`.
- **Dockable workspace** — the 3D cube view and the 2D slice/detail view are now independent `QDockWidget` panels. Each panel can be floated, resized, or closed independently. A "Reset Layout" action in the View menu restores the default side-by-side arrangement.
- **3D View Settings tab** — the former "Threshold" sidebar tab is redesigned. Rendering mode (Isosurface / Volume), volume blend mode, threshold slider, and color map selection are organised into collapsible cards. Color Map and Surface Color cards are shown only for the active rendering mode.
- **Volume rendering blend modes** — Composite ray-casting, MIP, and MinIP are selectable both from the sidebar and from the View → Volume Rendering submenu. The sidebar and menu stay in sync bidirectionally.
- **Color map scope** — the LUT combo in the 3D View Settings tab now updates only the volume color transfer function. Slice and moment LUTs are managed independently and are not affected.
- **Non-blocking async controllers** — `NoiseController`, `MomentMapController`, and `PvController` dispatch computation via `QtConcurrent::run` with per-request heap-allocated `QFutureWatcher`. The main thread never blocks waiting for results.
- **WCS axes overlay** — toggled from View → Show WCS Axes.
- **Backend LRU cache** — the overlay CSV catalogue cache is bounded (`functools.lru_cache(maxsize=32)`) instead of growing without limit.
- **Plot background theme** — Settings → UI → Plot background lets the user choose between a dark (default) and light plot background. The preference is applied to the PV Diagram window and all spectrum/profile plot windows when they are opened. The setting is persisted in `Settings.ini` under `Plots/theme`.
- **PNG export for plots** — the PV Diagram window and the spectrum/profile plot windows each have a "Save as PNG…" button in the footer bar. Clicking it opens a file dialog and saves the current plot via `QCustomPlot::savePng()`, suitable for inclusion in papers and presentations.
- **Non-modal analysis results** — the Noise Estimation and Region Analysis result dialogs (`ToolResultDialog`) are now non-modal. The result window stays open while the user continues to interact with the cube viewer, and closes itself when dismissed.
