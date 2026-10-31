# Large cube loading

How the backend and the desktop handle a cube too large to load whole into
the client: the backend states the cube's size and its own threshold, the
desktop stops at a decimated preview above it, and full resolution is fetched
only on request, as the whole cube or as a cropped region. Server-side
rendering (the 3-D view rendered on the backend's GPU, see
[Cube viewer](user-guide/cube-viewer.md#where-the-rendering-happens-auto-local-remote))
is the other way to full resolution, where the backend has EGL; on a backend
without it, this path is the only one.

## What the backend says at open

Every dataset-open response (`POST /v1/datasets/open`, `/v1/datasets/open_url`,
`/v1/datasets/open_skava`, schema `OpenDatasetResponse` in
`backend/app/schemas.py`) carries three fields:

```json
{
  "file_size_bytes": 11027525760,
  "full_res_threshold_bytes": 4294967296,
  "recommended_load_mode": "preview_only"
}
```

`recommended_load_mode` is `"preview_only"` when `file_size_bytes` is at or
above `full_res_threshold_bytes`, otherwise `"auto_full"`. A file size that
cannot be read counts as 0, so the mode falls back to `"auto_full"`.

The threshold (`_full_res_threshold_bytes()` in `backend/app/fits_dataset.py`)
is `VISIVO_FULL_RES_THRESHOLD_BYTES` when that is set. Otherwise it is the
smaller of:

* **4 GiB**, the size above which a 16 GB laptop starts thrashing on VTK volume
  rendering of an HI cube;
* **a quarter of the memory the backend can have**
  (`memory_limit_bytes()` in `backend/app/node_status.py`): the tightest
  cgroup limit (v2, else v1) or SLURM memory grant, else the host's memory. A
  full-resolution load sends the cube in several concurrent slabs, each read
  and then copied into the reply, so about twice the cube is in the backend's
  memory at once: a node with 8 GB gets a 2 GiB threshold instead of being
  killed serving a 3 GB cube. See
  [Node capacity under SLURM](deployment/hpc-slurm.md#node-capacity-under-slurm).

The threshold is a backend setting: it applies to every client of that
backend. On a SLURM node, export
`APPTAINERENV_VISIVO_FULL_RES_THRESHOLD_BYTES=<bytes>` in the shell the
launcher runs in (see
[Other backend settings](deployment/hpc-slurm.md#other-backend-settings));
on a backend outside a container, set `VISIVO_FULL_RES_THRESHOLD_BYTES`.

## What the desktop does with it

`BackendOpenDatasetResult` (`src/app/BackendClient.h`) reads the three fields;
a backend that does not send them leaves `recommendedLoadMode` at
`"auto_full"`, the earlier behaviour.

After the preview has landed, `vtkWindowCube` fetches the full cube by itself
only when the backend recommends `auto_full` **and** the cube is within the
client's own local-load threshold, the setting `Cube/local_full_threshold_mb`
(default 256 MB, clamped to 16 to 16384). Otherwise it stays on the preview
and the status bar says so:

```text
Preview-only mode: cube is 10.27 GB (threshold 4.0 GB). Use View → Load Full Resolution or Crop Region for full-res data.
```

The same two conditions raise the banner that recommends remote rendering or
a full-resolution load (described for users in the
[Cube viewer](user-guide/cube-viewer.md#where-the-rendering-happens-auto-local-remote)).

The **View** menu has two actions (`src/gui/vtkWindowCube.cpp`):

* **Load Full Resolution** fetches the whole cube. Above the local-load
  threshold it first warns about the memory and transfer cost and offers
  **Use Remote Rendering** when the backend can render server-side; the user
  can still load it anyway. A load is refused outright, with the preview left
  on screen, when three times the region's size (the slabs as they arrive,
  the assembled volume and its copy on the GPU) exceeds the desktop machine's
  physical memory.
* **Crop Full Resolution…** opens the sub-cube dialog: the user picks the X,
  Y and channel ranges, and the backend writes that region as a FITS file in
  the workspace exports (`POST /v1/cube/save_subregion`), with its WCS, and
  registers it in the session as a new dataset that can be opened like any
  other.

## The binary sub-volume route

A full-resolution fetch goes through `POST /v1/cube/subvolume_bin`
(`backend/app/routers/cube.py`), which returns the voxels as raw
little-endian float32 (`application/octet-stream`) with the geometry in
`X-Visivo-*` headers (`Width`, `Height`, `Depth`, `Scalar-Type`, `Range-Min`,
`Range-Max`, `Spectral-Axis-Type`, `Spectral-Axis-Unit`, `Bunit`,
`Byte-Order`). It avoids the base64 and zlib encoding of the JSON route
`/v1/cube/subvolume`, which dominated the time on multi-GB payloads.

The desktop (`BackendClient`, in `src/app/BackendClient.cpp`) splits the
channel range into 4 slabs fetched concurrently, each its own TCP stream over
the same tunnel, and stitches them back. `VISIVO_SUBVOLUME_PARALLEL` in the
**desktop's** environment sets the number of slabs (1 to 16). It falls back to
the JSON route only when the binary route answers `404` (an older backend); a
slab that fails or times out fails the load, rather than sending the whole
cube again as JSON.

The backend route does not count against the per-session task-slot limit
(`VISIVO_MAX_CONCURRENT_TASKS`, default 3), which answers `429` when it
overflows: the slabs are pieces of one request and must run at the same time.

## Scrubbing slices meanwhile

`POST /v1/cube/slice` runs through `_run_interactive`
(`backend/app/dependencies.py`), which always uses the backend's local process
pool: it never goes to Dask, whose per-task overhead and queueing behind bulk
work would turn a slice fetch of a fraction of a second into seconds, and it
does not take a per-session task slot.

On the desktop, remote slice changes are debounced by 300 ms
(`remoteSliceDebounceDelayMs`), and the neighbouring slices are prefetched
only 500 ms after the last change (`idlePrefetchDelayMs`), so a fast scroll
sends no prefetches and the link stays free for the slice being looked at.
