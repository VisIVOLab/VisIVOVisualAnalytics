# Testing
 
## Suite overview

QTest-based headless unit suite under `tests/`. No VTK, no Qt::Widgets, no live backend required.

```
tests/
  CMakeLists.txt          # visivo_test_support + VisIVOTests
  main.cpp                # QCoreApplication + QTest::qExec for each class
  TestBackendClient.h     # Q_OBJECT test class declaration
  test_backendclient.cpp  # 14 tests
  TestCatalogueParser.h
  test_catalogue_parser.cpp  # 26 tests
  TestBackendRouting.h
  test_backend_routing.cpp   # 13 tests — Settings backend registry + SKAVA routing
```

Total: **53 tests**.

---

## Build

```bash
cmake -B build -DBUILD_TESTING=ON   # off by default
cmake --build build --target VisIVOTests
ctest --test-dir build -V
# or run directly:
build/tests/VisIVOTests -v2
```

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

## `TestBackendClient` (14 tests)

All tests use `QJsonObject` literals — no network calls.

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
