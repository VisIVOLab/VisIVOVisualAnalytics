# Architecture Note

## Purpose
This note summarizes the current architecture after the recent local refactorings, backend integration, and build modularization.

It describes what is already true in the codebase today, which boundaries are now stable, and which parts are the best candidates for a future local/remote split.

This is not a full backend design document. It focuses on the current client/backend split that is already implemented in the codebase today.

## Current Module Map

### `visivo_shared_core`
Local non-UI application/core logic.

Current contents include:
- `DatasetOpenService`
- `DatasetOpenRequest`
- `DatasetOpenTypes`
- `ImageLayerImportService`
- `ImageLayerImportRequest`
- `ImageLayerImportResult`
- `AstroUtils`

Main role:
- dataset inspection/classification
- image-layer import validation
- scientific/file-level checks that do not require Qt widgets or VTK rendering widgets

### `visivo_shared_vtk`
VTK/runtime and client-side rendering support logic.

Current contents include:
- `ImageLayerSet`
- `ImageLayer`
- `MomentProcessingService`
- `vtkFITSReader`
- `vtkFITSWriter`
- `vtkLegendScaleActorWCS`
- `vtkInteractorStyleProfile`
- `ColorMaps`

Main role:
- local VTK runtime support
- client-side rendering/visual processing tied to VTK objects
- image layer runtime state

### Client GUI
Qt widget layer and UI orchestration.

Current contents include:
- `MainWindow`
- `vtkWindowImage`
- `vtkWindowCube`
- `DatasetWindowFactory`
- `ImageLayerController`
- `CubeViewController`
- dialogs and widget-only helpers

Main role:
- file dialogs, menus, actions, UI updates
- render-window orchestration
- widget lifecycle and interaction flow

## Stable Local Boundaries

The following local boundaries are now explicitly present in the code:

### Dataset opening
- `DatasetOpenRequest`
- `DatasetOpenService`
- `DatasetOpenInfo`

The GUI no longer owns dataset classification logic directly.

`DatasetOpenService` should currently be read as a local dataset inspection service, not as a generic remote-ready implementation.

### Image layer import validation
- `ImageLayerImportRequest`
- `ImageLayerImportService`
- `ImageLayerImportResult`

`vtkWindowImage` still owns the UI flow, but import validation is outside the widget.

`ImageLayerImportService` should currently be read as a local image-layer validation service, not as a transport-neutral validation API.

### Moment map processing
- `MomentMapRequest`
- `MomentMapResult`
- `MomentProcessingService`

`vtkWindowCube` still owns UI orchestration, but user-facing moment computation is delegated to the backend-authoritative processing path.

`MomentProcessingService` should currently be read as a client-side facade over the backend/API processing path, not as a local scientific implementation.

### Image layer runtime state
- `ImageLayerSet`

`LayerListModel` no longer owns the full non-Qt layer management logic.

## Local Async Paths

The project now includes three real async or staged client paths:

- moment-map recomputation on moment-order change in `vtkWindowCube`
- image-layer loading for additional layers in `vtkWindowImage`
- preview-first cube open in `vtkWindowCube`

Common pattern:
- compute/load runs off the UI thread using `QtConcurrent::run` and `QFutureWatcher`
- the worker uses its own data and VTK objects and does not touch the visible pipeline
- the UI thread receives a result payload and applies it through a dedicated apply seam
- UI actions are temporarily disabled while the operation is running
- window closing is blocked while the async operation is still in progress
- display-side `vtkTrivialProducer` bridges are used where the visible pipeline must be fed from externally prepared data

### Async moment-map recomputation
Current design:
- the worker submits or polls backend-side moment-map computation through `MomentProcessingService`
- the backend-authoritative path is used for both remote datasets and local desktop datasets that have already been staged/opened on the backend
- the UI thread applies the result through `applyMomentMapResult(...)`
- the visible moment pipeline is fed through a display-side bridge (`vtkTrivialProducer`)

### Async image-layer loading
Current design:
- the worker loads and prepares the layer payload through `loadImageLayer(...)`
- the payload includes image data and placement metadata needed to build the visible layer without rereading the FITS file in the UI thread
- the UI thread applies the result through `applyLoadedLayer(...)`

### Preview-first cube open
Current design:
- initial cube open in `vtkWindowCube` first tries a synchronous downsampled preview
- the preview downsamples X/Y and keeps the full Z axis
- the visible cube and slice pipeline is fed through `cubeDisplaySource`
- the visible moment pipeline starts from a placeholder and is updated only by an explicit backend-computed moment result
- both preview and full-resolution data are applied through `applyCubeOpenResult(...)`
- if the preview succeeds, a full-resolution cube load starts in the background and replaces the preview when ready
- the UI exposes a persistent state indicator during this flow with explicit states such as preview, loading full resolution, and applying full resolution
- the preview-to-full replacement also performs geometry synchronization:
  - clamp of the current slice index
  - immediate update of `slice` and `sliceOnCube`
  - camera and clipping-range reset after replacement
