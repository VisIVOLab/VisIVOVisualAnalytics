# Testing
 
## Suite overview

QTest-based headless unit suite under `tests/`. No VTK, no Qt::Widgets, no live backend required.

```
tests/
  CMakeLists.txt                   # visivo_test_support + VisIVOTests
  main.cpp                         # QCoreApplication + QTest::qExec for each class
  Test<Name>.h                     # Q_OBJECT test class declaration (one per area)
  test_backendclient.cpp           # 34 — result parsing, open response, image tiles
  test_catalogue_parser.cpp        # 35 — redshift/distance field detection, distances
  test_backend_routing.cpp         # 13 — Settings backend registry + SKAVA routing
  test_image_lod.cpp               # 24 — viewport LOD math (level select + visible tiles)
  test_analysis_param_store.cpp    #  8 — per-dataset dialog parameter persistence
  test_backend_contract.cpp        #  4 — shared header manifest ↔ client constants
  test_remote_slice_cache.cpp      # 11 — slice cache keying + eviction
  test_linked_window_registry.cpp  #  8 — linked-views registry lifetime
  test_spectral_unit_convert.cpp   # 25 — unit parsing + rest-frame → axis placement
  test_cube_region_geom.cpp        # 20 — ROI hit-testers (box/circle/polygon/annulus)
  test_cube_tool_gate.cpp          # 11 — CubeToolGate: which tools may act, and why not
  test_tool_result_record.cpp      #  8 — reopening a tool result exactly as it was
  test_product_origin.cpp          # 15 — where a measurement was taken (region / voxel)
  test_catalogue_csv.cpp           # 11 — quoting, multi-line records, truncated files
  test_fits_header_string.cpp      #  6 — header cards → 80-column FITS header string
  test_stokes_companion.cpp        #  9 — Stokes Q/U/V companion filename matching
  test_spectral_axis_infer.cpp     # 16 — spectral-axis descriptor inference
  test_kinematic_level_field.cpp   # 19 — kinematic-lasso level field
  test_ray_voxel_pick.cpp          # 18 — 3-D ray → voxel picking
  test_sed_builder.cpp             # 20 — SED from catalogue rows: band-merged groups, branches, flux columns, units
  test_vlkb_pos_string.cpp         # 11 — VLKB POS strings: longitude wrapping, the box across l = 0
```

Total: **326 tests** in one binary, as QTest counts them — each class
contributes its own `initTestCase` / `cleanupTestCase` to that figure.
`ctest -V` prints the per-class totals.

What belongs here: pure logic lifted out of the GUI classes so it can be
checked without a display — parsers, geometry, unit conversions, and decision
tables such as `CubeToolGate` (which tools may act on what is on screen) or
`SpectralUnitConvert` (where a rest-frame line lands on a given spectral axis).
Both of those were extracted precisely because the only previous way to check
them was to open the app and look at thirty buttons.

`ViewerModeState` (`test_viewer_mode_state.cpp`) is a smaller case of the same
idea. The image viewer's five modes are mutually exclusive, and the flags that
say so are cleared by four different code paths; the question the test pins is
what the mode strip must show if two of them ever disagree — the answer being
the mode that will consume the next click, which is `toggleProbeFreeze()`'s own
dispatch order. Getting that backwards (the first version did) shows the user a
tool they are not in, which is worse than showing nothing.

`SkyOverlap` (`test_sky_overlap.cpp`) is there for the same reason: whether a
FITS can be added as a layer to the image on screen is one interval comparison,
and the case that breaks it — a field straddling RA = 0, which `wcsrange()`
reports as `ra_min > ra_max` — cannot be reached from a GUI test. The rule is a
header with no FITS and no VTK in it, so the wrap, the touching edges and the
missing WCS solution are all checked directly.

