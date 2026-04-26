# VisIVO Visual Analytics — Multidisciplinary Panel Review  
**Branch:** `next-server` | **Target commit:** `cc35a366` | **Previous review commit:** `8e8a6c66`  
**Commits reviewed:** `3d5846ca` → `cc35a366` (25 commits, 53 files changed, +8 242 / −720 lines)  
**Review date:** 2026-04-14  
**Supersedes:** all previous reviews (final1–final9)

---

## Panel Composition

| Role | Focus |
|------|-------|
| **Senior Astronomer** | Scientific correctness, WCS, moment/PV/noise, VLKB integration |
| **Senior Cosmologist** | Cosmology module, redshift handling, distance computations |
| **Senior Software Engineer** | Architecture, C++/Qt patterns, code quality, test coverage |
| **Senior HPC Expert** | Memory management, I/O efficiency, concurrency, session lifecycle |

---

## Executive Summary

This sprint closes every outstanding issue from the previous review cycle and adds substantial new capability across all four domains. The backend is now a proper Python package (`sessions`, `fits_dataset`, `compute`, `product_cache`), with **286 pytest test functions** across 10 test files — the single largest quality leap in the project's history. Five previously flagged items (L1–L4, L6) are fully resolved. The startup UX is now production-grade via `StartupDialog` + `BackendLauncher`. VLKB SIAv2 + SODA cutout is reintegrated with correct WCS-based pixel-scale computation for layer alignment. One anonymous-session resource-leak concern (new, medium severity) is introduced and one load-more recovery issue (L5) remains open.

**Overall assessment: Excellent. Publication-ready architecture. One medium-severity issue to address before release.**

---

## 1. Senior Astronomer

### 1.1 VLKB SIAv2 Inventory and SODA Cutout — ✅ Correctly Implemented

The VLKB data discovery pipeline is back and substantially improved over its legacy predecessor.

**`VlkbQueryService`** issues standard SIAv2 `CIRCLE` and `RANGE` queries to the VLKB endpoint (`/siav2/query?POS=CIRCLE+l+b+r&POSSYS=GALACTIC`), parsing responses as VOTable byte arrays. The Galactic longitude wrap-around for `l < 0°` is handled (`l += 180°`). SODA cutouts (`/soda/sync?ID=…&POS=…`) stream the FITS response to a UUID-named temp file on the backend filesystem, which is then opened via `open_dataset` — the correct backend-authoritative pipeline. FITS data never bypasses the backend.

**`VLKBInventoryTree`** parses the VOTable via `QXmlStreamReader` and presents it as a tree widget; `cutoutRequested(ivoID, posCutout, displayName, type)` is emitted when the user selects a cutout row. The signal wiring in `MainWindow` is clean: a dedicated `VlkbQueryService` is allocated per search, connected once, and the `tree` widget lifetime is managed by the standalone inventory window. Good object ownership discipline.

**VLKB layer alignment fix (commits `e4d378c2`, `a31c123e`) — critical correctness fix:** The previous pipeline read `CDELT` directly from the header. For FITS files produced with a CD-matrix (common in interferometric imaging), `CDELT` defaults to 1.0 and the computed pixel scale is completely wrong. The new `effective_cdelt()` method in `ScientificFitsDataset` uses a 1-pixel perturbation at the reference pixel:

```python
sky_ref = celestial.all_pix2world([[crpix1_0, crpix2_0]], 0)[0]
sky_p1x = celestial.all_pix2world([[crpix1_0+1.0, crpix2_0]], 0)[0]
eff1 = float(sky_p1x[0] - sky_ref[0])  # deg/pixel with correct sign
```

This correctly derives the effective pixel scale including orientation and sign for any valid WCS (TAN, SIN, ZEA, …), and is exposed as `"effective_cdelt"` in `geometry_metadata()`. The `vtkWindowImage` layer alignment now reads `effective_cdelt` and `wcs_ref_sky_00` / `wcs_ref_sky_0100` from the metadata, replacing the broken raw-CDELT path. NaN-filled pixels at resampled-layer boundaries are made transparent in VTK. This is the correct approach for multi-resolution layer composition.

