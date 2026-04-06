# VisIVO Visual Analytics — Multidisciplinary Panel Review
## Iteration 4 — next-server branch, commit `04af233f`
**Date:** 2026-04-06
**Panel:** Senior Astronomer · Senior Cosmologist · Senior Software Engineer · Senior HPC Expert
**Scope:** 4 new commits since Iteration 3 (base: `8000ff94`)

---

## Commits Reviewed

| Hash | Title |
|------|-------|
| `743a7ccf` | Refactor moment computation so VisIVO uses a single authoritative backend-based implementation, even for local usage |
| `aeaebbc7` | Refactor moment computation to use backend as single authoritative path |
| `49c361ed` | Refactor moment flow to enforce remote-only backend dataset contract |
| `04af233f` | Remove legacy local moment filter and finalize backend-only moment path |

---

## Status of Previously Flagged Issues

| Issue (v3) | Status |
|---|---|
| Three moment implementations — no cross-validation | ✅ **RESOLVED** — `vtkMomentMapFilter` deleted, local C++ path deleted, one implementation remains |
| Obsolete comment in `MomentProcessingService.h` ("Future remote hook…") | ✅ **RESOLVED** — comment replaced with accurate description |
| God class `vtkWindowCube.cpp` (7,686 lines) | ⚠️ **OPEN** — now 7,682 lines; no structural decomposition |
| `Catalogue3DEntry` hardcoded schema | ⚠️ **OPEN** |
| `BackendClient` missing `cancelTask()` / `requestNoise()` C++ API | ⚠️ **OPEN** |
| Noise endpoint not wired to UI | ⚠️ **OPEN** |
| Cosmology `/v1/cosmology/distance` not connected to Catalogue3D viewer | ⚠️ **OPEN** |
| Base64 33% overhead on all binary payloads | ⚠️ **OPEN** |
| PV loads full spectral depth | ⚠️ **OPEN** |

---

## Panel Assessment

---

### Senior Astronomer

**Significant change: moment computation now requires backend connection for all cube windows opened via "Open Local File".**

This is the most consequential user-facing change in these 4 commits. When a user opens a FITS cube through File → Open (the primary workflow), the cube viewer loads correctly using the local FITS preview. However, when the user opens the Moment menu and selects any moment order, they receive a status bar message:

> "Moment computation requires a backend-visible dataset. Open or stage the dataset on the backend first."

The Moment menu is fully visible, fully enabled, and gives no prior indication that it requires a backend. There is no dialog, no tooltip, no asterisk. For an astronomer who does not have a backend configured — or who is working offline — moment maps are silently unavailable.

**What does work:** Any cube opened via File → Open Remote (which calls `openDataset()` and returns a `datasetId`) enters `isRemoteMode = true` and has full moment support with correct WCS units, `BUNIT`, and spectral axis metadata. This path is now the single authoritative path, and the Python implementation is scientifically correct.

**The cross-validation issue is resolved by architectural choice:** there is only one moment implementation left — the Python `_moment_map_from_array` path backed by `astropy.wcs`. This is a net positive for scientific correctness.

**Scientific credibility (remote path):** High. M0/M1/M2/M6/M8/M10 units are correctly derived from BUNIT × spectral_unit. WCS status is propagated to the UI. The provenance panel is detailed. This is solid work.

**Scientific credibility (local path):** Nil — not because the science is wrong, but because the feature is intentionally unavailable until a staging pipeline exists.

**Assessment:** The scientific correctness on the remote path is excellent. The UX regression for local-only users is real and should be treated as a blocking issue for any public release. The Moment menu must be either hidden or show a modal explaining the requirement when no `datasetId` is available.

---

### Senior Cosmologist

**No backend changes in these 4 commits.** The Python backend (`main.py`, `compute.py`, `fits_dataset.py`) was not modified. The `/v1/cosmology/distance` endpoint and `Catalogue3DEntry` hardcoded schema remain in the same state as Iteration 3.

