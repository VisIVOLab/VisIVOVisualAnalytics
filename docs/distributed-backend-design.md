# Distributed VisIVO Backend — Architecture & Design

> Status: design baseline, draft 1
> Author: VisIVO integration, June 2026
> Related: SKAVA D3.1 (§3.4 Data Locality), SKAVA D3.1 (§13.3 Data Staging)

## 1. Executive Summary

VisIVO today opens FITS / HDF5 datasets by talking to a single FastAPI
backend, typically hosted on the same machine as the desktop client
(`localhost:8001`). For large SKA-class datasets that pattern stops
scaling: a 50 GB HI cube hosted at an SKA Regional Centre (SRC)
should not be copied across the network to every analyst's laptop.

This document proposes the **distributed VisIVO backend** model: each
SRC (or data centre) hosts its own VisIVO backend co-located with the
data, and the desktop client transparently routes its compute requests
to the backend that owns each dataset. Discovery and routing are
SKAVA-driven; the desktop holds a registry of known backends and
picks the right one per-dataset based on metadata returned by
SKAVA's DataLink layer.

The result is the "compute next to data" pattern: raw data stays put,
only previews, slices, derived products and 1-D summaries travel to
the desktop. This is the same architectural principle SKA-SRC adopts
for its analysis services.

The proposal is **fully incremental** on top of the existing
single-backend model — the local backend remains the default, the
registry is opt-in, and unconfigured SKAVA peers transparently fall
back to today's "download to local backend" behaviour.

## 2. Background & motivation

### 2.1 The current single-backend model

```
┌────────────────────┐    ┌────────────────────┐
│  Desktop (Qt)      │───▶│  VisIVO backend    │
│  ─ open by path    │    │  (FastAPI)         │
│  ─ open by SKAVA   │    │  ─ FITS reader     │
│  ─ region stats    │    │  ─ moment workers  │
│  ─ moments         │    │  ─ exports cache   │
│  ─ etc.            │    └────────────────────┘
└────────────────────┘             │
                              filesystem
                              (local disk
                              or NFS mount)
```

`Settings::getBackendUrl()` returns a single URL. Every viewer window
constructs a `BackendClient(url, token)` and that's the only backend
in play. For SKAVA opens the flow today is:

1. Desktop queries SKAVA discovery
2. Desktop hands SKAVA's `access_url` to the local VisIVO backend
3. Backend **downloads** the remote file into
   `<workspace_exports>/skava_cache/`
4. Standard local-path open pipeline kicks in

### 2.2 Why this doesn't scale

- **Multi-GB datasets**: WALLABY HI cubes are ~10 GB, MeerKAT
  Galactic-Centre mosaics are ~800 MB, SKA SDC2 simulated cubes
  ~6 GB. Downloading each before opening is minutes-to-hours of
  one-way wait, and worse for users on metered home connections.
- **Embargo / proprietary data**: SKA proprietary period datasets
  legally cannot leave the SRC. Local download is **forbidden**, not
  just impractical.
- **Storage at the desktop**: A typical analyst working across 10
  datasets quickly fills the laptop's disk with cached copies of
  data that is already pristine at the SRC.
- **Compute resources**: Moment maps, source-finding and
  dedispersion benefit from the SRC's compute fleet. Running them on
  a laptop wastes both time and the SRC's existing capacity.

### 2.3 SKAVA's design intent

SKAVA D3.1 explicitly anticipates this evolution:

- §3.4 *Data Locality and Replica Awareness* — "the design should
  prefer access endpoints closest to compute resources".
- §13.3 *Data Staging and Prefetching* — "future SODA/staging
  services will materialise sub-products near compute".

The replica list returned by `/discovery/search` already includes a
`node` with `base_url`, `latency_score`, `load_score`,
`capability_score`. The DataLink response includes a
`service_descriptors[]` array explicitly intended for "future
services" at each node. Today this is unused; we propose to wire it
to a VisIVO backend at each node.

## 3. Goals & non-goals

### 3.1 Goals

1. Desktop client can talk to **N backends concurrently**, each
   identified by `(id, name, url, token)`.
2. SKAVA tells the desktop, per dataset, which VisIVO backend owns
   the data — no hard-coded "SRC X = backend URL Y" tables in the
   client.
3. **Backwards-compatible**: a SKAVA peer that does not declare any
   visivo-backend descriptor causes the desktop to fall back to the
   current behaviour (download via local backend).