The same reasoning drives `SedBuilder` (`test_sed_builder.cpp`): reading a SED
out of a catalogue is a pile of column-name rules, and a misread column does not
crash — it produces a fit that runs and is wrong. The rules therefore live in a
header with no widgets in it, where they can be checked directly.

The **backend** has its own pytest suite under `backend/tests/`. A few of its
files are worth knowing about when touching the science, the archive path or
the way files get opened:

- `test_sed_fit.py` (61) round-trips greybody fits against synthetic SEDs and
  checks the Δχ² = 1 uncertainty against its closed-form value;
- `test_arepo.py` (34) builds a synthetic AREPO snapshot and exercises the HDF5
  walk, the grid arithmetic and the FITS header — including the link cycles and
  the attribute types that used to break the reply;
- `test_soda_cutout.py` (20) covers what a VLKB cutout response has to be before
  it is called a FITS file;
- `test_vlkb_mcutout.py` (39) covers the batch job either side of the wire — the
  POS parsing, the job id, the report — and the parts that can do damage: path
  traversal out of the results directory, redirects that would carry the access
  token elsewhere, and an archive that unpacks to more than it downloaded;
- `test_classify.py` (26) pins what the single **Open…** does with each kind of
  file. Most of it is telling image, cube and table apart from the bytes — a
  length-1 Stokes axis must not make a map into a cube — and the rest is about
  refusing to guess: prose with commas is not a catalogue, a FITS table is
  ambiguous on purpose, and a compressed file is refused rather than routed to
  an opener that would reject it;
- `test_operation_history.py` (32) covers "Last 5 jobs": that work is recorded
  and that browsing, scrubbing and polling are not, and that a route answering
  HTTP 200 with `{"valid": false}` is filed as failed;
- `test_filter_fits.py` (38) is mostly about the three things the legacy's
  *Filter FITS* got wrong and that only surface later, in the photometry: a
  blank pixel eating its neighbourhood, a beam that no longer describes the map,
  and flux lost by block-averaging a per-pixel unit. It also pins the WCS
  arithmetic — the half-pixel terms in `CRPIX`, and the keywords of a squeezed
  Stokes axis going with it;
- `test_fits_header_edit.py` (35), `test_hips2fits.py` (33),
  `test_cone_search.py` (24) and `test_vlkb_tap.py` (27) cover the four archive
  and repair routes. The recurring theme is refusing *first*: a structural
  keyword, a 64-Mpx cutout, a radius of zero, a box of zero width — each with
  the reason, rather than a round trip and somebody else's error page. The other
  half is reading what services actually send: a VO error carried inside a valid
  VOTable, a TAP error returned as XML when CSV was asked for, an HTML gateway
  page where a FITS was expected;
- `test_hips_tile_query.py` (9) covers the two routes that decide whether the
  HiPS viewer shows anything — `query_tiles` and `catalogue_overlay`. Both had
  no test and both shipped dead: a router refactor left them calling a name the
  star-import does not re-export, and the surrounding `except Exception` dressed
  the `NameError` up as a plausible "HEALPix query failed", so the viewer drew
  an empty sky and nothing in CI noticed. The tests register a survey directly
  in the session registry, so no network is involved;
- `test_hips_registry.py` (19) covers the CDS survey registry behind the
  viewer's survey picker: that a record without a service URL is dropped, that
  one latin-1 byte in a title does not throw away the other 1500 records, and
  that the on-disk cache behaves as a cache — stale, damaged, unreadable and
  unwritable all fall back to the network rather than failing the request. It
  is a separate module from `test_hips_tile_query.py` on purpose: none of this
  needs healpy, and that one skips without it;
- `test_safe_fetch.py` (4) stands on its own because the bug it guards against
  has been fixed three times in this codebase and forgotten twice: a validated
  URL that redirects into the private network. It runs a real local server that
  answers 302, so the test fails if the redirect handler is ever dropped;
- `test_contract.py` pins the REST route list the desktop client depends on —
  **add an endpoint and it fails until you regenerate the snapshot**:

```
backend/.venv/bin/python backend/scripts/gen_contract.py
```

---

## Driving the real application (`tools/gui_smoke`)

The unit suites answer *is this function right*, and the backend suite answers
*is this route right*. Neither answers the question that keeps breaking: **does
the thing the user clicks reach the thing that was tested?** A menu entry wired
to nothing, a dialog opened on a window with no dataset, a result written to a
file nobody opens — all of those pass every test above.

`tools/gui_smoke/gui_smoke.py` launches the built application on a real FITS
file and clicks through thirty-eight scenarios across the image, cube, catalogue
and VBT viewers — moments, line-width, baseline subtraction, PV extraction, the
kinematic lasso, source finding, the exports, the regions, the products, the
cosmology selector, the shared docks — asserting on the backend's own job
history, on the files that appear in the workspace (including their NAXIS), and
on the labels the windows show. Never on a screenshot.

```bash
python3 tools/gui_smoke/gui_smoke.py --image 2d.fits --cube 3d.fits
```

The catalogue and VBT passes need no data of their own: the suite writes a
300-source RA/Dec/redshift CSV, a 2000-point VisIVO Binary Table and a 24³ VBT
volume into its artefacts directory unless `--catalogue` / `--vbt` name real
ones.

It is a **developer tool, not a test target**: it needs macOS, a display and
Accessibility permission, and CI has none of the three. `tools/gui_smoke/README.md`
documents the scenarios, the oracles, and the platform behaviour each helper
exists to work around.

It has already earned its keep several times over. Driving Compute Moment showed that no
`/v1/tasks/*` job ever appeared in the activity panel — the task API's terminal
status is `completed` and the registry snapshot recognised only `done`, so a
moment showed up neither as running nor as finished. The unit test that covered
the snapshot had used `done`, so nothing caught it. And arming a region from
Tools ▸ Regions turned out to do nothing at all while the ruler was armed: the
menu entry checked, the strip still saying Ruler, the drag drawing a distance.
And a file that is not a FITS — an IPAC table, a `.speck`, a CSV, a VisIVO
Binary Table — could not be opened from outside the application at all: the
desktop entry point never reached the classifier, so a double-click was answered
with "Unsupported file type. Expected FITS or HDF5" while the same file opened
fine from Open….

The same class of bug turned up once more in the Open dialog itself, reported by
a user rather than by the suite: its Open button was enabled only for a row the
listing had marked as FITS, so a VBT could be seen in the browser and not
opened — the button just stayed grey.

They are all the same shape — everything is wired, and the wire goes somewhere
else. None of them is reachable by a unit test, and all of them are one click
deep.

---

## Build

```bash
# Standalone — what CI runs. Needs only a base Qt6 (Core/Network/Test):
# no VTK, no Qt::Widgets, no WebRTC, so it configures in seconds.
cmake -S tests -B build-tests
cmake --build build-tests
ctest --test-dir build-tests -V

# Or as part of the main project:
cmake -B build -DBUILD_TESTING=ON   # off by default
cmake --build build --target VisIVOTests
ctest --test-dir build -V
```

---

## CI (`.github/workflows/tests.yml`)

Two jobs: **backend (pytest)** over `backend/tests/`, and **C++ unit tests
(ctest)** which configures the standalone `tests/` project against
`jurplel/install-qt-action`.

```{note}
Both Linux jobs first delete the runner image's unused third-party apt sources
(Google Chrome, Microsoft). GitHub's Ubuntu images ship them, nothing here
installs from them, and when one serves a stale index **every** `apt-get update`
on the runner fails with *"Hash Sum mismatch"* — including the one inside
`install-qt-action` — which fails the job for a reason that has nothing to do
with the repository. `release.yml` carries the same guard.
```

Running the backend suite locally needs `pip install -r
backend/requirements-dev.txt` (numpy / astropy / pytest); the C++ suite needs
nothing but Qt.