**What changed cosmologically:** nothing directly. What changed architecturally is that the moment pipeline is now solely Python-based, which is relevant because it removes the last scientific discrepancy risk (C++ CDELT3 vs Python WCS). From a cosmological simulation perspective — where moment maps of spectral cubes are less common — this is a non-issue. The hardcoded catalogue schema (`redshift`, `distanceMpc`, `morphology`, `fluxMJy`) remains the primary cosmology blocker.

**Positive:** The architecture documentation (`service-mapping-note.md`) accurately identifies `DatasetOpenService` as the next clean remote-split candidate and acknowledges the staging gap explicitly. The documentation is honest about what is deferred.

**Assessment:** Cosmology credibility unchanged from v3. The single-implementation moment pipeline removes scientific divergence risk. The hardcoded catalogue schema and disconnected cosmology API endpoint remain Critical gaps for cosmological use cases.

---

### Senior Software Engineer

**These commits complete the most significant architectural decision to date: a single authoritative scientific implementation.**

The `vtkMomentMapFilter` (339 lines of C++ implementing its own CDELT3-based spectral integration) has been deleted. `computeMomentMapLocalWithConfig` is gone. `MomentComputeMode` enum is gone. `MomentProcessingService` no longer holds references to `vtkMomentMapFilter` or `vtkLookupTable`. The service is now a lean 100-line facade over the backend API. The build graph is cleaner: `visivo_shared_vtk` no longer contains scientific numerical computation.

**Points of blame — new:**

**P1 — Moment menu fully enabled with no backend guard (Critical UX regression):**

In `vtkWindowCube.cpp` at line 6792:
```cpp
this->momentComputeWatcher.setFuture(QtConcurrent::run(
    &computeMomentMap, MomentMapComputeRequest {
        this->filepath,
        this->isRemoteMode ? this->remoteDatasetId : QString {},   // ← empty for local open
        this->isRemoteMode ? this->remoteBackendUrl : QString {},  // ← empty for local open
        ...
    }));
```

For any cube opened via `DatasetWindowFactory::createWindow` → `vtkWindowCube(filepath, backendUrl, backendToken)`, `datasetId` is empty → `isRemoteMode = false` → the moment dispatch passes empty identifiers. `MomentProcessingService::computeMomentRemote()` returns an error. The error surfaces only as a status bar message:

```cpp
this->statusBar()->showMessage(result.errorMessage);
```

The `menuMoment` entry in the menu bar is always visible (confirmed in `vtkWindowCube.ui`). The actions are always enabled at startup. There is no `isRemoteMode`-conditional guard on the moment menu.

**Root cause:** The staging/upload pipeline does not exist. The architectural decision to require backend visibility is correct, but the UX consequence — an always-visible, always-enabled Moment menu that silently fails for all locally-opened files — is a regression.

**Minimum fix (unambiguous):** In the cube window constructor, when `isRemoteMode == false`, disable the `menuMoment` submenu and set a tooltip: "Moment computation requires a backend-visible dataset. Use File → Open Remote to compute moments." This is a 5-line change.

**Better fix:** Implement explicit backend staging for local files. `BackendClient` already has `openDataset(path)`. When a local cube is opened and a backend URL is configured, call `openDataset()` in the background. If successful, promote to `isRemoteMode = true` with the returned `datasetId`. This eliminates the gap for any user who has a backend running locally (which is the common developer/astronomer use case).

**P2 — Local backend scenario: path reachability mismatch:**

A user with a local backend at `localhost:8000` opens a FITS file via File → Open (not Remote). The backend could access the same file at the same path. But the error message says "Stage or upload the dataset to the backend first" — with no mention that using File → Open Remote with the same file path would solve the problem immediately. This is an onboarding gap that will cause confusion.

**P3 — Architecture documentation is excellent:**

`docs/architecture-note.md` (136 lines) and `docs/service-mapping-note.md` are well-written, honest, and do not overstate the current state. The deferred-items section is accurate. The "Stable Local Boundaries" section correctly describes the service contracts. This documentation will accelerate onboarding and future contributors.

**P4 — God class is unchanged:**

`vtkWindowCube.cpp` is now 7,682 lines (down 4 lines). The 4 new commits removed ~503 lines of `vtkMomentMapFilter.cpp` from the build, but the vtkWindowCube god class itself was barely touched. The class still owns rendering, WCS display, overlays, moments, PV, regions, isosurface, probe, slice cache, remote state machine, and diagnostics. No decomposition work was started.