4. **Transparent to viewers**: `vtkWindowImage`, `vtkWindowCube`,
   etc. continue to receive a backend URL through their constructor
   — they don't need to know which backend it is.
5. **Single source of truth for sessions**: each backend keeps its
   own session, no cross-backend session merging.

### 3.2 Non-goals

1. **Backend-to-backend orchestration**. Backends do not talk to
   each other in this design. If two datasets live on two different
   SRCs, the desktop opens two viewer windows talking to two
   backends. No cross-backend joins.
2. **Automatic deployment** of VisIVO backends at SRCs. Each SRC's
   operator deploys their own backend instance and registers it in
   SKAVA. The desktop only consumes the routing info.
3. **OAuth / OIDC** federation. The token model stays bearer-token
   per backend; OAuth is a separate work item (R10 in the roadmap).
4. **SODA cutout integration**. Out of scope for this proposal —
   tracked separately, lands when SKAVA exposes real `/soda/sync`.

## 4. Architectural overview

### 4.1 Topology

```
                      Desktop (Qt)
                          │
                          ▼
              ┌───────────────────────┐
              │  Backend Registry     │
              │  (Settings)           │
              │  ─ local              │
              │  ─ src-it-inaf        │
              │  ─ src-de-mpg         │
              │  ─ …                  │
              └───────┬───────────────┘
                      │ HTTPS
       ┌──────────────┼──────────────┐
       │              │              │
       ▼              ▼              ▼
┌────────────┐ ┌────────────┐ ┌────────────┐
│  Local     │ │  SRC-IT    │ │  SRC-DE    │
│  backend   │ │  backend   │ │  backend   │
│            │ │            │ │            │
│  cache/    │ │  /data/    │ │  /data/    │
│  exports/  │ │  …         │ │  …         │
└────────────┘ └────────────┘ └────────────┘

         SKAVA queries this map
                  │
                  ▼
        ┌─────────────────────────┐
        │  SKAVA Discovery        │
        │                         │
        │  for each dataset:      │
        │    obs_id, replicas[],  │
        │    service_descriptors  │
        │    [visivo-backend]     │
        └─────────────────────────┘
```

### 4.2 Routing decision

For a single user action (Open dataset from SKAVA), the routing
decision proceeds as:

1. Desktop calls SKAVA `/datalink/{obs_id}`.
2. SKAVA returns `service_descriptors` including
   `{service_type: "visivo-backend", endpoint, node_code}`.
3. Desktop matches `endpoint` against its known backend registry by
   URL (exact match) or `node_code` (preferred — stable identifier).
4. If match found → open the dataset against **that** backend, no
   download.
5. If no match found AND auto-add is enabled → prompt user to add
   the backend to the registry, then open.
6. If no match found AND auto-add disabled → fall back to current
   behaviour (download to local backend cache, open from there).

### 4.3 Open flow comparison

| Step | Today (single backend) | Distributed |
|---|---|---|
| 1 | Discovery / DataLink | Discovery / DataLink |
| 2 | Local backend `/v1/datasets/open_url` | Pick backend per dataset |
| 3 | Backend downloads file (GB) | Backend opens local file (instant) |
| 4 | Pixel reads on cached local file | Pixel reads on backend's native filesystem |
| 5 | Identical viewer experience | Identical viewer experience |

Step 3 saves the download (the dominant cost for large data).
Step 4 is also faster: SRC storage is typically faster than a
laptop SSD for sequential reads.

## 5. Data model

### 5.1 BackendNode (desktop-side, Settings)

```cpp
struct BackendNode {
    QString id;          // stable id, e.g. "local", "src-it-inaf"
    QString name;        // display name shown in UI
    QString url;         // base URL (no trailing slash)
    QString token;       // bearer auth (empty for unauthenticated)
    QString srcCode;     // optional SKAVA node code for cross-ref
    bool    isLocal;     // marks the default fallback
    bool    isBuiltin;   // local backend is auto-managed
};
```

Persisted in `QSettings` under `Backends/nodes` as a JSON array.
`local` is auto-created with the value of the legacy
`Settings::getBackendUrl()` for migration compatibility.

### 5.2 SKAVA service descriptor extension

The DataLink response gains entries of shape:

```json
{
  "service_descriptors": [
    {
      "service_type": "access-resolution",
      "enabled": true
    },
    {
      "service_type": "visivo-backend",
      "enabled": true,
      "endpoint": "https://srcit.inaf.it/visivo",
      "node_code": "INAF-IT",
      "requires_auth": true,
      "supports_kinds": ["image", "cube", "dynspec"]
    }
  ]
}
```

