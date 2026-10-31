# Backend API Reference

Reference for the FastAPI backend (`backend/app/main.py`, routers in
`backend/app/routers/`). This page covers authentication and the conventions
shared by every route; each area of the API has its own page, listed
[below](#areas).

---

## Auth

```
X-Visivo-Token: <token>          # almost every request
X-Visivo-Session: <session_id>  # dataset-scoped requests
```

Most routes require the `X-Visivo-Token` header; a missing or wrong token is
answered 401. The exceptions:

| Route | Access |
|---|---|
| `GET /v1/ping` | Always open |
| `GET /v1/node/status`, `GET /v1/metrics` | Open by default; require the token when `VISIVO_NODE_STATUS_PUBLIC=0` |
| `GET /soda/capabilities`, `GET /soda/availability` | Open |
| `GET /v1/samp/files/{token}` | Open (the file token is the credential) |
| `WebSocket /v1/render/{session_id}/ws` | Token in the `token` query parameter or the header; a bad token closes the socket with code 1008 |

A few server-wide routes (`/v1/sessions`, `/v1/diagnostics/events`, the
listing, download and deletion of Workspace Exports, and SAMP) need the
**operator** token once several per-user tokens are configured: the primary
token, `VISIVO_TOKEN`. With a single token everyone is the operator.

The token is taken from `VISIVO_TOKEN`; when it is unset the backend generates
one at startup and writes it to `~/.visivo_token`, where
`BackendClient::readTokenFile()` reads it. `VISIVO_TOKENS` adds per-user
tokens (comma- or space-separated).

`X-Visivo-Session` names the backend session that holds the caller's open
datasets. Get one with `POST /v1/sessions/new` (or take the `session_id`
returned by an open call) and send it on every dataset-scoped request. With a
single token an absent header falls back to a shared anonymous session; with
several tokens the header is mandatory (400 without it) and a session is
visible only to the token that created it (404 for any other).

---

## Error convention
Most endpoints return `{ "valid": false, "error": "<message>" }` on failure.
HTTP status is then typically 200 even for logical errors, and the client checks
`valid`. Many routes also use HTTP status codes for refusals (404 unknown
dataset or session, 422 unusable input, 429 no free compute slot, 503 missing
optional component, 504 heavy-task timeout); each page says which. A path
outside the allowed data roots is answered as `404 Path not found.`, the same as
a missing one. An unexpected exception is a 500 with
`{error, error_code: "internal_error", request_id}`, the id also in the
`X-Request-ID` header.

---

## Worker pool and heavy-task throttle

CPU-bound work runs in a single process pool of `VISIVO_WORKERS` (default 4)
workers. To keep the GUI responsive while a long compute is running, endpoints
are classified into two tiers:

| Tier | Helper | Examples |
|------|--------|----------|
| **Interactive** | `_run` / `_run_with_limit` | `/v1/cube/preview`, `/slice`, `/subvolume`, `/noise`, `/image/*` |
| **Heavy** | `_run_heavy` / `_run_heavy_with_limit` | `/v1/products/moment`, `/products/isosurface`, `/v1/cube/pv`, `/v1/spectral/*` |

Heavy invocations are gated by a global `asyncio.Semaphore` (`_HEAVY_SEM`)
with capacity `VISIVO_HEAVY_SLOTS` (default `max(1, VISIVO_WORKERS-1)`),
so the pool is never fully saturated by long jobs and an interactive
request (slice scroll, ROI subvolume, probe) always finds a free worker.

---

## Configuration

The variables that matter when calling the API:

| Variable | Default | Effect |
|----------|---------|--------|
| `VISIVO_TOKEN` | generated at startup | The primary (operator) token |
| `VISIVO_TOKENS` | *(unset)* | Additional per-user tokens, comma- or space-separated; enables multi-user mode |
| `VISIVO_NODE_STATUS_PUBLIC` | `1` | `0` makes `/v1/node/status` and `/v1/metrics` require the token |
| `VISIVO_WORKERS` | 4 | Size of the worker pool |

Every other variable (heavy slots and timeouts, moment streaming and Dask,
rendering, remote-service allowlists, limits) is documented in the
[Configuration reference](configuration.md).

---

## Areas

| Area | Routes | Page |
|---|---|---|
| Meta, sessions, files, diagnostics | `/v1/health`, `/v1/sessions*`, `/v1/files/*`, `/v1/diagnostics/*` | [Meta, sessions, files and diagnostics](backend-api/meta-files.md) |
| <span id="catalogue"></span>Datasets, catalogues, VBT | `/v1/datasets/*`, `/v1/catalogue/*`, `/v1/vbt/*` | [Datasets, catalogues and VBT](backend-api/datasets.md) |
| <span id="cube"></span>Cube | `/v1/cube/*`, `/v1/moment/result/binary`, `/v1/pv/result/binary` | [Cube](backend-api/cube.md) |
| <span id="products"></span>Products | `/v1/products/*` | [Products](backend-api/products.md) |
| Tasks and finders | `/v1/tasks/*`, `/v1/finders` | [Tasks and source finders](backend-api/tasks-finders.md) |
| Image, astrometry, photometry | `/v1/image/*`, `/v1/astrometry/*`, `/v1/photometry/*` | [Image, astrometry and photometry](backend-api/image.md) |
| Regions | `/v1/regions/*` | [Regions](backend-api/regions.md) |
| <span id="spectral"></span>Spectral | `/v1/spectral/*`, `/v1/sourcefind/{dataset_id}` | [Spectral](backend-api/spectral.md) |
| Polarisation | `/v1/stokes/*`, `/v1/polarisation/*` | [Polarisation](backend-api/polarisation.md) |
| Velocity fields | `/v1/velocity/*` | [Velocity fields](backend-api/velocity.md) |
| Dynamic spectra | `/v1/dynspec/*` | [Dynamic spectra](backend-api/dynspec.md) |
| Cosmology and analysis | `/v1/cosmology/*`, `/v1/analysis/*` | [Cosmology and analysis](backend-api/cosmology-analysis.md) |
| <span id="hips"></span><span id="vlkb"></span>HiPS, cone search, VLKB | `/v1/hips/*`, `/v1/resolve/*`, `/v1/vlkb/*` | [HiPS, cone search and VLKB](backend-api/sky-services.md) |
| SED and simulations | `/v1/sed/*`, `/v1/simulations/*` | [SED fitting and simulations](backend-api/sed-simulations.md) |
| Exports | `/v1/exports/*` | [Exports](backend-api/exports.md) |
| <span id="samp"></span>SAMP | `/v1/samp/*` | [SAMP](backend-api/samp.md) |
| Server-side rendering | `/v1/render/*` | [Server-side rendering](backend-api/render.md) |
| SRC node and SODA | `/v1/ping`, `/v1/node/status`, `/v1/metrics`, `/soda/*` | [SRC node and SODA](backend-api/node-soda.md) |
| Assistant | `/v1/assistant/*` | [Assistant](backend-api/assistant.md) |
| Reproducibility | `/v1/reproducibility/*` | [Reproducibility](backend-api/reproducibility.md) |

```{toctree}
:maxdepth: 1
:hidden:

backend-api/meta-files
backend-api/datasets
backend-api/cube
backend-api/products
backend-api/tasks-finders
backend-api/image
backend-api/regions
backend-api/spectral
backend-api/polarisation
backend-api/velocity
backend-api/dynspec
backend-api/cosmology-analysis
backend-api/sky-services
backend-api/sed-simulations
backend-api/exports
backend-api/samp
backend-api/render
backend-api/node-soda
backend-api/assistant
backend-api/reproducibility
```