**Caveat (minor):** `effective_cdelt()` computes the perturbation at the single reference pixel. For strongly distorted projections (e.g., ZPN), the pixel scale may vary significantly across the field. This is acceptable for the current layer-alignment use case (which assumes a locally TAN approximation), but should be noted in the user documentation for wide-field images.

### 1.2 WCS Sanitisation — `fits_dataset.py` Extraction — ✅ Improved

`ScientificFitsDataset` has been moved from an inline helper to a proper `backend/app/fits_dataset.py` module. The class now also exposes:
- `spatial_pixel_scale_arcsec()` using `astropy.wcs.utils.proj_plane_pixel_scales` on the celestial sub-WCS (handles rotated/skewed projections correctly), falling back to `|CDELT1| × 3600` only when WCS fails.
- `wcs_ref_sky_00` / `wcs_ref_sky_0100`: pre-computed sky coordinates for two reference pixels exposed via `geometry_metadata()`, enabling the client-side layer alignment without an additional API call.
- `degenerate_axes_summary()`: human-readable label for collapsed axes (e.g., `"Collapsed axes: STOKES=1"`)

The `_sanitize_header_for_wcs()` logic is unchanged from the previous review and remains correct.

**`test_wcs_sanitization.py` (33 tests)** covers: static helpers, 3-axis cubes with malformed CUNIT, partial sanitisation, Galactic coordinates (GLON/GLAT), empty CUNIT, non-celestial non-standard units, `geometry_metadata()` after sanitisation, and non-regression for valid headers. Coverage is excellent.

### 1.3 2D Slice Statistics Fix (commit `3d5846ca`) — ✅ Fixed

The 2D slice mean and RMS now correctly use the visible scalar range after threshold masking, with improved numeric readability (scientific notation for very small/large values). No scientific concern remaining in this area.

### 1.4 Scientific Verdict

All scientific workflows (moments M0/M1/M2/M6/M8/M10, PV, noise, isosurface, VLKB SIA+SODA) are backend-authoritative and have no remaining correctness issues. The WCS handling is now substantially more robust than most other open-source radio-astronomy visualisation tools.

---

## 2. Senior Cosmologist

### 2.1 Redshift Sentinel Replacement (commits `3aa65351`, `191db868`) — ✅ L4 Resolved

The ambiguous `0.0` sentinel for invalid/missing redshifts has been replaced with `None` (JSON `null`) throughout the cosmology response models:

```python
# CosmologyBatchResponse
distances_Mpc: list[float | None] = []

# batch logic
distances = _np.full_like(zs, float("nan"))
valid_mask = _np.isfinite(zs) & (zs >= 0.0)
if valid_mask.any():
    distances[valid_mask] = cosmo.comoving_distance(zs[valid_mask]).to("Mpc").value
# NaN → None for JSON serialisation
distances_out = [None if _np.isnan(d) else float(d) for d in distances]
```

This correctly distinguishes `z=0.0` (local universe, valid, `d=0.0 Mpc`) from `z=None` (invalid or missing, `d=null`). The `valid_mask` now uses `np.isfinite(zs) & (zs >= 0.0)`, which correctly excludes NaN, inf, and negative redshifts.

**`test_redshift_nan.py` (26 tests)** covers all paths:
- `CosmologyDistanceResponse` defaults to `None` when `valid=False`
- Real `z=0.0` returns `d=0.0` (not `None`)
- Batch: invalid redshifts produce `None`, valid `z=0.0` produces `0.0`
- `_apply_tabular_query` NaN handling (sort, filter, in-list) — 4 tests

This is the correct mathematical treatment. The comment in the code (`"JSON has no NaN literal, and 0.0 would be ambiguous with a real z=0 result"`) accurately captures the design rationale.

### 2.2 Cosmology Model Correctness — No Change

The four Planck/WMAP cosmologies and the `FlatLambdaCDM` integration are unchanged and correct. The batch endpoint still caps at 100k redshifts.