Server side, SKAVA stores per-node `visivo_backend_url`,
`visivo_backend_requires_auth` on the `nodes` table. The DataLink
builder emits the descriptor when populated; nodes without the
column populated do not emit it (backwards compatibility).

### 5.3 Session state

`BackendClient` already stores `m_sessionId` per instance. In the
distributed model the desktop maintains a `QHash<QString,
QSharedPointer<BackendClient>>` indexed by backend `id`, so each
backend's session id is preserved across the lifetime of the
desktop.

## 6. Component design

### 6.1 Settings (`src/Settings.{h,cpp}`)

New accessors:

```cpp
QList<BackendNode> getBackendNodes() const;
void               setBackendNodes(const QList<BackendNode> &nodes);
BackendNode        getBackendNode(const QString &id) const;
BackendNode        getLocalBackend() const;
QString            getDefaultBackendId() const;
void               setDefaultBackendId(const QString &id);
/// Insert if not present, update if present (matched by id).
void               upsertBackendNode(const BackendNode &node);
void               removeBackendNode(const QString &id);
```

Legacy `getBackendUrl()` / `getBackendToken()` remain as
compatibility-layer methods returning the local backend's URL/token,
so older call sites keep working without change.

### 6.2 Settings dialog (`src/gui/SettingsDialog.{h,cpp}`)

A new tab **Backends** containing:

- A `QListView` of registered backends.
- **Add**, **Edit**, **Remove**, **Set as default** buttons.
- An edit form for each backend (id, display name, URL, token,
  SRC node code).
- The `local` backend is non-removable; it can be reconfigured but
  not deleted.

### 6.3 BackendClient (`src/app/BackendClient.{h,cpp}`)

Already takes `(url, token)` in the constructor — no API change. We
add a desktop-side factory:

```cpp
class BackendClientFactory {
public:
    static QSharedPointer<BackendClient> get(const QString &nodeId);
    static QSharedPointer<BackendClient> getLocal();
    static QSharedPointer<BackendClient> getForUrl(const QString &url);
    static void invalidate(const QString &nodeId);
};
```

The factory caches one `BackendClient` per backend `id` so session
state persists. Cache is cleared on Settings change.

### 6.4 SkavaClient updates

The `SkavaDatalinkResponse` struct gains:

```cpp
struct SkavaVisivoBackendDescriptor {
    QString endpoint;
    QString nodeCode;
    bool    requiresAuth{ false };
    QStringList supportedKinds;
};

struct SkavaDatalinkResponse {
    // … existing fields …
    /// One entry per available VisIVO backend that can serve this
    /// dataset (filtered from service_descriptors[]). May be empty,
    /// in which case the desktop falls back to download.
    QVector<SkavaVisivoBackendDescriptor> visivoBackends;
};
```

### 6.5 MainWindow routing

`doOpenDataFromUrl()` becomes a thin facade over a new
`doOpenSkavaDataset(obsId, datalinkResp)`:

```cpp
void MainWindow::doOpenSkavaDataset(
    const QString &obsId,
    const SkavaDatalinkResponse &datalink)
{
    // 1. Try to match a known backend by SKAVA's visivo-backend descriptor
    BackendNode chosen = pickBackendForDataset(datalink);

    if (!chosen.url.isEmpty() && chosen.id != "local") {
        // Remote-backend open: backend already has the file
        auto client = BackendClientFactory::get(chosen.id);
        auto opened = client->openDataset(/* path from datalink */ ...);
        openViewerFromOpenedDataset(displayPath, opened, *client);
        return;
    }

    // 2. Fall back to download-into-local behaviour
    auto local = BackendClientFactory::getLocal();
    auto opened = local->openDatasetFromUrl(datalink.primaryAccessUrl, obsId);
    openViewerFromOpenedDataset(displayPath, opened, *local);
}
```

`pickBackendForDataset()` matches descriptors against the registry by
(a) `endpoint == registry URL`, (b) `nodeCode == registry srcCode`,
or (c) the user's default backend if no SKAVA hint applies.

### 6.6 SkavaSearchPanel UI

The results table gains a column **Served by** showing the chosen
backend's display name (e.g. "Local", "INAF-IT", "—" when no
visivo-backend descriptor present). The Open button updates its
label dynamically to "Open via INAF-IT" / "Open via Local"
depending on selection.

A status line below the table reports the routing decision:

> Selected: `d22-mgcls-abell194-StokesI` → backend **INAF-IT**
> (compute next to data, no download required)

### 6.7 Viewer window indicator

Each viewer's status bar gains a small badge:

```
[ INAF-IT 🌐 ]   for remote-backend datasets
[ Local 💾 ]    for local-backend datasets
```

Clicking the badge opens the Settings → Backends tab so the user can
inspect/change the backend.

## 7. SKAVA-side extension

### 7.1 Database migration

`migrations/versions/0007_add_visivo_backend.py`:

```python
def upgrade():
    op.add_column("nodes", sa.Column(
        "visivo_backend_url", sa.String(256), nullable=True))
    op.add_column("nodes", sa.Column(
        "visivo_backend_requires_auth", sa.Boolean,
        nullable=False, server_default="false"))
```

### 7.2 Model update

`app/models/node.py`:

```python
class Node(Base):
    # … existing …
    visivo_backend_url: Mapped[str | None] = mapped_column(
        String(256), nullable=True)
    visivo_backend_requires_auth: Mapped[bool] = mapped_column(
        Boolean, server_default="false", nullable=False)
```

### 7.3 DataLink builder

`app/vo/datalink.py:build_service_descriptors()` accepts the
selected replica's node and appends a `visivo-backend` descriptor
when the node has `visivo_backend_url` populated.

### 7.4 Seed update

`app/services/seed_d22.py`: configure `VIS-LOCAL` and `INAF-PUB`
nodes with their VisIVO backend URLs:

```python
"VIS-LOCAL": dict(
    ...
    visivo_backend_url="http://host.docker.internal:8001",
    visivo_backend_requires_auth=False,
),
```

### 7.5 Backwards compatibility

Old clients that don't recognise the new descriptor see it as an
unknown `service_type` and ignore it (per SKAVA's existing extension
contract). Old SKAVA peers that don't expose the descriptor still
return the legacy `primary_access.access_url`, and the desktop's
fallback (download via local backend) handles them.

## 8. Authentication & security

### 8.1 Per-backend tokens

Each `BackendNode` carries its own bearer token. Tokens are stored in
`QSettings` and protected by the OS keychain when available
(`QSettings::Settings` on macOS uses the user's profile, with
filesystem permissions). A future enhancement could swap to the
platform credential store explicitly.

### 8.2 TLS

All backend URLs are expected to be `https://` in production. The
`QNetworkAccessManager` already enforces certificate validation; we
do not weaken this.

### 8.3 Auth scope

A backend's bearer token authorises **only that backend's API**. It
is never forwarded to other backends or to SKAVA. SKAVA's token is
separate (configured in the SKAVA tab).

### 8.4 Replicas vs backends

SKAVA's `replicas[]` list contains storage endpoints (HTTP URLs to
files). VisIVO backends are a separate concept — a backend may serve
files from one or many replicas, depending on what its filesystem
mounts. The mapping is established by SKAVA's node-level
configuration (`visivo_backend_url` lives on the `Node`, not on the
`Replica`).

## 9. Failure modes & mitigations

| Failure | Mitigation |
|---|---|
| Remote backend unreachable | Try next replica's backend; if all fail, fall back to download via local backend |
| Remote backend returns 401 | Surface "auth required" message; if token already set, prompt for re-auth |
| Remote backend returns 5xx | Mark as transient, retry once; if still failing show error and offer "open via local" fallback |
| Remote backend serves wrong / corrupt FITS | Same handling as local backend errors — message box with backend's response |
| Network partition mid-operation | Backend client times out, viewer surfaces "backend lost" status, user can retry or switch backends |
| Backend session expired | `BackendClient` detects 401 with `session_invalid`, transparently re-opens session via `/v1/datasets/open` |

## 10. Migration from single-backend

1. On first launch after upgrade, `Settings` migrates the legacy
   `Backend/url` + `Backend/token` to a `BackendNode { id: "local",
   isLocal: true, isBuiltin: true, … }`.
2. The legacy `getBackendUrl()` / `getBackendToken()` accessors are
   reimplemented as `getLocalBackend().url` / `.token` —
   transparent to existing call sites.
3. Old SKAVA peers (no visivo-backend descriptor) cause the
   download-via-local behaviour, identical to today.
4. Users gradually opt in to multi-backend by adding SRC entries
   to the registry via the new Settings UI.

## 11. Testing strategy

### 11.1 Unit tests

- `Settings::upsertBackendNode()` round-trips correctly.
- `BackendClientFactory::get(id)` returns the same instance on
  repeated calls (session-stable).
