# SRC node and SODA

A backend deployed on an SKA Regional Centre (SRC) node exposes two groups of routes for the services around it. The node routes tell a catalogue or a monitoring system whether the node is alive and how much work it can take on. The SODA routes implement the IVOA SODA service on the node, so that a subset or a derived product of a dataset is computed where the data are stored and only the result leaves the node.

## SRC node

The node routes are public by default: a catalogue (such as SKAVA) or a monitoring system can probe the node without holding a user token. They report capacity figures only, never users, datasets or paths. Setting `VISIVO_NODE_STATUS_PUBLIC` to `0`, `false` or `no` makes `GET /v1/node/status` and `GET /v1/metrics` require the `X-Visivo-Token` header (401 without a valid token). `GET /v1/ping` is always open.

### `GET /v1/ping`

Liveness probe for round-trip measurements. Constant and cheap, no authentication.

Response: `{"ok": true}`.

### `GET /v1/node/status`

What the node can take on now: CPU and memory under its limits, tasks, render sessions, available source finders. The same document is what the node pushes to its catalogue.

Figures are read where they hold for this process: in a container, from its cgroup (v2, or the v1 memory controller on older hosts), including the limits of parent groups; inside a SLURM job, also from the memory the scheduler granted (`SLURM_MEM_PER_NODE`, or `SLURM_MEM_PER_CPU` times `SLURM_CPUS_ON_NODE`); elsewhere, from the operating system. A field that cannot be read is `null`.

| Field | Type | Description |
|---|---|---|
| `node_code` | string or null | `VISIVO_NODE_CODE`, or `null` when unset. |
| `version` | string | `VISIVO_VERSION`, or the backend's version. |
| `timestamp` | float | Unix time of the snapshot. |
| `cpu.cpus` | float | CPUs granted: the CPUs the process may run on, capped by the cgroup CPU quota (rounded to 2 decimals). |
| `cpu.limited` | bool | `true` when a CPU quota applies or the process may run on fewer CPUs than the host has. |
| `cpu.utilisation_pct` | float or null | Share of the granted CPUs used since the previous status request, in %. `null` on the first request and outside a cgroup. |
| `cpu.pressure_pct` | float or null | Pressure stall information, `some avg10`: share of the last 10 s in which a task waited for CPU, in %. |
| `cpu.load_1m` | float or null | The host's 1-minute load average (meaningful only outside a cgroup). |
| `memory.total_bytes` | int or null | The tightest memory limit (cgroup limits, SLURM grant), or the host's memory without limits. |
| `memory.available_bytes` | int or null | Memory still available under that limit: the limit minus the working set (usage minus reclaimable file cache), never more than the host has available. |
| `memory.pressure_pct` | float or null | Memory pressure stall information, `some avg10`, in %. |
| `disk.workspace_free_bytes` | int or null | Free disk space in the backend's workspace (exports) directory. |
| `workers.total` | int | Compute worker processes. |
| `tasks.running` | int | Background tasks running. |
| `tasks.queued` | int | Background tasks started but waiting to run. |
| `render.active` | int | Active server-side render sessions. |
| `render.capacity` | int | Render session cap. |
| `gpu` | bool or null | `VISIVO_NODE_GPU` as declared by the operator (`1`/`true`/`yes` or `0`/`false`/`no`); `null` when unset. |
| `platform` | string | Machine architecture (for example `x86_64`, `ppc64le`). |
| `capabilities.kinds` | array | Dataset kinds the node handles: `["image", "cube", "dynspec"]`. |
| `capabilities.finders` | array | Source finders available on this node, among `sofia2` and `photutils`. |
| `capabilities.finder_memory` | object | For each available finder, `{"factor": f, "floor_bytes": b}`: the finder needs about `f` times the dataset size plus `b` bytes. |

```json
{
  "node_code": "INAF-CT",
  "version": "1.0.0",
  "timestamp": 1793440000.12,
  "cpu": {"cpus": 16.0, "limited": true, "utilisation_pct": 12.5, "pressure_pct": 0.0, "load_1m": 3.1},
  "memory": {"total_bytes": 68719476736, "available_bytes": 51539607552, "pressure_pct": 0.0},
  "disk": {"workspace_free_bytes": 912345678901},
  "workers": {"total": 4},
  "tasks": {"running": 1, "queued": 0},
  "render": {"active": 0, "capacity": 8},
  "gpu": true,
  "platform": "x86_64",
  "capabilities": {"kinds": ["image", "cube", "dynspec"], "finders": ["sofia2", "photutils"],
                   "finder_memory": {"sofia2": {"factor": 2.3, "floor_bytes": 134217728},
                                     "photutils": {"factor": 8.0, "floor_bytes": 536870912}}}
}
```