### 2.3 Cosmological Verdict

No remaining issues in the cosmology module.

---

## 3. Senior Software Engineer

### 3.1 Backend Modularisation — ✅ Major Structural Improvement

`backend/app/main.py` has been modularised into four dedicated modules:

| Module | Contents | Lines |
|--------|----------|-------|
| `sessions.py` | `Session`, `SessionRegistry`, `REGISTRY`, `get_session()` | ~220 |
| `fits_dataset.py` | `ScientificFitsDataset`, `BeamMetadata` | ~280 |
| `compute.py` | `_moment_map_from_array`, `_pv_from_array`, encoding helpers | ~180 |
| `product_cache.py` | `ProductCache`, `PRODUCT_CACHE` | ~90 |

`main.py` now sits at 2 539 lines (down from the monolithic peak). Each module has a clear single responsibility, proper module-level docstrings, and imports only what it needs. This is the correct direction; `main.py` could be reduced further by extracting the HiPS and VLKB endpoint groups.

### 3.2 StartupDialog + BackendLauncher — ✅ Production-Grade

`StartupDialog` (590 lines, 130 header) drives a three-step wizard: Backend → Auth → Ready. It is cleanly separated from `MainWindow` (it only signals readiness), uses `QPointer` for all child widgets, and handles all error states (retry, show logs, quit, skip auth). The step-state machine (Pending/Active/Done/Error) with Unicode icon glyphs and CSS colour coding is clear and maintainable.

`BackendLauncher` (439 lines, 104 header) implements a 6-step Python resolution chain:
1. `VISIVO_BACKEND_PYTHON` env override
2. `$VIRTUAL_ENV/bin/python` (active venv)
3. `<backendDir>/../.venv/bin/python` (project venv)
4. Bundled Python alongside the executable
5. Settings value (detected at startup)
6. Hard `python3` / `python` (PATH last resort)

This is robust for both development (source tree with .venv) and distribution (bundled Python). The platform-specific bundle path handling (macOS `Contents/Resources/backend`, Linux `<exe_dir>/backend`) is correct. The health-poll loop (`QTimer`, max ~30 s) and `QProcess` stdout/stderr capture for the log panel are well-implemented.

**One concern (low severity):** `findBackendDir()` searches up to 5 candidate paths. If none match, it returns an empty string, and `startAsync()` falls through to `failed()`. The error message could be more specific about which paths were searched, to help end users diagnose installation problems.

### 3.3 Test Coverage — ✅ L6 Resolved, Outstanding Achievement

| Test file | Tests | Scope |
|-----------|-------|-------|
| `test_api.py` | 45 | Full HTTP round-trips (pytest-asyncio + httpx) |
| `test_tabular_query.py` | 48 | `_apply_tabular_query()` edge cases |
| `test_fits_dataset.py` | 39 | `ScientificFitsDataset` |
| `test_wcs_sanitization.py` | 33 | WCS sanitisation, geometry_metadata |
| `test_redshift_nan.py` | 26 | NaN/None sentinel + tabular NaN handling |
| `test_compute.py` | 23 | Worker functions (moment, PV, noise) |
| `test_catalogue_overlay.py` | 22 | Overlay CSV loader, bounding-box filter |
| `test_hips_cache_policy.py` | 19 | TTL purge, LRU eviction |
| `test_memmap_cleanup.py` | 18 | Session close, FD release |
| `test_resolver_cache.py` | 13 | Sesame LRU cache, normalisation |
| **C++ (QTestLib)** | **36** | BackendClient JSON parsing, CatalogueParser |
| **Total** | **322** | |

The jump from 36 C++ tests to 322 total (286 Python + 36 C++) is the largest single quality improvement in the project history. `test_api.py` uses `pytest-asyncio` with in-process FastAPI via `httpx.AsyncClient`, meaning real HTTP round-trips are exercised without a live server. This is the correct pattern for API integration tests.