- `SkavaClient::parseDatalinkResponse()` extracts
  `visivo-backend` descriptors correctly.
- `pickBackendForDataset()` matches by URL, by node_code, falls back
  to default.

### 11.2 Local end-to-end

Two-backend setup on localhost:
- Backend A on `:8001` serves `/data/`
- Backend B on `:8002` serves `/data2/`
- SKAVA seed associates dataset X with backend A, dataset Y with B
- Desktop registry has both
- Open dataset X → uses backend A (verifiable via backend logs)
- Open dataset Y → uses backend B
- Open dataset Z (no descriptor) → downloads via local backend

### 11.3 SRC-style end-to-end (manual)

- Set up a backend in a Docker container with a mounted data
  directory
- Configure SKAVA seed to point at it
- Verify desktop talks to the container, not to a local cache

### 11.4 Heterogeneous-architecture testbed (Power9)

VisIVO backend has no architecture-specific code: it is Python +
FastAPI + numpy / scipy / astropy / healpy / photutils. All these
have ppc64le wheels on conda-forge and Anaconda channels, and
upstream CPython supports ppc64le natively. The backend therefore
runs **without code changes** on IBM Power9 nodes, which are common
in SKA-SRC infrastructures.

Recommended testbed:

1. Provision an IBM Power9 node with Linux ppc64le (RHEL 8 / Ubuntu
   20.04+ both work).
2. Install Docker for ppc64le.
3. Build the backend image natively on the node:
   ```bash
   cd backend && docker build -t visivo-backend:ppc64le .
   ```
   (Multi-arch buildx is also possible but native build is simpler
   for a dedicated testbed.)
4. Mount the SRC dataset filesystem read-only into the container:
   ```bash
   docker run -d --name visivo-be \
     -v /srv/data:/data:ro \
     -v /var/visivo/exports:/exports \
     -p 8001:8001 \
     visivo-backend:ppc64le
   ```
5. Register the node in the local SKAVA seed (or production SKAVA)
   with `visivo_backend_url=https://<power9-host>:8001` so the
   desktop sees it as a routing target.

For the wheel install path, prefer Anaconda / conda-forge over pure
`pip` to avoid 20–30 min compile times for numba, scipy, healpy:

```bash
conda install -c conda-forge fastapi uvicorn numpy scipy astropy h5py \
    healpy photutils scikit-image reproject numba pandas dask
```

Optional GPU acceleration (`cupy-cuda*`) requires NVIDIA GPU on the
Power9 node (V100/A100, Summit-class) — gracefully degrades to CPU
when absent.

## 12. Future evolution

| Feature | Notes |
|---|---|
| OAuth/OIDC federation | Token model evolves to bearer with refresh, IdP integration |
| Backend health monitor | Desktop polls each registered backend's `/health` periodically; UI shows green/yellow/red |
| Quota-aware routing | SKAVA's `load_score` becomes input to routing; desktop prefers under-loaded backends |
| SODA cutout integration | When SKAVA exposes server-side cutout, the request goes to the backend hosting the dataset |
| Workspace-to-backend push | Reverse direction: user pushes a Workspace Export to a remote backend (already designed; gated on backend write API) |
| Backend discovery via VOSI | SRC backends discovered via SKAVA's `/capabilities` rather than explicit registry |

## 13. Roadmap

| Phase | Work | Estimated effort |
|---|---|---|
| 1 | Settings backend registry (data model + accessors) | 0.5 day |
| 2 | Settings dialog UI for backends | 0.5 day |
| 3 | BackendClientFactory + session caching | 0.5 day |
| 4 | SkavaClient: parse visivo-backend descriptors | 0.25 day |
| 5 | MainWindow routing + viewer badge | 0.5 day |
| 6 | SKAVA migration + model + DataLink builder | 0.5 day |
| 7 | SKAVA seed_d22 update | 0.25 day |
| 8 | End-to-end testing + docs | 0.5 day |

**Total ≈ 3.5 days** of focused work for the full implementation.

## 14. Out of scope

- Workspace-to-backend push from the Data Hub Workspace Exports
  panel (separate item)
- OAuth / OIDC across backends (separate work item)
- Automated SRC backend deployment / Helm chart (operator concern)
- SODA cutout dialog (depends on SKAVA exposing the real endpoint)
- Backend-side cross-replication or data movement (out of scope per
  SKAVA D3.1 §13.3 deferred-feature list)
