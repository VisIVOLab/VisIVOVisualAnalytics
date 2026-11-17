# SKAVA Discovery tab

The SKAVA Discovery tab searches a data discovery service, either
[SKAVA](https://github.com/VisIVOLab/SKAVA) (our own prototype, not a service
validated by SRCNet) or any service that speaks the IVOA TAP and DataLink
standards, and opens each dataset on the VisIVO
backend of the node that holds it, so the file is not transferred. From the same
tab you can run cutouts, moment maps, PV diagrams and source finders on that
node. Open it from the Data Hub's *Archives* section, from *Data ▸ SKAVA
Discovery* or from the command palette (⌘K). It opens as a tab of the main
window; closing and reopening it keeps the filters and results.

The tab only reaches a node through a VisIVO backend registered for it in
*Settings ▸ Backends ▸ Remote Backends*, with that backend's own token.

## Discovery service

| Field | What it does |
|---|---|
| *Service* (protocol) | *SKAVA JSON API* (SKAVA's own endpoints) or *IVOA TAP + DataLink (SRCNet)* (ADQL on `ivoa.obscore` and DataLink VOTables, as the SRCNet global services speak). |
| *Service* (URL) | The service's base URL. |
| *Token* | A bearer token pasted by hand, or the one obtained with *Sign in…*. |
| *Sign in…* | OpenID Connect sign-in to the IAM the service trusts (the SRCNet IAM, or Keycloak on a testbed). The sign-in is kept in the system's secure store, so later starts sign in silently. A service that answers with an `ivoa_bearer` challenge configures the endpoints by itself. |
| *IAM…* | Sets the IAM endpoints, client id and scope by hand. |
| *Advanced: TAP, DataLink, Computing Broker* | *TAP URL* and *DataLink URL* for the IVOA protocol (empty: SKAVA's own); *Broker URL*, a Computing Broker that places and runs source finders next to the data (empty: the finder runs as a task on the dataset's node). |

When a request is refused for lack of sign-in, the tab offers to sign in and
repeats the request.

## Filters

The *Filters (ObsCore; all optional)* section folds away with its arrow. All filters combine (AND); a blank field is ignored.

| Field | Notes |
|---|---|
| *Position*: RA, Dec, radius | Degrees. RA 0 to 360, Dec −90 to 90. Set all three or none. |
| *Band*: min, max, unit | MHz, GHz, Hz or m; converted to metres for the service. |
| *Time*: MJD min, max | Modified Julian Date. |
| *Dataset*: collection | ObsCore `obs_collection`. |
| *type* | `image`, `cube`, `timeseries`, `spectrum`. |
| *obs_id* | Exact match. |
| *limit*, *offset* | Pagination; `limit` is at most 1000. |

## Results

| Column | Content |
|---|---|
| *obs_id* | The dataset id (hover for the title). |
| *Target* | Target name. |
| *Type* | Data product type. |
| *Sky position* | RA and Dec. |
| *Band* | Spectral coverage, in MHz, GHz or μm. |
| *Time* | Time coverage (MJD). |
| *Served by* | The node SKAVA ranked first, and how many other replicas exist. |
| *Why* | SKAVA's reason for the ranking: measured round trip, CPU pressure, queue and memory margin of the node, or the declared scores. |

Hover *Served by* or *Why* for every replica in ranking order, with its cost,
reason and the age of the measurements it was judged on. The provenance of the
selected row is shown below the table.

## Opening a dataset

- *Open* (or double-click a row): opens the dataset on the first node, in
  SKAVA's order, that has a matching registered backend. The status bar's
  backend badge names the node, and its tooltip gives SKAVA's reason. If that
  backend refuses, the next node is tried and the status bar says so.
- *Open via…*: lists the replicas with SKAVA's reasons; pick one to open on that
  node first.

The dataset is added to Recent Datasets with the service, the replica and the
node, so reopening it asks the service again.

### How the desktop picks a node

A registered backend matches a node by URL (`localhost`, `127.0.0.1`, `::1` and
`host.docker.internal` count as the same host; port and path must match) or by
its *SRC code*, set in *Settings ▸ Backends* for a backend you reach at another
address than the catalogue advertises (a tunnel, a VPN). The backend must also
accept the dataset's type and have a token when it needs one. The desktop always
opens a node's own copy of the file.

When no node can serve the dataset, a replica reachable over HTTP is downloaded
through the local backend. A `file://` replica cannot be: the dialog then says
why each node was passed over and offers *Register backend…* or *Enter backend
token…*.

The local backend's automatic token is sent only to the local backend's address
(`http://127.0.0.1:8000` by default). A tunnel to a cluster backend therefore
uses another local port; the cluster helper's default, 18900, does.

## Compute next to data

*Compute next to data…* runs on the node that holds the dataset, through the
services the node advertises. The product stays on the node and opens from
there.

- *SODA cutout…*: a sky region (circle or polygon), a band and/or a time range,
  run as a job on the node and opened in place. The dialog does not select
  polarization; other VO clients can ask the node for Stokes planes. On a
  MeerKAT sub-band cube (spectral axis `SPECLNMF`), a band returns the whole
  sub-bands whose centre falls in it.
- *Moment map…*: order 0, 1 or 2, with an optional channel range and threshold.
- *PV diagram…*: the path's vertices in degrees and the slice width in pixels.
- *Source finder…*: the finders the node offers (SoFiA-2 for cubes, photutils
  for images) with their parameters. With a *Broker URL* set, the broker picks a
  node that has the finder and enough memory; otherwise the finder runs on the
  first-ranked node. The dialog follows the log, can cancel the run, and opens
  the mask (as a cube) and the catalogue (VOTable).

## Problems

| Message | Fix |
|---|---|
| *Set the SKAVA base URL before searching.* | Fill in the service URL. |
| *… requires a sign-in* | Sign in, or paste a token. |
| `no_available_nodes` | The service cannot reach any node holding the dataset (its backend stopped, or the tunnel closed). A node that comes back is used again within seconds. |
| `invalid_query - POS must be ...` | Set RA, Dec and radius together, or none. |
| *… is stored on an SRC node and can only be opened by the VisIVO backend running there* | No registered backend matches the nodes holding it. Register the node's backend (the dialog offers to) or check its token. |
| *SKAVA advertises no moment service for …* (or no PV service) | The dataset is not a cube, or the node's backend is older. Open the dataset and compute in the viewer. |

Details: how a node is chosen, in
[Distributed backend design](../../technical/design/distributed-backend-design.md);
running a backend as a node, in
[The backend as an SRC node](../../technical/deployment/src-node.md) and
[Running the backend on a cluster](../../technical/deployment/hpc-slurm.md);
the node's SODA service, in [SRC node and SODA](../../technical/backend-api/node-soda.md).