- if the preview cannot be built, `vtkWindowCube` falls back to the older synchronous full open path

Current limitations:
- these async or staged paths are intentionally use-case-specific
- they do not form a general async/job framework
- cancellation, progress reporting, and queued execution are not implemented
- the final preview-to-full replacement can still introduce a small visible lag
- preview and full-resolution state can temporarily diverge while the full cube is still loading
- any new async use case must still define its own explicit separation between worker-side state and UI/display-side apply

## Current Responsibilities

### Shared Core
Owns:
- explicit request/result-based local services
- dataset/file validation rules
- non-UI logic that may later map to service contracts

Does not own:
- widget flow
- render windows
- VTK rendering orchestration

### Shared VTK
Owns:
- VTK-side processing and helpers
- image layer runtime objects
- client-side service facades and helpers that bridge backend results into VTK display objects

Does not own:
- Qt widget behavior
- menus, dialogs, action wiring

### Client GUI
Owns:
- widget wiring
- action handling
- render triggers
- UI state updates
- choosing when to call local services

Does not need to own:
- dataset classification rules
- image-layer import validation rules
- moment-map scientific computation rules

## Future Local/Remote Candidates

The best candidates for a future local/remote split are:

### `DatasetOpenService`
Reason:
- already has explicit request/result shape
- behavior is local and bounded
- good future candidate for remote dataset inspection or catalog-backed open flows

Current semantics:
- `DatasetOpenRequest.filepath` is currently a local desktop file path
- the current implementation is a local inspection service backed by `AstroUtils`

Future mapping:
- local implementation: inspect a local FITS path
- remote implementation: inspect a dataset reference or remote resource identifier
- the widget/client should not depend on how inspection is performed

### `ImageLayerImportService`
Reason:
- already has explicit request/result shape
- validates compatibility between a base dataset and a candidate layer
- good future candidate for remote validation

Current semantics:
- `ImageLayerImportRequest.baseDatasetPath` is currently a local desktop path for the base dataset
- `ImageLayerImportRequest.layerFilepath` is currently a local desktop path for the candidate image layer
- the current implementation is a local validation service backed by `AstroUtils`

Future mapping:
- local implementation: validate overlap and image compatibility using local FITS/WCS inspection
- remote implementation: validate a base dataset reference against a candidate layer reference
- the widget/client should not depend on how validation is performed

### `MomentProcessingService`
Reason:
- already has explicit request/result shape
- now fronts the single authoritative backend moment implementation
- is the clearest example of the client acting as a rendering/orchestration shell over backend science logic

Current semantics:
- `MomentProcessingService` is a client-side API/service facade
- backend-visible `datasetId` is required for authoritative moment computation
- client-local desktop paths are not treated as directly backend-openable in the normal flow
- `MomentMapResult` carries backend-derived image payload plus scientific metadata for UI/provenance display

Constraint today:
- a local desktop dataset must first become backend-visible through dataset open/staging before moment computation is supported
- the future upload/staging pipeline is not implemented yet

Future step needed:
- add an explicit staging/upload flow that yields backend dataset identifiers for desktop-local files
- keep the compute contract backend-authoritative and dataset-id-based

## Non-Goals / Deferred Items

The following are intentionally not implemented yet:

- no general dataset upload/staging pipeline for desktop-local files
- no remote rendering
- no general async/job framework beyond the current moment-map, image-layer, and cube-open use cases
- no generic service interfaces
- no large widget rewrite
- no full decomposition of `vtkWindowCube`
- no attempt to make all VTK runtime code backend-ready today

These are deferred on purpose. The current focus has been to create stable local boundaries first.

## Architecture Decisions Already Taken

### 1. Service logic moves out of widgets and backend-authoritative science moves out of the client
We first extracted local services and controllers from GUI classes, then made moment computation backend-authoritative instead of maintaining two scientific implementations.

### 2. Build modularization follows code boundaries
The build now reflects the code structure with:
- `visivo_shared_core`
- `visivo_shared_vtk`
- final GUI executable

### 3. Request/result contracts are preferred for stable service boundaries
Where useful, local services now expose explicit request/result types instead of ad-hoc primitive inputs.

### 4. GUI remains the orchestration shell
Widgets still own:
- file dialogs
- action triggers
- render calls
- UI updates

Services own bounded non-UI logic, and scientific computation that must be authoritative belongs on the backend side.

## Recommended Next Milestone

Define and document backend-ready service contracts for the existing local services, without implementing backend or async yet.

Practical next step:
- keep `DatasetOpenService`, `ImageLayerImportService`, and `MomentProcessingService` as the reference service boundaries
- document their intended local/remote mapping
- avoid further widget refactors unless they directly improve those boundaries

## Summary

The project is still a local Qt/VTK desktop application.

What changed is that several previously embedded pieces of logic are now explicit local boundaries with clearer ownership. This is the main architectural foundation for any future evolution toward async execution or client/server separation.