---

## `visivo_test_support` static library

Compiled once, linked by all test targets.

Contents:
- `BackendClient.cpp`
- `DiagnosticsManager.cpp`
- `Settings.cpp` — for the multi-backend registry tests
- `skava/BackendRouting.cpp` — the SKAVA → backend routing decision

Dependencies: `Qt::Core`, `Qt::Network` only.
Does **not** pull in Qt::Widgets, VTK, or libwcs.

`DiagnosticsManager.cpp` uses `#include <QCoreApplication>` (not `<QApplication>`) so it compiles without Qt::Widgets.

---

## `TestBackendClient` (30 tests)

All tests use `QJsonObject` literals — no network calls. Covers the static
`parse*Object` methods (moment, PV, noise, save/exports, **image tile**).

### `parseMomentResultObject`
| Test | What it checks |
|------|---------------|
| `parseMoment_happyPath` | all fields round-trip correctly |
| `parseMoment_errorResponse` | `valid=false`, `error` string preserved |
| `parseMoment_missingFields_defaults` | absent fields default to zero/empty |
| `parseMoment_wcsStatusDefaultsToOk` | absent `wcs_status` → `"ok"` |
| `parseMoment_wcsSanitized` | `wcs_status="sanitized"` + warning message |

### `parsePvResultObject`
| Test | What it checks |
|------|---------------|
| `parsePv_happyPath` | all fields round-trip |
| `parsePv_errorResponse` | `valid=false`; `error` cleared by design (mid-function clear) |
| `parsePv_widthPixelsDefault` | absent `width_pixels` → 1 (not 0) |
| `parsePv_beamFields` | `beam_major`, `beam_minor`, `beam_pa` |

### `parseNoiseResultObject`
| Test | What it checks |
|------|---------------|
| `parseNoise_happyPath` | region coords, channel range |
| `parseNoise_madSigmaArrays` | array values with float tolerance |
| `parseNoise_emptyArrays` | `mad.isEmpty()`, `sigma.isEmpty()` |
| `parseNoise_errorResponse` | `valid=false`, `error` preserved |
| `parseNoise_missingRegion_defaultsToZero` | absent region → all coords 0 |

**Note on `parsePv_errorResponse`**: `parsePvResultObject` calls `result.error.clear()` after the `positions_arcsec_base64` decode step (missing arcsec positions is non-fatal, and `error` is reused as a scratch buffer). The test therefore only asserts `!r.valid` and documents this behaviour.

### `parseImageTileObject`
| Test | What it checks |
|------|---------------|
| `parseImageTile_happyPath` | all tile fields (dims, level, num_levels, tile_x/y, range) round-trip |
| `parseImageTile_errorResponse` | `valid=false`, `error` preserved (e.g. out-of-range level) |
| `parseImageTile_missingFields_defaults` | `full_*` default to width/height, `num_levels`→1, `level`→0 |

---

## `TestCatalogueParser` (26 tests)

### `detectedRedshiftField`
| Test | Input schema | Expected result |
|------|-------------|----------------|
| `redshift_lowercase_z` | `z` | `"z"` |
| `redshift_uppercase_REDSHIFT` | `REDSHIFT` | `"REDSHIFT"` |
| `redshift_ZSPEC` | `Zspec` | `"Zspec"` |
| `redshift_ZMEAN` | `zmean` | `"zmean"` |
| `redshift_ZPHOT` | `Zphoto_z` | `"Zphoto_z"` |
| `redshift_absent` | `flux` only | `""` |
| `redshift_caseInsensitive_mixed` | `Redshift` | `"Redshift"` |
| `redshift_multipleAliases_firstInAliasListWins` | `redshift` + `z` | `"z"` (Z alias has priority) |
| `redshift_unrelated_fields_notMatched` | `flux_z`, `size_z` | `""` (normalise to FLUXZ/SIZEZ) |