**P5 — No new C++ tests:**

The backend test suite (892 API tests, 376 compute tests, 518 dataset tests) was not modified in these 4 commits. No C++ unit tests exist for `MomentProcessingService`, `MomentMapComputeTask`, or `BackendClient`. The backend-only moment path is exercised only by end-to-end integration (manual or backend tests). A failure in the C++ bridge (e.g., a wrong field name in JSON parsing) would be invisible in CI.

**Assessment:** The architectural direction is correct and the code quality is improving. P1 is the only unambiguous regression — it must be addressed before any release. P2 is a UX issue that can be addressed with better error messages as a minimum. P3 is a genuine positive. P4 and P5 are ongoing structural debts.

---

### Senior HPC Expert

**No changes to the computation, transport, or concurrency layer** in these 4 commits. The backend process pool, task registry, per-session concurrency limits, and product cache are unchanged.

**Implication of the single-implementation moment pipeline:**

All moment computation now runs on the backend `ProcessPoolExecutor`. This is correct for HPC fitness: Python with numpy/astropy on a compute node, not on the client desktop. The removal of the C++ CDELT3 path eliminates a class of result divergence that would have been opaque to users.

**Remaining HPC concerns (unchanged from v3):**

The PV diagram still loads the full spectral depth into the slab. For a 100-channel ALMA cube this is acceptable; for an SKA-scale 10,000-channel cube this is not. The base64 JSON transport still imposes 33% overhead on every binary payload; for large moment maps (4K × 4K float32 = 64 MB → 85 MB over the wire) this is measurable latency on any non-gigabit link. Neither issue was addressed in these commits, and neither was expected to be.

**New concern:** The local staging gap (P1 above) means that for a user who has a large dataset on their desktop and wants to run moments, they must use File → Open Remote and navigate the backend's file browser to the same path. For datasets on network mounts, this may require backend path configuration. There is no documentation of this requirement in the UI.

**Assessment:** HPC fitness on the backend path is unchanged and solid. The frontend changes do not affect throughput, memory, or concurrency. The primary new concern is operational: users must understand the two-tier open model to access moment computation.

---

## Panel Convergence

**Converged finding:** The elimination of `vtkMomentMapFilter` and the C++ local moment path is the correct long-term architectural decision. A single Python/WCS implementation is scientifically more correct, easier to maintain, and easier to test. The panel agrees this was the right move.

**Converged finding:** The moment menu UX regression is the only Critical new issue introduced in these 4 commits. It is a direct consequence of removing the fallback without replacing it with either a staging pipeline or a conditional UI guard. It must be addressed before any public release.

**Divergence — priority of staging pipeline vs. UI guard:**

The Astronomer and Software Engineer both flag the staging gap, but differ on priority. The Astronomer sees it as blocking for typical solo-researcher workflows (open local file, compute moment). The Software Engineer sees the 5-line UI guard as a viable minimum fix that unblocks release, with staging as a medium-term item. The HPC Expert notes that staging matters for large datasets and should not be ad-hoc when implemented. The panel recommends the UI guard immediately and staging in the next iteration.

---

## Open Issues — Complete Ranked List

