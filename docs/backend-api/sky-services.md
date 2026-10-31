# HiPS, cone search and VLKB

Remote sky services: HiPS surveys and hips2fits cutouts, name resolution and
VO cone search, and the VLKB archive (cutouts, multi-cutout jobs, TAP
catalogue queries).

## HiPS

### `POST /v1/hips/open`
```json
{ "url": "http://alasky.u-strasbg.fr/DSS/DSS2Merged" }
```
Returns `survey_id`, `order_min`, `order_max`, `frame`, `tile_format`, `ra_center`, `dec_center`, `fov`.

### `POST /v1/hips/hips2fits`
Cut a FITS (or a picture) out of any HiPS survey, via the CDS hips2fits service,
into the Workspace.

```json
{ "hips": "CDS/P/DSS2/color", "width": 512, "height": 512,
  "ra": 83.82, "dec": -5.39, "fov": 0.5, "projection": "TAN",
  "coordsys": "icrs", "rotation_angle": 0.0, "format": "fits" }
```
Returns `output_path`, `workspace_filename`, `bytes`, `is_fits` and `url` — the
request as sent, so a cutout is reproducible outside the application.

Every parameter is checked before the request leaves: an empty survey, a field
of view of zero, a declination off the sky or a cutout over 30 Mpx is refused
with the reason. `min_cut`, `max_cut` and `stretch` are sent only for `jpg`/`png`
— they turn numbers into pixels, and a FITS carries the numbers. A response that
is not a FITS when one was asked for is reported with the service's own
`description` rather than saved under a `.fits` name.

### `GET /v1/hips/hips2fits/surveys`
The starting survey list plus the accepted projections and stretches. The legacy
carried these as a `QStringList` inside the dialog, so correcting a survey id
meant releasing a client.

### `GET /v1/hips/surveys?kind=image&refresh=false`
Every HiPS registered with the CDS MOCServer: `id`, `title`, `url`, `category`,
`regime`, `frame`, `tile_formats`, `kind` and `max_order`. `kind` is the
registry's `dataproduct_type` (`image`, `catalog`, `cube`, …) and the default
filter keeps only the imagery, which is all the viewer can draw as tiles; pass
an empty `kind` for the lot. Surveys whose frame is a planetary body rather
than a sky (`mars`, `moon`, `io`, …) are never returned — they have no
celestial position. The raw document is cached under the HiPS cache
directory for a week, in a file named after the query so changing the requested
fields invalidates it; `refresh=true` fetches it again. A registry that cannot
be reached answers `valid: false` with the reason rather than failing.

### `GET /v1/hips/{survey_id}/allsky?order=<N>`
AllSky mosaic PNG/JPEG bytes.

### `GET /v1/hips/{survey_id}/tile/{order}/{pix}`
Single HiPS tile bytes.

### `POST /v1/hips/{survey_id}/query_tiles`
Given a viewport (RA/Dec center + FOV), return the HEALPix tiles covering it —
NESTED pixel index, centre and four corners per tile — at the requested order,
clamped to the survey's `max_order`. A field of 360° means the whole sphere and
returns every tile of the order (`12 × 4^order` of them), which is what the
all-sky projections ask for. `radius_deg` is the angular radius the client
measured for its own viewport; without it the disc is derived from `fov_deg`,
which is quoted across the width and so misses the corners of a tall window or
a curved projection. **The position is in ICRS and so is every position
returned**, whatever frame the survey is tiled in: the centre is converted into
the survey's frame to pick the tiles and the tile centres and corners are
converted back. Over half the CDS registry is galactic, and reading those tile
coordinates as RA/Dec puts the imagery nowhere near the field asked for. Requires **healpy** on the backend; without it the
route answers `valid: false` with an install hint rather than failing.

### `POST /v1/hips/catalogue_overlay`
Given a HiPS survey viewport and the path of a **CSV/TSV on the backend
filesystem** (confined to the configured data roots), return the rows falling
inside the field as `BackendHiPSCatalogueSource[]`. Selection is by angular
separation from the centre, not by a box in RA — near a pole every right
ascension is in view and no interval says so. `radius_deg` is the radius the
client measured for its own viewport, corners included; without it the radius
is guessed from `fov_deg` and the corners are cut off. It does not query Simbad or
VizieR — use `/v1/resolve/cone_search` for that and overlay the CSV it writes.

---

## Resolve

### `POST /v1/resolve/target`
```json
{ "name": "M87" }
```
Returns `ra_deg`, `dec_deg`, `resolved_name`.

### `POST /v1/resolve/cone_search`
A VO Simple Cone Search against any service.

```json
{ "url": "https://vizier.cds.unistra.fr/viz-bin/conesearch/II/246/out",
  "ra": 83.82, "dec": -5.39, "radius": 0.05, "verbosity": 2, "max_rows": 5000 }
```
Returns `url`, `columns`, `rows` (capped at `max_rows`), `total_rows`,
`truncated`, `position_columns` (`{"ra": …, "dec": …}` when they could be
identified) and `output_path` — **the whole result saved as CSV**, which is what
lets a search end in a catalogue the viewers can open.

