# Service Mapping Note

## Purpose
This note captures the current service boundaries and the intended future mapping toward a cleaner remote-authoritative architecture.

It documents the current state and the minimum gaps that still exist before a cleaner client/backend split.

## `DatasetOpenService`

### Current contract
- `DatasetOpenRequest`
- `DatasetOpenInfo`
- `DatasetOpenService::inspect(const DatasetOpenRequest &request)`

### Current local implementation
- local inspection service in `visivo_shared_core`
- input is currently a local desktop FITS file path
- implementation uses `AstroUtils` to classify the dataset as image or cube

### Future remote equivalent
- inspect a remote dataset reference or catalog resource id
- return the same kind of inspection result needed by the client to decide how to open the dataset

### Missing step before clean split
- clarify the future request identity model beyond local file paths
- keep the client dependent on the contract, not on local inspection assumptions

## `ImageLayerImportService`

### Current contract
- `ImageLayerImportRequest`
- `ImageLayerImportResult`
- `ImageLayerImportService::inspect(const ImageLayerImportRequest &request)`

### Current local implementation
- local validation service in `visivo_shared_core`
- request currently carries:
  - local desktop path of the base dataset
  - local desktop path of the candidate image layer
- implementation uses `AstroUtils` to validate overlap and image compatibility

### Future remote equivalent
- validate a base dataset reference against a candidate layer reference
- return acceptance/error information needed by the client import flow

### Missing step before clean split
- replace path-based request semantics with stable dataset/layer references
- keep validation result small and client-oriented

## `MomentProcessingService`

### Current contract
- `MomentRequest`
- `MomentResult`
- `MomentProcessingService::computeMoment(const MomentRequest &request)`

### Current implementation
- client-side service facade in `visivo_shared_vtk`
- request carries backend-facing identifiers and moment parameters
- authoritative scientific computation happens on the backend
- result carries image payload plus scientific metadata needed by the client UI

### Current remote-authoritative contract
- submit or execute moment-map computation through the backend API
- return a portable processing result that the client applies locally for display
- require a backend-visible dataset identity rather than assuming the backend can open a desktop-local file path directly

### Missing step before cleaner remote-only contract
- add explicit upload/staging for desktop-local datasets so they can become backend-visible datasets
- keep the compute contract dataset-id-based and backend-authoritative

## Non-Goals
This note does not imply that any of the following already exist:
- remote rendering
- remote jobs
- async execution
- generic service interfaces
- widget rewrites

## Summary
Today the project already has three meaningful local service boundaries.

The best current pilot for a future local/remote dual implementation is `DatasetOpenService`, because it already has the cleanest contract and the lowest runtime coupling.

`ImageLayerImportService` is the next closest candidate.

`MomentProcessingService` is now already backend-authoritative. The main missing step is not scientific decoupling anymore, but explicit dataset staging/upload for desktop-local files so the same remote-only contract can cover local desktop usage cleanly.