| Rank | Severity | Issue | Owner | Evidence |
|------|----------|-------|-------|---------|
| 1 | **Critical** | Moment menu visible/enabled for local-mode cube windows; clicking any order produces only a status bar message with no actionable next step | GUI | `vtkWindowCube.cpp:6792`; `MomentProcessingService.cpp:29-32`; `vtkWindowCube.ui` menuMoment always present |
| 2 | **High** | No dataset staging/upload pipeline — local files cannot be promoted to backend-visible datasets within the "Open Local File" flow | Architecture | `service-mapping-note.md` §MomentProcessingService; `BackendClient.h` has no upload method |
| 3 | **High** | Error message for unstaged datasets does not hint at "Open Remote" as the immediate workaround | UX | `MomentProcessingService.cpp:30-31` |
| 4 | **High** | `vtkWindowCube.cpp` god class at 7,682 lines — owns rendering, WCS, overlays, moments, PV, isosurface, probe, slice cache, remote state machine | Architecture | `wc -l src/gui/vtkWindowCube.cpp` |
| 5 | **High** | `Catalogue3DEntry` has hardcoded fields (`redshift`, `distanceMpc`, `morphology`, `fluxMJy`) — dynamic catalogues (Euclid, LSST) require new struct fields | Cosmology | `src/gui/Catalogue3DParser.h` |
| 6 | **Medium** | `/v1/cosmology/distance` backend endpoint not connected to Catalogue3D C++ viewer | Cosmology | `backend/app/main.py` vs `src/gui/Catalogue3DParser.h` |
| 7 | **Medium** | `/v1/cube/noise` backend endpoint has no UI widget in the cube viewer | SWE | `BackendClient.h` has no `requestNoise()`; no noise UI in `vtkWindowCube.ui` |
| 8 | **Medium** | `BackendClient` missing `cancelTask()` — `DELETE /v1/tasks/{id}` exists on backend but C++ client cannot call it | SWE | `src/app/BackendClient.h` public interface |
| 9 | **Medium** | No C++ unit tests for `MomentProcessingService`, `MomentMapComputeTask`, or `BackendClient` bridge layer | SWE | `src/vtk/` — no test directory |
| 10 | **Low** | Base64 binary encoding imposes 33% overhead on all payloads; measurable for large moment maps | HPC | `BackendClient::decodePayload()`; `compute.py` base64 encoding |
| 11 | **Low** | PV diagram loads full spectral depth into slab — memory concern for very deep cubes | HPC | `backend/app/compute.py worker_pv()` |
| 12 | **Low** | No tiled/multiresolution delivery for large 2D full-resolution images | HPC | `BackendClient::requestImage()` |

---

## Recommended Next Steps

**Immediate (before any release candidate):**

1. Disable `menuMoment` (or all moment order actions) when `isRemoteMode == false` in `vtkWindowCube`. Set tooltip/status bar hint: "Open via File → Open Remote to enable moment computation." — 5-line fix, no architectural change.

2. Improve the error message in `MomentProcessingService::computeMomentRemote()` to explicitly mention File → Open Remote as the solution.

**Next iteration:**

3. Implement background staging: when a cube is opened locally and a backend URL is configured, call `BackendClient::openDataset(filepath)` asynchronously. If successful, update `remoteDatasetId`/`remoteSessionId`/`isRemoteMode` and enable the moment menu. This closes the local-backend gap completely.

4. Wire `/v1/cube/noise` to a UI widget in the cube viewer (noise estimation panel or status bar display).

5. Add `cancelTask()` to `BackendClient` to expose `DELETE /v1/tasks/{id}` for the C++ layer.

**Strategic:**

6. Extract `MomentMapController` from `vtkWindowCube` as the first step of god-class decomposition. The moment display source, LUT, provenance panel, and apply/dispatch logic form a cohesive sub-object.

7. Design and implement a dynamic field map for `Catalogue3DEntry` to support arbitrary catalogue schemas.

8. Connect `/v1/cosmology/distance` to the Catalogue3D viewer for backend-computed comoving/luminosity distances.

---

## Summary Scorecard

| Dimension | v3 Score | v4 Score | Δ |
|-----------|----------|----------|---|
| Astronomical correctness (remote path) | ★★★★☆ | ★★★★☆ | — |
| Astronomical accessibility (local path) | ★★★☆☆ | ★★☆☆☆ | ▼ |
| Cosmological workflow coverage | ★★☆☆☆ | ★★☆☆☆ | — |
| Software architecture | ★★★☆☆ | ★★★½☆ | ▲ |
| HPC/scalability fitness | ★★★★☆ | ★★★★☆ | — |
| Code maintainability | ★★★☆☆ | ★★★★☆ | ▲ |
| Documentation quality | ★★☆☆☆ | ★★★★☆ | ▲▲ |

**Legend:** ▲ improved · ▼ regression · — unchanged

The single-implementation moment pipeline and architecture documentation are genuine step-changes. The local-mode UX regression is real and the only Critical new finding. The recommended immediate fix is a 5-line guard.