**Remaining gap (low):** `test_api.py` tests do not yet cover the HiPS tile proxy endpoints, the VLKB proxy (expected — no live VLKB in CI), or the `/v1/cosmology/distance/batch` endpoint directly. These are acceptable omissions for a CI environment.

### 3.4 CubeUiAssembler Refactored Header — ✅ Improved

`CubeUiAssembler.h` has been restructured (201 → 221 lines). The data-struct pattern and no-circular-dependency constraint are preserved. The assembler is still the correct single point of truth for all sidebar/status-bar rendering.

### 3.5 Advanced LUT Editor — ✅ Good Implementation

`LUTCustomizerDialog` now has full-height vertical markers (min cyan, max orange) implemented as `QCPItemStraightLine` objects, draggable with mouse events, with live enforcement that min < max. Gradient preview icons populate the colormap combo box using the same `buildLutPreview()` static helper. This is consistent with the approach in the sidebar.

**Minor concern:** `selectReferenceLine` / `moveReferenceLine` / `deselectReferenceLine` respond to raw `QMouseEvent*` — ensure these are connected to `QCustomPlot` signals (not raw Qt mouse events on the widget) to avoid interference with QCustomPlot's own interaction handlers.

### 3.6 Anonymous Session Resource Leak — ⚠️ M3 (Medium, New)

`SessionRegistry._evict_stale()` explicitly skips the anonymous session:

```python
stale = [
    sid for sid, s in self._sessions.items()
    if sid != _ANON_SESSION_ID and s.idle_seconds() > self._ttl
]
```

This means: if the Qt client connects without sending `X-Visivo-Session` (backward-compat mode), all VBT `np.memmap` file descriptors opened in that session accumulate indefinitely. They are released only when the server process exits. In a long-running server session this can exhaust OS file descriptor limits.

**Recommended fix:** Apply the TTL to the anonymous session as well, but call `session.close()` on eviction (without removing it from `_sessions` — just clearing its `datasets` dict). Or: document explicitly that backward-compat mode is unsupported for long-running server processes and emit a startup warning when the anonymous session is used.

### 3.7 Paper File Committed to Repo (low)

`backend/Vitello_VisIVO_AstronomyComputing_2026.tex` (786 lines) was committed into the repository. A LaTeX draft paper is not software, does not belong in `backend/`, and will confuse future contributors. Recommend moving it to a dedicated `docs/paper/` directory or removing it from the tracked tree with a `.gitignore` entry.

---

## 4. Senior HPC Expert

### 4.1 HiPS Disk Cache TTL + LRU Eviction (commit `f0e6f674`) — ✅ L1 Resolved

The disk cache is now managed by three cooperating helpers:

```python
_HIPS_CACHE_TTL_SECONDS  = int(os.environ.get("VISIVO_HIPS_CACHE_TTL_SECONDS", str(7 * 24 * 3600)))
_HIPS_CACHE_MAX_BYTES    = int(os.environ.get("VISIVO_HIPS_CACHE_MAX_BYTES",   str(2 * 1024**3)))
_HIPS_CACHE_CLEANUP_INTERVAL_SECONDS = 3600
```

`_maybe_cleanup_hips_cache()` is called inside every tile-fetch response path, but rate-limited by `_last_hips_cache_cleanup_ts` to at most once per hour. `_purge_expired_hips_cache()` deletes files older than the TTL by `mtime`. `_enforce_hips_cache_size_limit()` iterates files sorted by `mtime` ascending (oldest first) and deletes until total size ≤ max. Both limits are configurable via environment variables.

**Correctness:** The eviction uses file `mtime` as the LRU proxy. This is correct: `_HIPS_CACHE_MAX_BYTES` default of 2 GiB is appropriate for a workstation. The cleanup is synchronous but rate-limited, so it adds at most one directory scan per tile batch per hour — acceptable overhead.

**One edge case (low):** `_hips_cache_size_and_entries()` calls `os.stat()` on every cached file in the scan. If the cache directory is large (millions of files), this scan can be slow. For the expected tile count of a few thousand files this is not a concern.