Two documents are refused before astropy sees them: one carrying a `STREAM`
with an `href` — the parser would fetch that address itself, outside every
guard, so a service could have `file:///etc/passwd` or an internal port come
back as table cells — and one carrying a DTD, which no VOTable needs and which
is an entity-expansion denial of service waiting to happen. The check parses the
document rather than matching a pattern, because an attribute value may contain
`>`.

`RA`/`DEC`/`SR`/`VERB` are merged into whatever query the service URL already
carries (a VizieR cone-search URL has its own parameters, and appending a second
`?` produces something that is not a URL). The URL is user-supplied, so it goes
through the SSRF guard; when `VISIVO_CONE_SEARCH_ALLOWED_HOSTS` is set, only the
hosts it lists are accepted.
A service reports failure *inside* a valid VOTable, so an empty table is not
necessarily an empty sky — that `INFO value="ERROR"` is raised as the error.

### `GET /v1/resolve/cone_search/services`
A few known endpoints, so the field starts with something.

---

## VLKB

### `POST /v1/vlkb/fetch_cutout`
Download a cutout from a *remote* SODA service to the backend's filesystem.
```json
{ "soda_url": "https://…/soda/sync?ID=…&POS=CIRCLE%2040%200%200.3&POSSYS=GALACTIC",
  "vlkb_bearer": "" }
```
Returns `{valid, error, path}` — `path` is on the **backend**, and is what the
dataset-open route is then given.

The URL comes from the client, so it is checked against SSRF before anything is
fetched: private, loopback and link-local targets are refused, and when
`VISIVO_VLKB_ALLOWED_HOSTS` is set only the hosts it lists are accepted.

**What came back is checked before it is called a FITS file.** A SODA service
answers an error with HTTP 200 and a VOTable or an HTML page, and mislabels its
Content-Type in both directions — the observed failure carried
`Content-Type: application/fits` on a plain-text error. So the check is on the
bytes:

| First bytes | Reported as |
|-------------|-------------|
| `SIMPLE` | a FITS file; the path is returned |
| `\x1f\x8b` | gzip — ask for it uncompressed |
| `ustar` at offset 257 | a tar archive — this ID needs the multi-cutout endpoint |
| anything else | the service's own message, extracted from the body |

The message is extracted from a VOTable `<INFO value="ERROR">`, an HTML
`<title>`/`<h1>`/`<p>`, or plain text, and is returned as **plain text**: markup
is stripped, because it is written by a remote service and ends up in a dialog.
A response that is not FITS is deleted rather than kept, and a *cached* response
is re-checked for the same reason — a bad file saved before this check would
otherwise be handed back for ever, and re-requesting the same URL is exactly
what a user does after a failure.

| Variable | Default | Effect |
|----------|---------|--------|
| `VISIVO_VLKB_MAX_BYTES` | 8 GiB | Cap on a single cutout download |
| `VISIVO_VLKB_ALLOWED_HOSTS` | *(unset)* | When set, the only hosts accepted; every other host is refused |

### Multi-cutout (asynchronous batch)

One SODA `sync` request cuts one dataset; the archive's answer to "cut twenty"
is a UWS job. Five routes drive it, and the protocol lives in
`app/vlkb_mcutout.py`.

#### `POST /v1/vlkb/mcutout/submit`
```json
{ "items": [{ "id": "ivo://…", "pos": "CIRCLE 40 0 0.3", "possys": "GALACTIC" }],
  "service_url": "", "vlkb_bearer": "" }
```
`pos` is the same POS string the single-cutout path sends, so a batch covers
exactly the region the user was shown; it is parsed server-side into the
service's `{circle}`/`{range}` shape. Returns
`{valid, error, job_id, phase, count}`.

**A submitted job is never lost.** If the submit succeeds but the first phase
read fails, the route still returns the `job_id` with `phase: "UNKNOWN"` —
reporting an error without the id would leave the batch running on the archive
with nobody able to collect it, and a retry would submit a second one.

#### `POST /v1/vlkb/mcutout/phase`
`{job_id, service_url?, vlkb_bearer?}` → `{valid, error, phase, terminal}`.
A job the archive left `HELD` (accepted but not started) is started here rather
than reported as stuck.

#### `POST /v1/vlkb/mcutout/abort`
`{job_id, service_url?, vlkb_bearer?}` → `{valid, error, phase, terminal}`.
Stops a running job on the archive (UWS `PHASE=ABORT`) and reports the phase
the archive gives afterwards; a job that had already finished keeps its phase.

#### `POST /v1/vlkb/mcutout/report`
→ `{valid, error, results: [{index, ok, filename, detail}]}`, in submission
order. `detail` keeps whatever the archive said about a failure — the only clue
to why one dataset of twenty came back empty.

