# Configuration reference

The VisIVO Visual Analytics backend is configured through environment
variables, read when the backend starts (a few are re-read on every request;
those are marked). Set them in the shell that launches `uvicorn`, in the
container's environment, or in the `environment:` block of a compose file.

Every default below is the value in the backend code. Sizes are in bytes and
times in seconds unless stated otherwise; `1 GiB` is 1073741824 bytes.
Boolean switches name the values they accept, because they are not all parsed
the same way.

Variables marked **secret** hold credentials. Keep them out of shell history,
scripts under version control and log files; prefer an env file readable only
by the service account.

The variables used by the cluster launch scripts (`VISIVO_PARTITION`,
`VISIVO_TIME`, `VISIVO_SIF`, `VISIVO_GRES` and the others read by
`scripts/launch_backend_slurm.sh`) configure the job, not the backend, and are
documented in [Running the backend on a cluster](deployment/hpc-slurm.md).

## Worker pool and admission

CPU-bound work runs in one process pool. Heavy tasks (moments, isosurfaces,
PV slices, spectral fits) are throttled so that at least one worker stays free
for interactive requests such as slice scrolling and probing; see
[Async patterns](async-patterns.md) for the design.

| Variable | Default | Effect |
|---|---|---|
| `VISIVO_WORKERS` | `4` | Number of worker processes in the pool. The default is a fixed 4, not derived from the CPU count. |
| `VISIVO_HEAVY_SLOTS` | `max(1, VISIVO_WORKERS - 1)` | Heavy tasks allowed to occupy the pool at once. Set it to `VISIVO_WORKERS` to give up the reserved interactive slot, or to `1` to serialise heavy work. |
| `VISIVO_HEAVY_TIMEOUT_S` | `900` | Wall-clock ceiling for one heavy task; beyond it the request answers HTTP 504 and its slot is released. `0` disables the ceiling. The worker process itself may still run to completion. |
| `VISIVO_MAX_CONCURRENT_TASKS` | `3` | Compute tasks one session may run at once; further requests wait (see below) and then answer HTTP 429. |
| `VISIVO_TASK_SLOT_WAIT_S` | `15` | How long a request waits for a free per-session slot before answering 429. `0` refuses immediately. |
| `VISIVO_MP_START_METHOD` | `spawn` | Start method of the worker processes (`spawn`, `fork` or `forkserver`). `spawn` is required when workers use CUDA. An unknown value falls back to the platform default, with a warning. |
| `VISIVO_GPU_ROUND_ROBIN` | `1` | With `1`, each worker is pinned to one GPU in turn (through `CUDA_VISIBLE_DEVICES`), taken from the inherited `CUDA_VISIBLE_DEVICES` or, failing that, from `nvidia-smi -L`. Any other value turns pinning off. |
| `VISIVO_PRODUCT_CACHE_ENTRIES` | `32` | Capacity of the in-memory LRU cache of computed products (moment, isosurface, PV, line width). Identical recomputations are served from it. |

## Memory and full-resolution loading

The backend measures the memory it can use from the tightest of: the cgroup
memory limit at each level of its own cgroup (cgroup v2 `memory.max`, else
cgroup v1 `memory.limit_in_bytes`), the memory SLURM granted the job, and,
when none of these exists, the host's physical memory. The *available* memory
is the headroom under those limits, or the host's available memory.

| Variable | Default | Effect |
|---|---|---|
| `VISIVO_FULL_RES_THRESHOLD_BYTES` | `min(4 GiB, memory limit / 4)` | File size at or above which the backend tells the client to open a cube as a preview only (`recommended_load_mode = preview_only`). The user can still load full resolution or crop explicitly. When set, the value is used as is, with no memory check. |
| `VISIVO_CGROUP_ROOT` | the process's own cgroup | cgroup v2 directory to read the memory and CPU limits from. When set, cgroup v1 is not consulted. Useful when `/proc/self/cgroup` does not name the right group. |

## Moments and Dask

How a moment map is computed depends on the size of the selected channel
range, `channels × NAXIS1 × NAXIS2 × 4` bytes (float32):

