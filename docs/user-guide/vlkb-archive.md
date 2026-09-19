# VLKB archive

The **VLKB** tab of the main window — beside *Data Hub* and *SKAVA*, and
reachable from the Command Palette (⌘K / Ctrl+K) as *VLKB Inventory* — queries
the ViaLactea Knowledge Base: you give it a region of the Galactic plane, it
lists every image and spectral cube that covers it, and it cuts out the piece
you asked for and opens it.

Three steps, and each one tells you what it is doing.

## 1 · Choosing the region

Fill in the galactic coordinates of the centre, then give the region a size —
and the size decides the **shape**:

```{list-table}
:header-rows: 1
:widths: 26 74

* - Filled in
  - Region sent
* - **radius**
  - A cone: `CIRCLE l b r`.
* - **dl** and **db**
  - A box: `RANGE l₁ l₂ b₁ b₂`, with dl and db as the *full* widths.
* - neither
  - Nothing. Query stays disabled and says what is missing.
```

Under the fields, a line shows the region that will actually be sent, updated as
you type:

```
Will query a cone
CIRCLE 40.000000 0.000000 0.300000
```

This is the same string the archive receives and the same one the cutout is
taken with — not a separate rendering of it.

```{note}
The **Selection** radios above (*None / Point / Rectangular*) are not part of
the query. They choose what you are allowed to **draw** on the all-sky view; the
drawing then fills in the coordinate fields. What is queried is always what the
fields say.
```

### Longitude near the Galactic centre

Galactic longitude wraps at 360°, and a box around l = 0 legitimately runs from,
say, 359° to 3°. VisIVO sends that as it stands — `RANGE 359 3 …` — which is how
the archive expresses a wrapped interval, and says so under the fields. A box
360° wide is sent as `RANGE 0 360`, meaning every longitude.

## 2 · The inventory

The result is a window listing everything that covers the region, grouped by
wavelength regime and product type, with a count in the header.

```{list-table}
:header-rows: 1
:widths: 22 78

* - Column
  - What it is for
* - **Dataset**
  - Survey and band or line — "THOR 1.44 GHz", "GLIMPSE I 3.6 um".
* - **Coverage**
  - `full` when the dataset covers the whole region, `partial` when it only
    overlaps it. Full-coverage rows are also highlighted green.
* - **Identifier**
  - The tail of the archive identifier. An inventory routinely lists six rows
    that read *THOR 1.44 GHz*; this is what tells them apart.
```

Two ways to narrow it down: the **filter box** matches survey, band and line
names, and **Only datasets that fully cover the region** hides everything that
merely clips a corner.

## 3 · Downloading

Double-click a dataset, or select several — ⌘/Ctrl-click, or shift-click for a
run — and press **Download & Open**, which counts what it will fetch. Each
dataset is cut to your region by the archive, downloaded **to the backend**, and
opened:

- **images** are added as layers to the viewer this inventory session already
  opened, so a set of bands stacks in one window rather than scattering across
  six;
- **cubes** open in the spectral cube viewer.

The viewer loads one layer at a time, so a batch queues and goes in one after
another — the status line says how many are waiting. A download that fails does
not cancel the rest.

## When the archive says no

A cutout request can fail for ordinary scientific reasons — the region falls
outside the survey, the identifier needs a different endpoint — and the archive
explains itself in the body of its reply, not in the HTTP status.

VisIVO checks that what came back **is** a FITS file, and when it is not it
shows you the archive's own words:

> The service returned application/fits instead of FITS: UsageError : overlap
> computation could not be completed with the given arguments.

```{tip}
A message of that shape is about the **server**, not your query. It is worth
sending to whoever runs the archive: it names the failure exactly.
```

The check is on the file's contents, not on the type the service declares —
services mislabel FITS as `application/octet-stream` routinely, and mislabel
their error pages as FITS just as readily. A gzip or a tar archive is recognised
and named as such rather than handed to the FITS reader, which could only ever
report the same *"No SIMPLE card found"* whatever had actually gone wrong.

## See also

- [Catalogues, HiPS, and SAMP](catalogues-hips) — overlaying the VLKB *source
  catalogue* on an image you already have open, which is a different route
  through the same archive.
- [SED fitting](sed-fitting) — what to do with a band-merged source's fluxes.
- [Backend API](../backend-api) — `/v1/vlkb/fetch_cutout`.
