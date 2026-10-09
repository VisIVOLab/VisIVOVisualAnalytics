# SKAVA Discovery tab

The **SKAVA** tab in the Data Hub queries an external SKAVA Data
Discovery and Access service (see the SKAVA D3.1 design and
[SKAVA project page](https://github.com/your-org/SKAVA)) and lets you
open the matching datasets directly in VisIVO without leaving the
application.

## How the flow works

```
[SKAVA tab]  →  GET /discovery/search       → list of matching datasets
                      (POS, BAND, TIME, COLLECTION, DPTYPE)
[user picks one in the table]
[Open button] →  GET /datalink/{obs_id}     → primary_access.access_url
              →  POST /v1/datasets/open_url → VisIVO backend downloads
                                              the FITS / HDF5 to its
                                              cache and opens it in the
                                              appropriate viewer
```

Discovery is **metadata-only**: SKAVA holds an ObsCore-style catalogue
of dataset descriptors plus replica routing information. The actual
FITS / HDF5 files live on storage nodes (or, for a fully local demo,
on your own VisIVO backend's Workspace Exports endpoint). The desktop
never bypasses the VisIVO backend — file downloads always go through
`POST /v1/datasets/open_url`, which caches them under
`<workspace_exports>/skava_cache/`.

## Configuration

Two settings persist between sessions (Settings dialog → **SKAVA**
section, or via the connection bar at the top of the tab):

* **SKAVA URL** — base URL of the SKAVA discovery service
  (e.g. `http://localhost:8000` for a dev container, or your SRC's
  public discovery endpoint).
* **Token** — optional bearer token. If SKAVA was started with
  `SKAVA_ACCESS_TOKENS` set, paste one of those tokens here. Empty
  when the service does not require authentication.

The settings sync both ways: edits in the tab's connection bar are
saved as you press Enter; conversely, changes from the Settings
dialog show up in the connection bar the next time the tab opens.

## Scientific filters

The form mirrors the ObsCore parameters supported by SKAVA's
`GET /discovery/search` (per the *discovery_queries* SKAVA guide).
All filters combine with `AND` semantics; leave any field blank to
skip it.

| Field | What it sends to SKAVA | Notes |
|---|---|---|
| **RA / Dec / Radius** (deg) | `POS=CIRCLE <ra> <dec> <radius>` | CIRCLE is the only shape supported by the current SKAVA baseline. RA in [0, 360], Dec in [-90, 90]. |
| **BAND min / max** + unit | `BAND=<λ_min> <λ_max>` (metres) | The unit selector (MHz / GHz / Hz / m) tells the panel how to convert from your input to **metres**, the unit SKAVA expects internally. Radio astronomers typically enter frequencies and let the panel handle the conversion. |
| **TIME MJD min / max** | `TIME=<mjd_min> <mjd_max>` | Modified Julian Date floats. |
| **COLLECTION** | `COLLECTION=<value>` | Free-form string matching ObsCore `obs_collection` (e.g. `MGCLS`, `LoTSS-DR2`, `WALLABY`). |
| **DPTYPE** | `DPTYPE=<value>` | Dataproduct type: `image`, `cube`, `timeseries`, `spectrum`. |
| **obs_id** | `obs_id=<value>` | Exact-match lookup of a known identifier. |
| **limit / offset** | `limit=…&offset=…` | Pagination. `limit` ≤ 1000, `offset` ≤ 1 000 000. |

Press **Search** (or just hit Enter inside any field) to run the query.

## Results table

Each row is one SKAVA discovery result. Columns:

| Column | Source |
|---|---|
| **obs_id** | `metadata.obs_id` |
| **Target** | `metadata.target_name` |
| **Type** | `metadata.dataproduct_type` |
| **Sky position** | `metadata.s_ra` / `metadata.s_dec` (formatted as `RA°, Dec°`) |
| **Band** | `metadata.em_min` / `em_max` (metres on the wire, formatted in MHz / GHz / μm depending on the value) |
| **Time** | `metadata.t_min` / `t_max` (MJD) |
| **Best node** | `best_node` from the routing block |
| **Latency** | best replica's `latency_score` (ms) |

Hover the **obs_id** cell for the full `obs_title` tooltip.

## Provenance panel

Below the results table, a panel shows the **selected dataset**'s
ObsCore provenance metadata:

* **Title** — `obs_title`
* **DOI** — clickable link to `https://doi.org/<doi>` (or the raw
  value if it already starts with `http`)
* **PID** — persistent identifier (e.g. `skava:demo:dataset-1`)
* **License** — typically `CC-BY-4.0`, `Public Domain`, etc.
* **Citation** — full citation string; **Copy citation** copies it to
  the clipboard for pasting into papers / notebooks.

## Opening a dataset

Click **Open selected in VisIVO** (enabled once a row is selected).
The panel:

1. Resolves the access URL by querying
   `GET /datalink/{obs_id}` on SKAVA and reading
   `primary_access.access_url`.
2. Hands the URL to the VisIVO backend via
   `POST /v1/datasets/open_url`.
3. The backend downloads the file with streaming `urllib` to
   `<workspace_exports>/skava_cache/`, classifies it with the same
   `_fits_metadata` used for local opens (so HDF5 / PSRFITS / dynamic
   spectrum sidecar conversion all kick in automatically), and
   registers it as a session dataset.
4. The matching viewer opens (image viewer for `kind=image` or
   `dynspec`, cube viewer for `kind=cube`).
5. The dataset is added to the **Recent Datasets** panel with the
   SKAVA `obs_id` as the display label, so reopening it later
   re-runs the full discovery → download → open chain.

A status line under the action bar reports `Showing N of M results
(limit=X, offset=Y)` so you know whether to paginate.

## Caching

`POST /v1/datasets/open_url` keeps a hash-keyed cache under the
backend's Workspace Exports directory
(`~/.visivo/exports/skava_cache/`). Re-opening the same SKAVA URL is
free — no second download. To force a re-download (e.g. after the
underlying file has changed upstream) delete the matching cache file
manually.

## Common failure modes

* **`SKAVA base URL is not configured`** — populate the URL field
  before searching.
* **`Search failed: invalid_query — POS must be ...`** — SKAVA's
  `parse_pos` rejected the spatial filter. Double-check ra / dec /
  radius are all set (or all blank).
* **`Open from SKAVA: Remote returned HTTP 404`** — the
  `primary_access.access_url` for the chosen dataset is unreachable.
  Causes: the storage node is down, the file has moved, or for the
  local demo seed the file is not in `~/.visivo/exports/` under the
  expected name. Check the SKAVA seed catalogue's `access_endpoint`
  fields to see what filename the discovery layer expects.
* **`Cached file could not be classified`** — the file downloaded
  fine but didn't pass the FITS / HDF5 classifier (e.g. it's HTML
  from an error page). Inspect the cache file directly to see what
  came back.