Errors: 401 only when `VISIVO_NODE_STATUS_PUBLIC` disables public access and the token is missing or wrong.

### `GET /v1/metrics`

The same status in the Prometheus text exposition format (`text/plain; version=0.0.4`). Every metric is a gauge; a metric whose value is unknown (`null` in the status document) is omitted. Booleans are written as 1 or 0.

| Metric | Status field |
|---|---|
| `visivo_cpus` | `cpu.cpus` |
| `visivo_cpu_utilisation_percent` | `cpu.utilisation_pct` |
| `visivo_cpu_pressure_percent` | `cpu.pressure_pct` |
| `visivo_memory_total_bytes` | `memory.total_bytes` |
| `visivo_memory_available_bytes` | `memory.available_bytes` |
| `visivo_memory_pressure_percent` | `memory.pressure_pct` |
| `visivo_workspace_free_bytes` | `disk.workspace_free_bytes` |
| `visivo_workers` | `workers.total` |
| `visivo_tasks_running` | `tasks.running` |
| `visivo_tasks_queued` | `tasks.queued` |
| `visivo_render_sessions` | `render.active` |
| `visivo_render_capacity` | `render.capacity` |
| `visivo_gpu` | `gpu` |

```text
# HELP visivo_cpus CPUs granted to the node.
# TYPE visivo_cpus gauge
visivo_cpus 16.0
```

Errors: as for `GET /v1/node/status`.

## SODA

The SODA routes follow SODA 1.0, with UWS 1.1 for asynchronous jobs and VOSI for the service metadata.

### Authentication and ownership

All `/soda/sync` and `/soda/async` routes require the `X-Visivo-Token` header; a missing or wrong token gives 401 with a JSON body (`{"detail": "Missing or invalid X-Visivo-Token header."}`). `/soda/capabilities` and `/soda/availability` are open.

An asynchronous job belongs to the token that created it. The job list shows only the caller's jobs, and every job route answers 404 for a job owned by another token, exactly as for a job that does not exist. A job with no owner is reachable by any token only on a single-token backend.

### Parameters

Parameter names are case-insensitive, values are not. Parameters can be sent in the query string or, with `POST`, as an `application/x-www-form-urlencoded` body; a `multipart/` body is refused. Each parameter may appear once (a repeated non-empty value gives `MultiValuedParamNotSupported`). A parameter that the selected operation does not understand is refused with a `UsageError`, so a misspelled name is never ignored silently. `OPERATION`, `PHASE`, `RUNID` and `REQUEST` are accepted with every operation.

| Parameter | Value | Description |
|---|---|---|
| `ID` | `file://` URL | The dataset: the replica's identifier on this node, the `file://` URL that SKAVA's DataLink hands out. The path must lie inside the node's data roots. `http(s)://` URLs and non-local hosts are refused. Required. |
| `OPERATION` | `CUTOUT` (default), `MOMENT`, `PV` | What to compute. `MOMENT` and `PV` run only as asynchronous jobs. |
| `RESPONSEFORMAT` | `application/fits` | Optional. `image/fits` and `application/x-fits` are also accepted; anything else is a `UsageError`. |
| `RUNID` | string | Optional label stored with an asynchronous job. |

