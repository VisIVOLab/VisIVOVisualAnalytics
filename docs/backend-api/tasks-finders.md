# Tasks and source finders

## Tasks

For long-running operations the backend queues a task; the client polls for completion.

### `POST /v1/tasks/moment` · `POST /v1/tasks/pv`
Same request body as the synchronous equivalents (`/v1/products/moment`,
`/v1/cube/pv`); the dataset must be a cube (422 otherwise). Returns
`{valid, error, task_id, status: "running", cache_hit}`.

A task is not bound by `VISIVO_HEAVY_TIMEOUT_S`, the 900 s ceiling of the
synchronous routes. It runs until it completes or is cancelled, unless the
operator sets `VISIVO_TASK_TIMEOUT_S`. Use the task route for a moment or PV
over a large cube or channel range: on one HPC node an M2 over 450 GB takes
about 25 minutes (`benchmark/RESULTS.md`).

### `POST /v1/tasks/moment_fits`
The task form of `POST /v1/products/moment_fits`: it computes a moment map and
writes it to Workspace Exports as a 2-D FITS file, without the synchronous
route's `VISIVO_HEAVY_TIMEOUT_S` limit. Use it for large cubes.

Request: the same body as `/v1/products/moment_fits` (`MomentSaveFitsRequest`).

| field | type | default | meaning |
|---|---|---|---|
| `dataset_id` | string | required | an open cube of this session |
| `moment_order` | int | required | 0, 1, 2, … as for `/v1/products/moment` |
| `channel_start`, `channel_end` | int | 0, 0 | inclusive range, clamped to the cube |
| `mask_enabled` | bool | false | keep only voxels ≥ the threshold |
| `threshold_value` | float | 0.0 | the threshold |
| `threshold_auto` | bool | false | with `mask_enabled`: median + 3σ (MAD) of the range |
| `moment2_variance` | bool | false | order 2: variance instead of dispersion |
| `output_basename` | string | `""` | file name in Workspace Exports; empty gives `<cube stem>_m<order>.fits` |

Answers at once:

- 200 `{valid: true, error: "", task_id, status: "running", cache_hit: false}`;
- 404 when `dataset_id` is not open in the session;
- 422 when the dataset is not a cube or the body is malformed.

Poll `GET /v1/tasks/{task_id}`; its `operation` is `"moment_fits"`. When
`status` is `completed`, `result` has the fields of the synchronous route's
response (`MomentSaveFitsResponse`):

```json
{ "valid": true, "error": "", "output_path": "/…/exports/cube_m0.fits",
  "workspace_filename": "cube_m0.fits", "width": 1286, "height": 1286,
  "range_min": -0.01, "range_max": 2.3, "bunit": "Jy/beam",
  "moment_unit": "Jy/beam Hz" }
```

A failed computation gives `status: "failed"` with the reason in `error` (no
`result`). `DELETE /v1/tasks/{task_id}` cancels it: the worker stops at the
next slab. A task that fails or is cancelled leaves no file: what was
reserved or written is removed once its worker has ended. The file name is
reserved when the task is created, as an empty placeholder in Workspace
Exports until the map is written. If the name
is already taken, a numeric suffix is added (`cube_m0_1.fits`), so read the
name from `result.workspace_filename`.

### `GET /v1/tasks/{task_id}`
```json
{ "valid": true, "error": "", "task_id": "...", "operation": "moment",
  "status": "completed", "created_at": "…", "updated_at": "…",
  "progress": 1.0, "cache_hit": false, "result": { … } }
```
`status` is `pending`, `running`, `completed`, `failed` or `cancelled`.
`result` holds the same payload as the synchronous route once the task has
`completed`; `error` holds the reason of a failure. A task is visible only to
the token that created it (404 for any other, as for an unknown id).

### `DELETE /v1/tasks/{task_id}`
Request cancellation of a pending or running task: `{cancelled, task_id}`.
404 for an unknown task, 409 when it has already finished. Cancellation is
best-effort: the task may still complete first. A moment or PV worker polls a
stop file between slabs or blocks: the streamed path and the block-wise (dask)
path that the backend uses above 512 MB both stop within one slab or block.

On the desktop, moment maps, moment FITS exports and PV diagrams follow their
task with `visivo::followBackendTask()`. It polls after 250 ms, then backs off by
1.5× per poll up to 3 s, with no wall-clock cap, and sends
`DELETE /v1/tasks/{task_id}` when the user aborts. It falls back to the
synchronous route only when task creation answers 404 or 405 (a backend
without the task route). A PV whose path the user changes while it runs is
cancelled on the backend, and the newest path starts; closing the cube window
cancels the PV under way.

---

## Source finders

Source finders are plugins (`backend/app/compute/finders`) behind one
interface: a finder writes a label mask (FITS on the dataset's grid, 0 for
background, n for the n-th source) and a VOTable catalogue, and returns the
catalogue rows. Two ship with the backend:

| Finder | Works on | Available when |
|---|---|---|
| `sofia2` | `cube` | the SoFiA-2 executable is found (`VISIVO_SOFIA_BIN`, or `sofia2`/`sofia` on `PATH`) |
| `photutils` | `image` | always |

The list of available finders is also published in
[`/v1/node/status`](node-soda.md#get-v1nodestatus).

### `GET /v1/finders`
Every finder this node knows, available or not:

```json
{ "finders": [
  { "name": "sofia2", "kinds": ["cube"],
    "description": "SoFiA-2 smooth-and-clip source finder for spectral-line cubes.",
    "parameters": { "scfind.threshold": "S/N threshold of the smooth-and-clip finder (default 5.0)", "…": "…" },
    "available": false,
    "reason": "SoFiA-2 executable not found (set VISIVO_SOFIA_BIN or put sofia2 on PATH)",
    "memory_factor": 2.3, "memory_floor_bytes": 134217728 } ] }
```

`parameters` maps each parameter a caller may set to a one-line description.
`memory_factor` and `memory_floor_bytes` estimate a run's memory as
`factor × dataset bytes + floor`.

### `POST /v1/tasks/sourcefind`
Run a finder as an asynchronous task.

```json
{ "dataset_id": "...", "finder": "sofia2",
  "params": { "scfind.threshold": 4.0, "linker.minSizeZ": 3 } }
```

`finder` defaults to `sofia2`. For SoFiA-2, `params` keys are SoFiA's own
(`section.key`), limited to the pipeline, flag, scfind, threshold, linker,
reliability, dilation and parameter sections; the input and output sections
and the keys that name a file are refused, so a parameter can never make the
node read a path of the caller's choosing. For `photutils` they are `nsigma`
(default 3.0), `npixels` (default 5) and `connectivity` (4 or 8, default 8).

Returns `{valid, error, task_id, status: "running", cache_hit}`. Refusals
before the task starts: 404 unknown finder, 503 finder not available on this
node, 422 wrong dataset kind. Poll with `GET /v1/tasks/{task_id}`; on
completion `result` holds `valid`, `error`, `dataset_id`, `finder`,
`n_sources`, `sources` (catalogue rows), `mask_path` and `catalog_path` (under
`<workspace exports>/sourcefind/<task_id>/`), plus `log_path` for SoFiA-2 and
`threshold` and `noise` for photutils. A run is limited to
`VISIVO_FINDER_TIMEOUT_S` (default 3600 s); cancelling the task stops the
finder process and removes its output directory.

The older synchronous route `POST /v1/sourcefind/{dataset_id}` is described
under [Spectral](spectral.md#post-v1sourcefinddataset_id).