1. With `VISIVO_DASK_MODE=auto` (the default) and Dask installed, the moment
   is computed with chunked `dask.array`: always on the distributed cluster
   when `VISIVO_DASK_SCHEDULER` names a reachable one, otherwise with a local
   threaded scheduler once the range reaches `VISIVO_DASK_MIN_BYTES`. Blocks
   are read from disk on demand; each holds at most 64 channels and at most
   `VISIVO_MOMENT_CHUNK_BYTES`, and at least one channel.
2. Otherwise the worker keeps the range in memory only when it is no larger
   than `min(VISIVO_MOMENT_STREAM_BYTES, available memory / 12)`. The factor 12
   covers the float64 temporaries of the reduction.
3. A larger range is streamed from disk: M0, M1 and M2 in spectral slabs,
   M3 to M10 through the same chunked Dask path (if Dask is installed; without
   it they fall back to memory). Slabs are sized like the Dask blocks, by
   `VISIVO_MOMENT_CHUNK_BYTES`, with `VISIVO_MOMENT_SLAB_CHANNELS` as the cap
   on their channel count.

Moments that need the centroid first (M2 to M5) are computed in two passes
over the data on both out-of-core paths, so memory stays bounded by one block.

| Variable | Default | Effect |
|---|---|---|
| `VISIVO_MOMENT_STREAM_BYTES` | `2 GiB` | Ceiling of the channel range kept in memory; the effective threshold is the smaller of this and one twelfth of the available memory. |
| `VISIVO_MOMENT_CHUNK_BYTES` | `64 MiB` | Size of each streamed slab or Dask block, never less than one channel. |
| `VISIVO_MOMENT_SLAB_CHANNELS` | `64` | Upper limit on the channels in a streamed slab. It only caps the slab; the size comes from `VISIVO_MOMENT_CHUNK_BYTES`. |
| `VISIVO_MOMENT_THREADS` | `0` | Threads for the parallel moment kernels and the local Dask scheduler. `0` means `min(4, CPU count)`. |
| `VISIVO_DASK_MODE` | `auto` | `auto`: distributed if a scheduler is set, else local threaded Dask for large ranges. `off`: never Dask. `local`: always local threaded Dask. `distributed`: only a cluster, else the plain worker. |
| `VISIVO_DASK_MIN_BYTES` | `512 MiB` | In `auto` mode, smallest range sent to the local threaded Dask path. |
| `VISIVO_DASK_SCHEDULER` | unset | Address of a `dask.distributed` scheduler (for example `tcp://host:8786`). When set, moments are computed on that cluster; its workers must see the same files. |

## Rendering

Remote 3-D views ([Cube viewer](user-guide/cube-viewer.md)) are rendered by the
backend, each in a process of its own.

| Variable | Default | Effect |
|---|---|---|
| `VISIVO_RENDER_MAX_SESSIONS` | `8` | Remote views accepted at once; beyond it a new view answers HTTP 503. |
| `VISIVO_RENDER_IDLE_TTL_SECONDS` | `300` | A view that receives no command for this long is closed. |
| `VISIVO_RENDER_IN_PROCESS` | unset | `1` renders inside the backend's own process instead of a child process (slower: rendering then competes with the process that serves the clients). |

## SRC node

Settings for a backend that runs as a node of the SKA Regional Centre network
and is discovered by a SKAVA catalogue; see
[The backend as an SRC node](deployment/src-node.md) and
[SKAVA discovery](user-guide/skava-discovery.md).