#### `POST /v1/vlkb/mcutout/fetch`
→ `{valid, error, archive, files, bytes}`. Downloads the job's `.tar.gz` **and
unpacks it**, into a staging directory of its own per fetch; `files` are the
cutouts, ready for the ordinary dataset-open route. Goes through the heavy-task
throttle.

**What is refused, and why.** A `job_id` is interpolated into a URL *and* into
that directory, so it must be a plain identifier (`.` and `..` included in the
refusal). Redirects are never followed on these calls: `urllib` copies the
`Authorization` header into the redirected request, so a service answering 302
with a host of its choosing would be handed the user's VLKB token — the two
calls whose answer *is* a 3xx (submit, and the RUN command) read the `Location`
without following it. The token is never sent over plain HTTP. On extraction, a
member that resolves outside the directory, a symlink, a device, or a **sparse**
member is refused — for a sparse member `tarfile` does not check that the map
agrees with the declared size, so the size stops bounding what gets written —
and the totals are capped.

| Variable | Default | Effect |
|----------|---------|--------|
| `VISIVO_VLKB_MCUTOUT_TIMEOUT` | 300 s | Socket timeout for each archive call |
| `VISIVO_VLKB_MCUTOUT_MAX_ITEMS` | 500 | Datasets in one batch |
| `VISIVO_VLKB_MCUTOUT_MAX_BYTES` | 16 GiB | Results archive download |
| `VISIVO_VLKB_MCUTOUT_MAX_EXTRACTED` | 32 GiB | Total unpacked size |
| `VISIVO_VLKB_MCUTOUT_MAX_MEMBERS` | 2000 | Entries in the archive |
| `VISIVO_VLKB_MCUTOUT_MAX_REPORT` | 8 MiB | Job report |
| `VISIVO_VLKB_URL` | *(unset)* | Default archive base URL. The multi-cutout routes have no fallback: without it and without `service_url` they answer 503 |

---

### `POST /v1/vlkb/tap`
Run a VLKB catalogue query — one of the named presets over a Galactic box, or
ADQL of your own — and save the result as a CSV catalogue in the Workspace.

```json
{ "preset": "bubbles", "glon_min": 10.0, "glon_max": 12.0,
  "glat_min": -1.0, "glat_max": 1.0, "band": "", "max_rows": 0 }
```
Returns `url`, `query` (the ADQL actually run — a preset is built on the backend,
so this is how the user sees what it asked for), `columns`, `rows` (a preview),
`total_rows`, `truncated`, `output_path`, `workspace_filename` and
`hit_row_limit`.

| Preset | Table |
|--------|-------|
| `bandmerged` | `compactsources.sed_view_final` (positions are `glonft`/`glatft`) |
| `band` | `compactsources.higal<70\|160\|250\|350\|500>` |
| `filaments` | `filaments.filaments` joined to `filaments.branches` |
| `bubbles` | `bubbles.bubbles` |
| `distances` | `compactsources.distances`, with `x`/`y`/`z` derived from `dist`, `glon`, `glat` — this is what makes a 3-D selection a point cloud rather than a list |

These are the legacy's own queries, with one addition: it had a bubble mode —
a button, a selector and an importer — and no bubble query, because
`generateQuery()` had no branch for it, so choosing bubbles ran the compact-source
query. Only `SELECT` statements are forwarded (TAP sync is a read interface), and
the service URL goes through the SSRF guard (`VISIVO_VLKB_ALLOWED_HOSTS`). The
service is the request's own URL, else `VISIVO_VLKB_URL`, else
`https://vlkb.ia2.inaf.it/vlkb`.

### `GET /v1/vlkb/tap/presets`
The presets, the Hi-GAL bands and the configured service URL, so the client does
not carry the VLKB schema inside it.

---

## Fetching on the client's behalf

`/v1/resolve/cone_search`, `/v1/hips/hips2fits` and `/v1/vlkb/tap` all fetch a
URL the client chose or can override. They go through one helper
(`app/safe_fetch.py`), which applies the SSRF policy to the URL **and to every
redirect target** — validating only the first URL is not enough, since a public
host is free to answer `302 Location: http://169.254.169.254/` and `urlopen`
follows redirects by default. Each has its own allowlist variable
(`VISIVO_CONE_SEARCH_ALLOWED_HOSTS`, `VISIVO_HIPS2FITS_ALLOWED_HOSTS`,
`VISIVO_VLKB_ALLOWED_HOSTS`). When it is set, only the hosts it lists are
accepted and every other host is refused, public ones included; a listed host
is not subject to the private-address check, which is how a service on a
private network is reached deliberately.

Failures these routes can explain — a bad parameter, a service's own error
message — come back in `error`. Anything unforeseen is logged with its traceback
and answered with a fixed sentence: `str(exc)` on an unexpected exception
carries server paths and mount points to whoever asked.