### 4.2 Sesame Resolver LRU Cache (commit `973fbfd9`) — ✅ L2 Resolved

```python
@functools.lru_cache(maxsize=256)
def _resolve_target_cached(name: str) -> dict:
    ...
    coord = SkyCoord.from_name(name)
    ...

def _resolve_target_sync(name: str) -> dict:
    return _resolve_target_cached(name.strip().lower())
```

The `strip().lower()` normalisation before the cache key lookup is correct. `maxsize=256` is appropriate for typical observatory usage (256 objects covers essentially any observing campaign). `clear_resolver_cache()` is available for testing.

**`test_resolver_cache.py` (13 tests)** verifies: cache hits, miss-then-hit, case-insensitive normalisation, whitespace normalisation, error results not cached (important!), and `cache_clear()`. The decision to not cache failure results is correct — a transient Sesame timeout should not permanently prevent resolution of a valid object name.

### 4.3 Memmap FD Release on Session Close (commit `906eee3e`) — ✅ L3 Resolved

`sessions.py` implements `_safe_close_memmap()` and calls it from `Session.close()`:

```python
def _safe_close_memmap(arr: object) -> None:
    try:
        mmap_obj = getattr(arr, "_mmap", None)
        if mmap_obj is not None:
            mmap_obj.close()
    except Exception:
        pass

def close(self) -> None:
    for dataset_id, entry in self.datasets.items():
        if entry.get("kind") == "vbt":
            mm = entry.get("memmap")
            if mm is not None:
                _safe_close_memmap(mm)
```

`SessionRegistry._evict_stale()` calls `session.close()` after removing the session from `_sessions`. The pop-then-close order is correct: no new requests can acquire a reference to the session after the pop, so the close is race-free with respect to incoming requests. Existing in-flight requests that already hold a reference will complete normally.

**`test_memmap_cleanup.py` (18 tests)** includes: FD released on explicit close, FD released on TTL eviction, idempotent close, non-VBT datasets not affected, `_safe_close_memmap` robustness. This is exemplary test design.

### 4.4 Per-Session Concurrency Limit — ✅ Good Addition

`Session.try_acquire_task_slot()` / `release_task_slot()` implement a per-session semaphore (default `_MAX_CONCURRENT_TASKS=3`, env-configurable). This prevents a single session from saturating the process pool. The acquire/release pair is correctly used around the heavy compute endpoints via `_run_with_limit()`.

### 4.5 Loading Overlay in VBT/Image/VbtVolume Viewers (commit `8416ba25`) — ✅ Good UX

The VBT viewer uses a `vtkTextActor2D` positioned at (0.5, 0.5) in display coordinates, visible while `buildPointCloud()` runs in the background, hidden when the result is applied. This is a lightweight, correct approach — the VTK scene is valid throughout (no blank frames), the actor is simply toggled. VbtVolume uses the same pattern.

### 4.6 Lazy I/O and Pagination — No Regression

The `np.memmap` lazy-column materialisation, DataFrame cache, and 50 000-row pagination are unchanged from the previous review. No regressions detected.

### 4.7 Load-More Session-Expiry Recovery — ⚠️ L5 (Low, Ongoing)

When a session expires mid-pagination (between the initial filter and a "Load more" click), `get_session()` creates a new session with no datasets. The subsequent tabular query returns a 404 `dataset_not_found`. `vtkWindowVbt` catches the error and disables the "Load more" button, but does not attempt to re-open the dataset and resume. This requires the user to re-open the dataset manually. Acceptable for the current release given the 30-minute TTL, but should be addressed before HPC deployments with very large tables.

### 4.8 HPC Summary

| Issue | Status |
|-------|--------|
| L1: HiPS disk cache unbounded | ✅ Fixed (TTL 7d + LRU 2 GiB) |
| L2: Sesame resolver no cache | ✅ Fixed (LRU 256, failure not cached) |
| L3: memmap FD not closed on session expire | ✅ Fixed (`_safe_close_memmap` + `session.close()`) |
| L4: z=0.0 sentinel ambiguous | ✅ Fixed (None/NaN semantics) |
| L5: Load-more session-expiry recovery | ⚠️ Open (low) |
| M3: Anonymous session memmap FDs never released | ⚠️ New (medium) |