### `detectedDistanceField`
| Test | Input | Expected |
|------|-------|---------|
| `distance_DISTANCE` | `distance` | `"distance"` |
| `distance_DIST` | `Dist` | `"Dist"` |
| `distance_DMPC` | `dMpc` | `"dMpc"` |
| `distance_absent` | no distance field | `""` |

### `comovingDistanceMpc`
| Test | Input z | Expected range |
|------|---------|---------------|
| `comoving_zeroReturnsZero` | 0.0 | 0.0 exactly |
| `comoving_negativeReturnsZero` | -1.0 | 0.0 exactly |
| `comoving_z01_reasonable` | 0.1 | 400–460 Mpc |
| `comoving_z1_reasonable` | 1.0 | 3000–3600 Mpc |
| `comoving_z01_gt_z005` | 0.05, 0.10 | D(0.10) > D(0.05) (monotonicity) |

### `entryDistanceMpc`
| Test | Setup | Expected |
|------|-------|---------|
| `entryDist_fromDistanceField` | `distance=500` | 500.0 |
| `entryDist_fromRedshiftField` | `z=0.1` | 400–460 Mpc |
| `entryDist_fallback300Mpc` | no distance/z field | 300.0 |
| `entryDist_distanceMpcOverride_takesPriority` | `distance=500`, `entry.distanceMpc=750` | 750.0 |
| `entryDist_negativeDistanceField_fallsBackToRedshift` | `distance=-100`, `z=0.05` | 150–280 Mpc |
| `entryDist_zeroRedshift_fallsBackTo300` | `z=0.0` | 300.0 (z=0 → Dc=0 → fallback) |

---

## `TestBackendRouting` (13 tests)

Covers the desktop side of the [distributed backend](distributed-backend-design)
model: the `Settings` multi-backend registry and the SKAVA → backend routing
decision (`pickBackendForSkavaDataset`, extracted into `src/skava/BackendRouting.cpp`
so it is testable without the GUI/VTK stack). Each test runs against a fresh
`Settings` on a `QTemporaryDir`.

### Settings backend registry
| Test | Assertion |
|------|-----------|
| `registryAlwaysHasLocal` | the auto-managed `local` node is always present (even on a fresh store) |
| `upsertInsertsAndUpdates` | `upsertBackendNode` inserts a new node, then updates in place (no duplicate) |
| `removeDropsNode` | `removeBackendNode` deletes a non-local node |
| `localCannotBeRemoved` | `removeBackendNode("local")` is a no-op — `local` is protected |
| `defaultBackendIdFallsBackToLocal` | default id is `local`; empty is coerced back to `local`; a real id sticks |

### `pickBackendForSkavaDataset`
| Test | Setup | Expected |
|------|-------|---------|
| `routeByExactUrl` | descriptor endpoint == registry URL | that node |
| `routeByUrlIgnoresTrailingSlash` | endpoint has a trailing `/` | still matches (normalised) |
| `routeByNodeCode` | endpoint differs, `node_code == srcCode` | that node |
| `routeNoMatchReturnsEmpty` | unknown URL + code | empty node (→ local-download fallback) |
| `routeEmptyDatalinkReturnsEmpty` | no `visivoBackends` | empty node |
| `routeNullSettingsReturnsEmpty` | `settings == nullptr` | empty node |

---

## Adding new tests

1. Create `TestFoo.h` with `class TestFoo : public QObject { Q_OBJECT private slots: … };`
2. Create `test_foo.cpp` with implementations
3. Add both files to `add_executable(VisIVOTests …)` in `tests/CMakeLists.txt`
4. Add `{ TestFoo t; status |= QTest::qExec(&t, argc, argv); }` to `tests/main.cpp`

If the new test needs additional source files from `src/`, add them to `visivo_test_support` (if they have no Qt::Widgets/VTK dependency) or create a second test support library.