| Variable | Default | Effect |
|---|---|---|
| `VISIVO_NODE_CODE` | unset | The node's code in the catalogue, reported in `/v1/node/status` and in pushed telemetry. |
| `VISIVO_NODE_GPU` | unset | `1`/`true`/`yes` declares a GPU, `0`/`false`/`no` declares none; unset reports "unknown". |
| `VISIVO_NODE_STATUS_PUBLIC` | `1` | `0`/`false`/`no` makes `/v1/node/status` and the metrics require the backend token. `/v1/ping` always stays open. Re-read on every request. |
| `VISIVO_VERSION` | the application version | Version string reported in the node status. |
| `VISIVO_DATA_ROOT` | unset | A single directory that `file://` access URLs from SKAVA and the SODA service may point into. Combined with `VISIVO_DATA_ROOTS`; either is enough. |
| `VISIVO_SKAVA_TELEMETRY_URL` | unset | Base URL (`http` or `https`) of the catalogue to push the node status to. Pushing is off unless this, the token and the node code are all set. |
| `VISIVO_SKAVA_NODE_TOKEN` | unset | **Secret.** This node's telemetry token, issued by the catalogue's operator. |
| `VISIVO_SKAVA_TELEMETRY_INTERVAL` | `10` | Seconds between pushed reports; at least 1. |

## Security, tokens and multi-user

See [Sharing one backend between users](multi-user-backend.md) for how these
fit together.

| Variable | Default | Effect |
|---|---|---|
| `VISIVO_TOKEN` | generated at startup | **Secret.** The primary (operator) token. When unset, a random token is generated, printed at startup and written to `~/.visivo_token` (mode 600). Re-read on every request, so it can be rotated. |
| `VISIVO_TOKENS` | unset | **Secret.** Additional per-user tokens, comma- or whitespace-separated. More than one accepted token switches on multi-user mode. Re-read on every request. |
| `VISIVO_DATA_ROOTS` | unset | Directories (separated by `:` on Linux and macOS, `;` on Windows) that path-based endpoints may read. Unset: unconfined with a single token, all path access denied in multi-user mode. Re-read on every request. |
| `VISIVO_STRICT_SESSIONS` | on in multi-user mode, off otherwise | When on, only session ids minted by the backend are accepted. `0`/`false`/`no` turns it off, any other non-empty value on. |
| `VISIVO_SESSION_TTL` | `1800` | A session idle for longer than this many seconds is removed and its datasets closed, unless a task of it is still running. The shared anonymous session stays, but its datasets are released. |
| `VISIVO_MAX_SESSIONS_PER_TOKEN` | `32` | Live sessions one token may hold; a further session answers HTTP 429. |
| `VISIVO_TASK_TTL_SECONDS` | `1800` | A task record neither read nor updated for this many seconds is dropped from the task registry (at least 1), after which its status can no longer be polled. Running tasks are never dropped. |
| `VISIVO_SNAPSHOT_SECRET` | `ADMIN_SESSION_SECRET`, else random per process | **Secret.** HMAC key that signs reproducibility snapshots. Set it so signatures stay valid across restarts. |

## Files and caches

| Variable | Default | Effect |
|---|---|---|
| `VISIVO_EXPORTS_DIR` | `~/.visivo/exports` | The Workspace exports folder: saved cut-outs, moment FITS files and source-finding products. Created if missing. |
| `VISIVO_HIPS_CACHE` | `~/.cache/visivo/hips` | Directory of the HiPS tile cache. |
| `VISIVO_HIPS_CACHE_MAX_BYTES` | `2 GiB` | Size cap of the HiPS cache; the least recently used tiles are evicted beyond it. |
| `VISIVO_HIPS_CACHE_TTL_SECONDS` | `604800` (7 days) | Tiles older than this are deleted. |
| `VISIVO_HIPS_CACHE_CLEANUP_INTERVAL` | `60` | Minimum seconds between two cleanup passes over the HiPS cache. |
| `VISIVO_SAMP_FILE_TTL` | `3600` | Files staged for SAMP (in the system temporary directory, `visivo_samp_files`) are deleted after this many seconds. |

## Copilot

The copilot's provider is chosen per session in the client; these variables
give every new session its starting configuration. See
[Self-hosted copilot LLM](user-guide/copilot-self-hosted.md) and
[Copilot backend](copilot-backend.md).