---

## 5. File Size Monitoring

| File | `8e8a6c66` | `cc35a366` | Δ |
|------|-----------|-----------|---|
| `vtkWindowCube.cpp` | 7 814 | 7 921 | +107 |
| `vtkWindowCube.h` | 705 | 708 | +3 |
| `CubeUiAssembler.cpp` | 902 | 979 | +77 |
| `CubeUiAssembler.h` | 201 | 221 | +20 |
| `vtkWindowImage.cpp` | ~3 500 | 4 351 | +851 (VLKB layer) |
| `vtkWindowVbt.cpp` | ~840 | 1 614 | +774 (VBT revamp) |
| `vtkWindowVbtVolume.cpp` | ~350 | ~610 | +260 |
| `backend/app/main.py` | 2 320 | 2 539 | +219 |
| `StartupDialog.cpp` | — | 590 | new |
| `BackendLauncher.cpp` | — | 439 | new |
| `VLKBInventoryTree.cpp` | — | 285 | new |
| `VlkbQueryService.cpp` | — | 117 | new |
| `sessions.py` | — | ~220 | extracted |
| `fits_dataset.py` | — | ~280 | extracted |
| `compute.py` | — | ~180 | extracted |
| `product_cache.py` | — | ~90 | extracted |

`vtkWindowCube.cpp` is stable (+107 for the Advanced LUT wiring). `vtkWindowImage.cpp` growth (+851) is justified by the VLKB layer pipeline; it should be monitored — if another 400 lines are added, a `VlkbLayerController` extraction is warranted following the `NoiseController` / `MomentMapController` pattern.

---

## 6. Open Issues — Severity Summary

| ID | Description | Severity | Status |
|----|-------------|----------|--------|
| **M3** | Anonymous session memmap FDs never released (eviction skip) | Medium | 🔴 New |
| L5 | Load-more session-expiry recovery | Low | 🟡 Open |
| L7 | `CubeUiAssembler.h` requires full Qt type definitions in some include chains | Low | 🟡 Open |
| — | `vtkWindowImage.cpp` approaching god-class threshold (+851 lines) | Low | 🟡 Watch |
| — | `findBackendDir()` error message could specify searched paths | Low | 🟢 Minor |
| — | `.tex` draft committed to `backend/` | Low | 🟢 Housekeeping |

**Previously open issues now closed:** L1 ✅, L2 ✅, L3 ✅, L4 ✅, L6 ✅

---

## 7. Recommended Actions (Priority Order)

**Before next release:**
1. **(M3)** Apply TTL eviction to the anonymous session, or emit a deprecation warning and document that backward-compat mode is unsupported for long-running servers.

**Next sprint:**
2. **(Watch)** Extract a `VlkbLayerController` from `vtkWindowImage.cpp` if the file exceeds 4 500 lines with upcoming features.
3. **(L5)** Implement dataset re-open on load-more 404 (re-open → re-filter → resume pagination).

**Housekeeping:**
4. Move `Vitello_VisIVO_AstronomyComputing_2026.tex` to `docs/paper/` and add an entry to `.gitignore` for draft paper artifacts.
5. Clarify `findBackendDir()` failure message to list all searched paths.

---

## 8. Panel Verdict

| Domain | Rating |
|--------|--------|
| Scientific correctness | ⭐⭐⭐⭐⭐ |
| Cosmological correctness | ⭐⭐⭐⭐⭐ |
| Software architecture | ⭐⭐⭐⭐½ |
| HPC / resource management | ⭐⭐⭐⭐½ |
| **Overall** | **⭐⭐⭐⭐½** |

The codebase is now at a level of quality suitable for publication, community release, and use in production radio-astronomy workflows. The addition of 286 Python tests and the backend modularisation are transformative improvements. The single medium-severity issue (M3) is straightforward to fix. No breaking architectural decisions remain to be made.