`CUTOUT` parameters (positions are ICRS degrees, converted to the dataset's own frame, Galactic included):

| Parameter | Value | Description |
|---|---|---|
| `POS` | `CIRCLE <ra> <dec> <radius>` | The square of side twice the radius around the centre, trimmed to the dataset. |
| | `RANGE <ra1> <ra2> <dec1> <dec2>` | The pixel box covering the coordinate range. |
| | `POLYGON <ra1> <dec1> <ra2> <dec2> <ra3> <dec3> ...` | At least three vertices; reduced to their bounding box. |
| `CIRCLE` | `<ra> <dec> <radius>` | Same as `POS=CIRCLE ...`. |
| `POLYGON` | `<ra1> <dec1> ...` | Same as `POS=POLYGON ...`. |
| `BAND` | `<min> <max>` | Wavelength interval in metres, converted to the dataset's spectral axis (frequency, wavelength or energy axes). |
| `TIME` | `<mjd_lo> <mjd_hi>` | MJD interval. A cube has no time axis to cut: the interval is checked against the observation time recorded in the header, and a miss selects nothing. Without a recorded time it is not applied. |

Only one of `POS`, `CIRCLE` and `POLYGON` may be given. A `POS` on a dataset with no celestial axes, or a `BAND` on a dataset with no spectral axis, is a `UsageError`. The cutout keeps all axes, cuts the celestial and spectral ones, shifts `CRPIX` accordingly and is written as a single primary HDU.

**Whole file.** A `CUTOUT` request with none of `POS`, `CIRCLE`, `POLYGON`, `BAND` or `TIME` returns the dataset as stored on `/soda/sync`: the original file with every HDU and the original header, not a rewritten copy (this is what a DataLink `#this` link through the service fetches). Any regular file inside the data roots can be returned this way, including compressed FITS and HDF5. `RESPONSEFORMAT`, if given, must match the file's own media type (`application/fits` for `.fits`, `.fit`, `.fts`; `application/x-hdf5` for `.h5`, `.hdf5`, `.he5`; `application/gzip` for `.gz`; `application/octet-stream` otherwise), since the node converts nothing. An unconstrained `CUTOUT` submitted as an asynchronous job is not served as stored: it produces a FITS file holding the first image HDU's full data.

`MOMENT` parameters:

| Parameter | Value | Description |
|---|---|---|
| `MOMENT` | integer 0 to 10 | Moment order. Required. |
| `CHANNELS` | `<first> <last>` | 0-based inclusive channel range, `0 <= first <= last`. Default: the whole spectral axis. |
| `THRESHOLD` | number | Voxels below this value are masked. Default: no mask. |

The result is the 2-D moment map as FITS.

`PV` parameters:

| Parameter | Value | Description |
|---|---|---|
| `PVPATH` | `<ra1> <dec1> <ra2> <dec2> [...]` | Path vertices in ICRS degrees, at least two. Required. A path outside the dataset's projection selects nothing. |
| `PVWIDTH` | positive integer | Width of the path in pixels. Default 1. |

The result is the position-velocity diagram as FITS.

### Errors

SODA errors follow DALI: a `text/plain` body that starts with the error type, followed by the message.

| Status | Body | When |
|---|---|---|
| 400 | `UsageError: <message>` | Invalid or missing parameter, unknown parameter, unsupported `RESPONSEFORMAT`, an `ID` that cannot be served (wrong scheme, outside the data roots), a file that is not a supported dataset (FITS or HDF5), a cutout that cannot be made. |
| 400 | `MultiValuedParamNotSupported: <message>` | A parameter given more than once. |
| 404 | `UsageError: ID not found on this node: ...` | The `ID` resolves to no file. |
| 404 | `UsageError: no such job: <job_id>` | Unknown job, or a job of another token. |
| 401 | JSON `{"detail": ...}` | Missing or invalid token. |

A valid request that selects nothing (a position, band or time that misses the data) is not an error: `/soda/sync` answers 204 with no body, and an asynchronous job completes with a short text result that says why.

### `GET /soda/sync`

### `POST /soda/sync`

Synchronous `CUTOUT`. Answers 200 with the FITS cutout (`application/fits`, `Content-Disposition: attachment; filename="<stem>_cutout.fits"`), or the whole file as stored when no constraint is given (see above). The `X-Soda-Applied` header lists the constraints that took effect, comma-separated from `pos`, `band` and `time` (empty for the whole file). `OPERATION=MOMENT` or `PV` is a `UsageError`: use `/soda/async`.

```text
GET /soda/sync?ID=file:///data/hi/cube.fits&POS=CIRCLE%20150.1%202.2%200.05&BAND=0.2105%200.2112
```

Errors: see [Errors](#errors); 204 for an empty selection.

### `GET /soda/async`

The caller's SODA jobs as a UWS job list (`text/xml`, one `uws:jobref` with its `uws:phase` per job). With `Accept: application/json` the answer is a JSON array of `{"job_id", "phase", "href"}`.

### `POST /soda/async`

Creates a job from the parameters above (any `OPERATION`). The parameters are validated at creation, so an invalid request fails here with the errors above and no job is created. The job is created in phase `PENDING`; with `PHASE=RUN` it starts at once (any other `PHASE` value is a `UsageError`). Answers 303 with `Location` set to the job URL.

```text
POST /soda/async
Content-Type: application/x-www-form-urlencoded

ID=file:///data/hi/cube.fits&OPERATION=MOMENT&MOMENT=1&CHANNELS=120%20240&THRESHOLD=0.004&PHASE=RUN
```

Job phases map from the backend's task registry: `PENDING` (created, not started), `QUEUED`, `EXECUTING`, `COMPLETED`, `ERROR`, `ABORTED`.

A job's product is written in the backend workspace under `soda/<job_id>`. Jobs are kept by the task registry and forgotten after `VISIVO_TASK_TTL_SECONDS` (default 1800) without activity, unless still running; the product directory is removed with the job.

### `GET /soda/async/{job_id}`

The job as a UWS 1.1 `uws:job` document (`text/xml`): job id, run id, phase, creation, start and end times (UTC, millisecond precision), `executionDuration` 0, destruction time, parameters, results and, for a job in `ERROR`, the error summary. `ownerId` is always nil.

With `Accept: application/json` the answer is a JSON object:

| Field | Description |
|---|---|
| `job_id`, `phase`, `operation`, `run_id` | Identity and state. |
| `parameters` | `ID` and the operation's parameters. |
| `creation_time`, `start_time`, `end_time`, `updated_time`, `destruction` | ISO 8601 UTC times, or `null`. |
| `progress` | Fraction from 0 to 1. |
| `results` | `[{"id": "result", "href": ".../results/result"}]` once completed, else `[]`. |
| `error` | The error message, or `null`. |
| `output_path` | Completed with a product only: the product's path on the node, so a client of this backend can open it in place. |
| `applied` | Completed with a product only: the constraints applied (for a cutout), or `moment` / `pv`. |
| `no_data` | Completed with an empty selection only: the reason. |

### `POST /soda/async/{job_id}/phase`

`PHASE=RUN` starts a `PENDING` job (a `UsageError` in any other phase). `PHASE=ABORT` aborts a pending, queued or executing job; the job goes to `ABORTED` at once and its product, if any, is deleted. A running moment or PV computation stops within one slab; a running cutout runs to its end but its output is discarded. `ABORT` on a finished job changes nothing. Any other value is a `UsageError`. Answers 303 to the job URL.

### `GET /soda/async/{job_id}/results/result`

The product: the FITS file (`application/fits`) of a completed job, or, for a completed job whose selection was empty, a `text/plain` body starting with `No data:`. 404 (`UsageError: job <id> has no result (phase <phase>)`) when the job is not completed or the file is gone.

### `DELETE /soda/async/{job_id}`

Aborts the job if it is still running, deletes its product directory and forgets the job. Answers 303 to the job list.

### Other UWS job resources

All require the token and answer 404 for an unknown job or a job of another token.

| Route | Response |
|---|---|
| `GET /soda/async/{job_id}/phase` | The phase as `text/plain`. |
| `GET /soda/async/{job_id}/results` | A `uws:results` document listing the result link (empty until completed). |
| `GET /soda/async/{job_id}/error` | The error message as `text/plain` (empty when there is none). Messages start with `UsageError:` or `FatalError:`. |
| `GET /soda/async/{job_id}/parameters` | A `uws:parameters` document. |
| `GET /soda/async/{job_id}/quote` | Empty `text/plain`: no completion estimate is made. |
| `GET /soda/async/{job_id}/executionduration` | `0` (`text/plain`): no limit beyond the backend's own task timeouts. |
| `POST /soda/async/{job_id}/executionduration` | Ignored: the service keeps its own limits. 303 to the job. |
| `GET /soda/async/{job_id}/destruction` | The destruction time (ISO 8601 UTC, the last update plus the task retention time), as `text/plain`. |
| `POST /soda/async/{job_id}/destruction` | Ignored, as above. 303 to the job. |
| `GET /soda/async/{job_id}/owner` | Empty `text/plain`: owners are tokens and are never disclosed. |

### `GET /soda/capabilities`

VOSI capabilities (`text/xml`): the SODA sync (`ivo://ivoa.net/std/SODA#sync-1.0`) and async (`ivo://ivoa.net/std/SODA#async-1.0`) interfaces and the two VOSI endpoints, each with its full access URL on this node. No authentication.

### `GET /soda/availability`

VOSI availability (`text/xml`): always `<vosi:available>true</vosi:available>` when the service answers. No authentication.