| Variable | Default | Effect |
|---|---|---|
| `VISIVO_ASSISTANT_PROVIDER` | `auto` | `auto`, `anthropic`, `openai` or `null`. `auto` picks a self-hosted endpoint or OpenAI key first, then an Anthropic key, else `null` (tools only, no language model). |
| `VISIVO_ASSISTANT_MODEL` | provider default | Model name sent to the provider. Without it the Anthropic provider uses `claude-opus-4-8` and the OpenAI provider, cloud or self-hosted, uses `gpt-4o`; a self-hosted server usually needs its own served name here. |
| `VISIVO_LLM_BASE_URL` | unset | Base URL of an OpenAI-compatible server (for example `http://host:8000/v1`). Keep credentials out of it; the key belongs in `VISIVO_LLM_API_KEY`. A URL set from the client must be `http`/`https` with no credentials, query or fragment. |
| `VISIVO_LLM_API_KEY` | unset | **Secret.** Key sent to `VISIVO_LLM_BASE_URL`, and only there. |
| `ANTHROPIC_API_KEY` | unset | **Secret.** Key for the Anthropic provider. |
| `OPENAI_API_KEY` | unset | **Secret.** Key for the cloud OpenAI provider; never sent to a self-hosted URL. |
| `VISIVO_ASSISTANT_SLOTS` | `2` | Copilot queries running at once, across all sessions (at least 1). Separate from the heavy-task slots. |
| `VISIVO_ASSISTANT_TIMEOUT_S` | `120` | Wall-clock budget of one copilot query, tool calls included; beyond it the request answers HTTP 504. |

## Remote services: VLKB, HiPS, cone search, open URL, SAMP

Every URL the backend fetches on a client's behalf is checked against an
allowlist variable, separately for each service. **When an allowlist is set,
only the hosts it names (comma-separated, matched case-insensitively) are
fetched and every other host is refused.** When it is unset, only `http` and
`https` URLs are accepted and, except for the open-URL route, addresses that
resolve to private, loopback, link-local, reserved or multicast networks are
refused. Redirects are checked with the same policy.

| Variable | Default | Effect |
|---|---|---|
| `VISIVO_VLKB_URL` | `https://vlkb.ia2.inaf.it/vlkb` for TAP queries; none for multi-cutout | Default VLKB service when the client sends none. |
| `VISIVO_VLKB_ALLOWED_HOSTS` | unset | Allowlist for VLKB TAP, cut-out, multi-cutout and SED-fit requests. |
| `VISIVO_VLKB_MAX_BYTES` | `8 GiB` | Largest single cut-out download. |
| `VISIVO_VLKB_MCUTOUT_TIMEOUT` | `300` | Socket timeout of each multi-cutout archive call. |
| `VISIVO_VLKB_MCUTOUT_MAX_ITEMS` | `500` | Datasets in one multi-cutout batch. |
| `VISIVO_VLKB_MCUTOUT_MAX_BYTES` | `16 GiB` | Largest multi-cutout results archive. |
| `VISIVO_VLKB_MCUTOUT_MAX_EXTRACTED` | `32 GiB` | Total unpacked size of that archive. |
| `VISIVO_VLKB_MCUTOUT_MAX_MEMBERS` | `2000` | Entries in that archive. |
| `VISIVO_VLKB_MCUTOUT_MAX_REPORT` | `8 MiB` | Largest job report. |
| `VISIVO_VLKB_SEDFIT_URL` | unset | Base URL of the VLKB SED-fit model service. |
| `VISIVO_SED_MODELS_TIMEOUT` | `300` | Deadline of a whole SED model-fit request. |
| `VISIVO_SED_MODELS_MAX_BYTES` | `64 MiB` | Largest response accepted from the SED-fit service. |
| `VISIVO_SED_MAX_MODELS` | `2000` | Upper bound on the `max_models` a client may request. |
| `VISIVO_HIPS2FITS_URL` | `https://alasky.cds.unistra.fr/hips-image-services/hips2fits` | hips2fits service, for a mirror. |
| `VISIVO_HIPS2FITS_ALLOWED_HOSTS` | unset | Allowlist for hips2fits requests. |
| `VISIVO_CONE_SEARCH_ALLOWED_HOSTS` | unset | Allowlist for cone-search requests. |
| `VISIVO_OPENURL_ALLOWED_HOSTS` | unset | Allowlist for `/v1/datasets/open_url` (opening a dataset by URL, as SKAVA access URLs do). |
| `VISIVO_OPENURL_BLOCK_PRIVATE` | off | `1`/`true`/`yes` refuses private and loopback addresses on the open-URL route too. Off by default because federated data nodes often sit on private networks. |
| `VISIVO_SAMP_DIRECT` | `0` | `1` lets the backend register with a SAMP hub on its own machine and send messages directly. Without it, SAMP goes through the desktop client, which is the only way when the backend is remote. |

