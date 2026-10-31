# The backend as an SRC node

A VisIVO backend that runs next to an SRC node's data serves it without moving
it: the desktop opens a replica in place, and cutouts, moment maps, PV
diagrams and source finders run on the node, so only the product leaves it. A
discovery catalogue such as SKAVA lists the node's backend in its DataLink
answers and ranks the node on what the backend reports about itself. How the
desktop chooses among the nodes is described in
[SKAVA discovery](../user-guide/skava-discovery.md#how-the-desktop-picks-a-node);
starting a backend on a SLURM node in
[Running the backend on a cluster](hpc-slurm.md). The routes below are listed
in the [Backend API reference](../backend-api.md).

## Settings on a node

| Variable | Default | Meaning |
|---|---|---|
| `VISIVO_DATA_ROOTS` (or `VISIVO_DATA_ROOT`) | none | The directories the backend may open (`VISIVO_DATA_ROOTS` is a `:`-separated list; `VISIVO_DATA_ROOT` names one). Every `file://` location a client or a catalogue gives it must lie inside them. Without them a single-token backend opens any path; see [Sharing one backend between users](../multi-user-backend.md). |
| `VISIVO_NODE_CODE` | none | The node's code as the catalogue knows it, reported in the status document. |
| `VISIVO_VERSION` | the application's version | What the status document reports as the version. |
| `VISIVO_NODE_STATUS_PUBLIC` | `1` | `0` (or `false`, `no`) makes `/v1/node/status` and `/v1/metrics` require the backend token. `/v1/ping` stays open. |
| `VISIVO_NODE_GPU` | unset | Declares a GPU (`1`) or none (`0`) in the status document; unset reports nothing. |
| `VISIVO_CGROUP_ROOT` | the process's own cgroup | The cgroup directory to read limits from (see [Node capacity under SLURM](hpc-slurm.md#node-capacity-under-slurm)). |
| `VISIVO_SOFIA_BIN` | `sofia2` or `sofia` on `PATH` | The SoFiA-2 executable. |
| `VISIVO_FINDER_TIMEOUT_S` | `3600` | The longest a source finder may run, in seconds. |

`--data-root`, `--node-code` and `--version` of the SLURM launcher set the
first three, and a job given a GPU sets `VISIVO_NODE_GPU=1`.

## Status for whoever routes work to the node

Three routes let a catalogue or a monitoring system measure the node without
holding a user's token. They report capacity figures, never users, datasets
or paths.

| Route | Access | Answer |
|---|---|---|
| `GET /v1/ping` | always open | `{"ok": true}`: a constant, cheap answer for round-trip probes. |
| `GET /v1/node/status` | open unless `VISIVO_NODE_STATUS_PUBLIC=0` | The status document (JSON). |
| `GET /v1/metrics` | open unless `VISIVO_NODE_STATUS_PUBLIC=0` | The same facts in the Prometheus text format. |

The status document:

| Field | Content |
|---|---|
| `node_code`, `version`, `timestamp`, `platform` | Who answers, which build, when, on which architecture. |
| `cpu` | `cpus` granted (the cgroup quota, never more than the CPUs the process may run on), `limited`, `utilisation_pct` since the previous call, `pressure_pct` (share of the last 10 s with CPU stalls), `load_1m` (the host's). |
| `memory` | `total_bytes` (the tightest limit), `available_bytes` (that limit minus the working set), `pressure_pct`. Read from cgroup v2, else cgroup v1, with the SLURM grant as one more limit, else the host's memory: see [Node capacity under SLURM](hpc-slurm.md#node-capacity-under-slurm). |
| `disk` | `workspace_free_bytes`: free space where products are written. |
| `workers`, `tasks` | The process-pool size; tasks `running` and `queued`. |
| `render` | Server-side render sessions `active` and the `capacity` (`VISIVO_RENDER_MAX_SESSIONS`). |
| `gpu` | As declared by `VISIVO_NODE_GPU` (`true`, `false` or `null`). |
| `capabilities` | `kinds` (`image`, `cube`, `dynspec`), the available `finders`, and `finder_memory`: for each finder, the memory a run needs as `factor` times the dataset's bytes plus `floor_bytes`. |

The Prometheus metrics, all gauges (one is left out when its value is
unknown): `visivo_cpus`, `visivo_cpu_utilisation_percent`,
`visivo_cpu_pressure_percent`, `visivo_memory_total_bytes`,
`visivo_memory_available_bytes`, `visivo_memory_pressure_percent`,
`visivo_workspace_free_bytes`, `visivo_workers`, `visivo_tasks_running`,
`visivo_tasks_queued`, `visivo_render_sessions`, `visivo_render_capacity`,
`visivo_gpu`.

## Reporting the status to the catalogue

When the catalogue cannot reach the backend (a firewall, a NAT, a tunnel),
the backend can send its status document to the catalogue instead, at a fixed
interval: a `POST` to `<catalogue>/internal/telemetry/report` with the node's
code, the status and the round-trip time measured on the previous report.

| Variable | Meaning |
|---|---|
| `VISIVO_SKAVA_TELEMETRY_URL` | The catalogue's base URL (`http` or `https`, no credentials in it). |
| `VISIVO_SKAVA_NODE_TOKEN` | This node's telemetry token, issued by the catalogue's operator. |
| `VISIVO_NODE_CODE` | This node's code in that catalogue. |
| `VISIVO_SKAVA_TELEMETRY_INTERVAL` | Seconds between reports (default 10, at least 1). |

Reporting is off unless the first three are set; a URL set without the token
or the node code, a malformed token or a URL that is not plain `http(s)` is logged as a
warning and nothing is sent. The token travels in the `X-Skava-Node-Token`
header and speaks for this node only. A redirect is treated as a failure and
never followed with it. A report that fails is logged once per outage, without
the token, and the backend keeps reporting.

How the catalogue registers nodes, issues their tokens and uses the reports is
described in the SKAVA documentation:
[telemetry reports](https://skava.readthedocs.io/en/latest/api/telemetry.html).

## SODA on the node

The node's IVOA SODA service cuts and computes on the files under its data
roots (`backend/app/routers/soda.py`). `ID` is the replica's `file://` URL, as
the catalogue's DataLink hands it out, and must lie inside the data roots.
Parameter names are case-insensitive. Requests need the backend token
(`X-Visivo-Token`), except the two VOSI routes.

| Route | What it does |
|---|---|
| `GET` or `POST /soda/sync` | A cutout, streamed as FITS. Only `OPERATION=CUTOUT` (the default) runs synchronously. |
| `/soda/async` | A UWS 1.1 job list: `POST` creates a job (with `PHASE=RUN` it starts at once) and answers `303` to it; the usual UWS resources follow (`phase`, `results`, `error`, `parameters`, `destruction`, ...), and `DELETE` removes a job. |
| `GET /soda/capabilities` | VOSI capabilities: the sync and async interfaces. Open. |
| `GET /soda/availability` | VOSI availability. Open. |

Operations, selected with `OPERATION`:

| `OPERATION` | Parameters | Result |
|---|---|---|
| `CUTOUT` (default) | `POS` (`CIRCLE`, `RANGE` or `POLYGON`), or `CIRCLE` / `POLYGON` on their own; `BAND` (metres); `TIME` | The FITS cutout. |
| `MOMENT` (async only) | `MOMENT=<order>` (0 to 10); optional `CHANNELS=<first> <last>` (0-based, inclusive) and `THRESHOLD=<value>` (data below it masked) | The 2-D moment map as FITS. |
| `PV` (async only) | `PVPATH=<ra1> <dec1> <ra2> <dec2> [...]` (ICRS degrees, at least two vertices); optional `PVWIDTH=<pixels>` (default 1) | The position-velocity diagram as FITS. |

`RESPONSEFORMAT`, when given, must be FITS (`application/fits`). A parameter an
operation does not know is refused rather than ignored. Errors follow DALI:
`text/plain` bodies starting with `UsageError:` or `FatalError:`. A valid
synchronous cutout that selects nothing answers `204`.

A synchronous request with only `ID` (no `POS`, `CIRCLE`, `POLYGON`, `BAND` or
`TIME`) returns **the file as stored**, byte for byte, every HDU and the
original header, with its own media type (`application/fits` for FITS,
`application/x-hdf5`, `application/gzip`, otherwise
`application/octet-stream`). This is what a DataLink `#this` link through the
service fetches. `RESPONSEFORMAT` must then be the file's own format: the node
converts nothing.

Products are written in the backend's workspace, one directory per job, which
is removed when the job is aborted, deleted or expires. The UWS result link
streams the file; the JSON form of a job (`Accept: application/json`) also
carries `output_path`, so a client of this backend can open the product in
place, as the desktop does.

SKAVA's catalogue can also answer a SODA request itself by redirecting it
(`303`) to the SODA service of the node that holds the replica; the catalogue
side is described in the SKAVA documentation:
[SODA](https://skava.readthedocs.io/en/latest/api/soda.html).

## Source finders

`GET /v1/finders` lists the finders the node knows, available or not, with
their parameters and, when one is unavailable, why. Two ship with the backend:

* `sofia2`: SoFiA-2 for spectral cubes, available when the executable is
  installed (`VISIVO_SOFIA_BIN`, or `sofia2` / `sofia` on `PATH`);
* `photutils`: image segmentation for 2-D images, always available.

`POST /v1/tasks/sourcefind` (`dataset_id` of a dataset opened on this backend,
`finder`, default `sofia2`, and `params`) starts a finder as a task. An
unknown finder answers `404`, an unavailable one `503`, and a dataset of a kind
the finder does not handle `422`. The finder writes a label mask (FITS on the
dataset's grid) and a catalogue (VOTable) in the workspace under
`sourcefind/<task_id>/`; the task's result names them, and a cancelled or
failed run removes them. A run is stopped after `VISIVO_FINDER_TIMEOUT_S`
seconds.

The available finders and their memory needs are also part of the status
document, so a placement service (a Computing Broker) can send a finder job
only to a node that has the finder and enough memory for the dataset.
