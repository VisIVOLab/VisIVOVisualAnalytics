# SKAVA Discovery tab

The **SKAVA** tab in the Data Hub queries a data discovery service of the
SKA Regional Centre network, SKAVA ([VisIVOLab/SKAVA](https://github.com/VisIVOLab/SKAVA))
or any service that speaks the IVOA standards, and opens the datasets it
finds where they are: on the VisIVO backend of the node that holds the
copy, when SKAVA advertises one, so that nothing is transferred. The same
tab runs cutouts, moment maps, PV diagrams and source finders on that node,
and shows the result in VisIVO.

## How the flow works

```
[SKAVA tab]  →  GET /discovery/search  (or TAP on ivoa.obscore)
                 → matching datasets, each with the replicas ranked by SKAVA
                   and the reason for the ranking
[user picks one]
[Open]       →  GET /datalink/{obs_id}  (or the row's DataLink document)
                 → the replicas, the VisIVO backend on each node, the node's
                   SODA and product services
             →  routing: the first registered backend in SKAVA's order
             →  POST /v1/datasets/open_skava on that backend: opened in place
                 (a refusal moves on to the next node; a download through the
                 local backend is the last resort)
```

Discovery is metadata only: the service holds an ObsCore catalogue plus the
replicas and their routing. The files stay on the nodes. The desktop talks
to a node only through the VisIVO backend registered for it
(Settings ▸ Backends), with that backend's token.

## Discovery service

The box at the top of the tab (saved in the settings, shared with the
Settings dialog) has two rows, the service and the token; the endpoints a
default deployment never sets are folded under **Advanced**:

| Field | Meaning |
|---|---|
| **Service** (protocol) | *SKAVA JSON API* (SKAVA's own endpoints) or *IVOA TAP + DataLink* (ADQL on `ivoa.obscore` and DataLink VOTables, what the SRCNet global services speak). |
| **Service** (URL) | The service's base URL. |
| **Token** | A bearer token pasted by hand, or the one obtained with **Sign in…**. |
| **Sign in… / IAM…** | OpenID Connect sign-in to the IAM the service trusts (the SRCNet IAM, or Keycloak on a testbed), as a public client with `vlva://callback` as redirect URI. **IAM…** sets the endpoints, the client id and the scope. A service that answers `401` with an `ivoa_bearer` challenge names its IAM's discovery document, and the tab configures the endpoints from it on its own. The sign-in is kept in the system's secure store, so the next start signs in silently. |
| **Advanced: TAP URL**, **DataLink URL** | For the IVOA protocol: the TAP service (`/sync` is appended) and the DataLink service used when a TAP row carries no DataLink `access_url` of its own. Empty: SKAVA's own `/tap` and `/datalink/<id>`. |
| **Advanced: Broker URL** | A Computing Broker (the SRCNet job API) that places and runs the source finder next to the data. Empty: the finder runs as a task on the dataset's node, started by the desktop. |

When a request comes back `401`, the tab offers to sign in and repeats the
request afterwards.

## Filters

The **Filters** section (fold it away with its arrow to give the results
more room) mirrors the ObsCore parameters of `GET /discovery/search` and of
the ADQL query built for TAP. All filters combine with `AND`; leave a field
blank to skip it.

| Field | What it sends | Notes |
|---|---|---|
| **Position**: RA / Dec / radius (deg) | `POS=CIRCLE <ra> <dec> <radius>` | RA in [0, 360], Dec in [-90, 90]. |
| **Band**: min / max + unit | `BAND=<λ_min> <λ_max>` (metres) | The unit selector (MHz / GHz / Hz / m) converts your input to metres, the unit the services expect. |
| **Time**: MJD min / max | `TIME=<mjd_min> <mjd_max>` | Modified Julian Date. |
| **Dataset**: collection | `COLLECTION=<value>` | ObsCore `obs_collection`. |
| **type** | `DPTYPE=<value>` | `image`, `cube`, `timeseries`, `spectrum`. |
| **obs_id** | `obs_id=<value>` | Exact match. |
| **limit / offset** | pagination | `limit` ≤ 1000. |

## Results table

The table and the provenance of the selected row share the rest of the
tab; drag the divider between them. Short columns fit their content, the
**Why** column takes the remaining width, and a cell cut short shows its
whole text on hover.

| Column | Source |
|---|---|
| **obs_id** | `metadata.obs_id` (hover for the title) |
| **Target** | `metadata.target_name` |
| **Type** | `metadata.dataproduct_type` |
| **Sky position** | `s_ra` / `s_dec` |
| **Band** | `em_min` / `em_max`, shown in MHz / GHz / μm |
| **Time** | `t_min` / `t_max` (MJD) |
| **Served by** | the node SKAVA ranked first, and how many other replicas there are |
| **Why** | SKAVA's reason for that ranking: with the resource-aware policy the measured round trip, CPU pressure, queue and memory margin of the node; with the static policy the declared scores |

Hover **Served by** or **Why** for the whole decision: every replica in
ranking order with its cost, reason and the age of the telemetry it was
judged on.

## Opening a dataset

**Open** (or a double-click on the row) resolves the dataset's DataLink and opens it:

1. The DataLink lists the replicas in SKAVA's order and, for each node, the
   VisIVO backend co-located with it (`visivo-backend` service descriptor,
   or the `#auxiliary` row pointing at the backend's `/v1/node/status` in
   the VOTable).
2. The desktop takes the first node whose backend is registered in
   Settings ▸ Backends, opens the dataset *in place* there
   (`POST /v1/datasets/open_skava` with the replica's `file://` URL), and
   shows the viewer. The backend badge in the status bar names the node;
   its tooltip gives SKAVA's reason.
3. If that backend refuses, the next node in the ranking is tried, and the
   status bar says so. When no node can serve it, the tab explains why for
   each descriptor and, when SKAVA advertises a backend that is not
   registered, offers to register it (or to enter its token).
4. An `http(s)` replica with no co-located backend is downloaded through
   the local backend (`POST /v1/datasets/open_url`).

**Open via…** lists the replicas with SKAVA's reasons: pick one to open on
that node instead of the first-ranked one.

The dataset is added to the Recent Datasets panel with the catalogue it
came from, the protocol, the replica and the node that served it, so a
reopen asks the same service again (and routes afresh) rather than reusing
a path that may have moved.

## Compute next to data

The **Compute next to data…** menu runs on the node, through the services
SKAVA's DataLink advertises for the dataset (`node-soda-sync`,
`node-soda-async`, `visivo-moment`, `visivo-pv` as JSON descriptors; the
`#cutout` and `#proc` rows with their `adhoc:service` descriptors in the
VOTable). The product stays in the node's workspace and is opened from
there.

* **SODA cutout…**: a POS (CIRCLE or POLYGON), BAND and/or TIME subset,
  submitted to the node's `/soda/async` as a UWS job (`PHASE=RUN`), polled
  until it ends, then opened in place. Without an advertised SODA job
  service, the backend's own `/v1/datasets/cutout` route is used.
* **Moment map…**: `OPERATION=MOMENT` with the order (0, 1, 2), an optional
  channel range and threshold.
* **PV diagram…**: `OPERATION=PV` with the path's vertices (degrees) and the
  slice width in pixels.
* **Source finder…**: the finders the node offers (`GET /v1/finders`:
  SoFiA-2 for cubes, photutils for images) with their parameters. With a
  **Broker URL** set, the job goes to the Computing Broker
  (`POST /v1/jobs`), which asks SKAVA where to run it, keeps to the nodes
  that have the finder and the memory it needs (each node states how much
  its finders need per byte of dataset in `GET /v1/node/status`; the
  desktop sends no memory figure of its own) and starts the finder on that
  node's backend; otherwise the desktop opens the dataset on the
  first-ranked node and starts the finder there as a task
  (`POST /v1/tasks/sourcefind`). The dialog follows the run's log, can
  cancel it, and opens the mask (as a cube) and the catalogue (VOTable)
  from the node.

The desktop calls a node's service at the address of the backend
registered for that node (the DataLink names the node as the catalogue
sees it, which need not be reachable from the desktop), with that
backend's token.

## Running a backend as an SRC node

A backend next to a node's data serves `GET /v1/ping` and
`GET /v1/node/status` (CPU, memory under the container's limits, queued
tasks, finders) without a token, so a catalogue can measure the node. The
settings that matter on a node:

| Variable | Default | Meaning |
|---|---|---|
| `VISIVO_DATA_ROOTS` (or `VISIVO_DATA_ROOT`) | none | the directories the backend may open; every `file://` location a client or a catalogue gives it must lie inside them (see [Sharing one backend between users](../multi-user-backend.md)) |
| `VISIVO_NODE_CODE` | none | the node's code as the catalogue knows it, reported in the status document |
| `VISIVO_NODE_STATUS_PUBLIC` | `1` | `0` makes `/v1/node/status` require the backend token (`/v1/ping` stays open) |
| `VISIVO_NODE_GPU` | unset | declares a GPU (`1`) or none (`0`) in the status document |
| `VISIVO_SOFIA_BIN` | `sofia2` or `sofia` on `PATH` | the SoFiA-2 executable for source finding on the node |
| `VISIVO_FINDER_TIMEOUT_S` | `3600` | the longest a source finder may run |

The node's SODA service (`/soda/sync`, `/soda/async`) cuts and computes on
the files under its data roots. A synchronous request with only `ID`, and
no `POS`, `BAND` or `TIME`, returns the file as stored, byte for byte, with
its own media type (`application/fits` for FITS).

When the catalogue cannot reach the backend (a firewall, a NAT, a tunnel),
the backend can report its status document to the catalogue instead, every
few seconds:

| Variable | Meaning |
|---|---|
| `VISIVO_SKAVA_TELEMETRY_URL` | the catalogue's base URL (`http` or `https`) |
| `VISIVO_SKAVA_NODE_TOKEN` | this node's telemetry token, issued by the catalogue's operator |
| `VISIVO_NODE_CODE` | this node's code in that catalogue |
| `VISIVO_SKAVA_TELEMETRY_INTERVAL` | seconds between reports (default 10, at least 1) |

Reporting is off unless the first three are set. The backend sends the
token with each report and never follows a redirect with it. A report that
fails is logged once per outage, without the token, and the backend keeps
reporting.

How the catalogue measures nodes, issues their tokens and uses the reports
is described in the
[SKAVA documentation](https://skava.readthedocs.io/en/latest/deployment/configuration.html#node-reports-telemetry-pushed-by-the-node).

## Common failure modes

* **`SKAVA base URL is not configured`**: fill the URL before searching.
* **`… requires a sign-in`**: the service answered `401`. Sign in, or paste
  a token.
* **`Search failed: invalid_query - POS must be ...`**: the spatial filter
  was rejected; set RA, Dec and radius together, or none.
* **"… is stored on an SRC node and can only be opened by the VisIVO
  backend running there"**: no registered backend matches the nodes
  holding the dataset. Register the node's backend (the dialog offers to),
  or check its token.
* **"SKAVA advertises no moment service"**: the dataset is not a cube, or
  the node's backend predates the operation; open the dataset and compute
  in the viewer instead.