## Analysis limits

| Variable | Default | Effect |
|---|---|---|
| `VISIVO_LINEWIDTH_SNR` | `3.0` | Pixels below this signal-to-noise ratio are skipped by the line-width fit; `0` fits every pixel. |
| `VISIVO_LINEWIDTH_CHUNKS` | `VISIVO_WORKERS` | Row chunks a line-width map is split into; the chunks still pass the heavy-task throttle. |
| `VISIVO_KINEMATIC_MAX_VOXELS` | `268435456` (256 Mi voxels) | Largest kinematic model cube; larger requests answer HTTP 413. Zero or invalid values use the default. |
| `VISIVO_AREPO_MAX_CELLS` | `600000000` | Largest grid an AREPO extraction may materialise. |
| `VISIVO_AREPO_MAX_LEVEL` | `14` | Highest AREPO refinement level accepted. |
| `VISIVO_SOFIA_BIN` | `sofia2`, else `sofia`, on `PATH` | The SoFiA-2 executable. |
| `VISIVO_SOFIA_TIMEOUT_S` | `900` | Wall-clock limit of a SoFiA-2 run started from `POST /v1/sourcefind/{dataset_id}`; `0` disables it. |
| `VISIVO_FINDER_TIMEOUT_S` | `3600` | Wall-clock limit of a source finder run as a task (`POST /v1/tasks/sourcefind`); at least 1. |

## Velocity fields

| Variable | Default | Effect |
|---|---|---|
| `VISIVO_VELOCITY_STREAM_BYTES` | `512 MiB` | Above this field size, the full-resolution divergence is computed in z-slabs read from disk instead of in memory. `0` always streams. |
| `VISIVO_VELOCITY_SLAB` | `32` | z-planes per slab on the streamed path (at least 1). |

## Diagnostics

| Variable | Default | Effect |
|---|---|---|
| `VISIVO_DIAGNOSTICS_LEVEL` | `INFO` | Lowest Python logging level copied into the diagnostics buffer that *View → Diagnostics* shows (`DEBUG`, `INFO`, `WARNING`, `ERROR`). An unknown name means `INFO`. |
| `VISIVO_DIAGNOSTICS_BUFFER` | `2000` | Log records kept in that buffer (at least 16); older ones are dropped. |

## Other variables the backend honours

| Variable | Effect |
|---|---|
| `CUDA_VISIBLE_DEVICES` | The GPUs the job was given. Workers are spread over them (see `VISIVO_GPU_ROUND_ROBIN`), and so are remote render views. |
| `VTK_EGL_DEVICE_INDEX` | Binds every remote render view to this EGL device instead of spreading views over the job's GPUs. |
| `SLURM_JOB_ID` | Marks the backend as running in a SLURM job, so the memory grant below is taken into account. |
| `SLURM_MEM_PER_NODE` | Memory granted to the job, in MiB; counts as a memory limit. `0` (the whole node) sets none. |
| `SLURM_MEM_PER_CPU`, `SLURM_CPUS_ON_NODE` | Used together as the memory grant when `SLURM_MEM_PER_NODE` is absent. |
| `ADMIN_SESSION_SECRET` | **Secret.** Fallback for `VISIVO_SNAPSHOT_SECRET`. |
| `ANTHROPIC_API_KEY`, `OPENAI_API_KEY` | See [Copilot](#copilot). |
