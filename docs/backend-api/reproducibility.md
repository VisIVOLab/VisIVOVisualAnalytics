# Reproducibility

Session history, provenance and signed snapshots
(`backend/app/routers/reproducibility.py`). All routes need the token and
work on the session named by `X-Visivo-Session`; history and snapshots are
held in memory with the session.

Compute routes append to the history on their own (through
`record_history`), so the history reflects the work actually done in the
session, not only the steps recorded by hand. Each entry is
`{timestamp, operation, endpoint, dataset_id, result_dataset_id, params}`.

## `GET /v1/reproducibility/snapshots`
The session's append-only operation history, oldest first:
`{valid, error, n_entries, history}`.

## `POST /v1/reproducibility/snapshots`
Append an entry by hand.

```json
{ "operation": "manual-mask", "dataset_id": "ds_…",
  "result_dataset_id": "", "params": { "note": "…" }, "timestamp": "" }
```

An empty `timestamp` is filled with the current UTC time. The response is the
whole history, as for `GET`.

## `GET /v1/reproducibility/provenance`
The session as a W3C PROV-JSON document: `{valid, error, prov}`. Each open
dataset is an `entity` (`visivo:<dataset_id>` with `visivo:path` and
`visivo:kind`), each history entry an `activity` (`prov:startTime`,
`visivo:operation`, `visivo:params`); `used` links an activity to its input
dataset, `wasGeneratedBy` a produced dataset to its activity, and
`wasDerivedFrom` a produced dataset to its source.

## `POST /v1/reproducibility/snapshot`
Freeze the session state into a named snapshot signed with HMAC-SHA256.
Request: `{label}`. Response: `{valid, error, snapshot_id, hmac, state}`,
`state` being `{label, created_at, datasets, history}` (datasets as
`{path, kind}` per id).

The signing key is `VISIVO_SNAPSHOT_SECRET` (or `ADMIN_SESSION_SECRET`);
without either, a random key is generated per process, so signatures do not
survive a restart.

## `GET /v1/reproducibility/snapshot/{snapshot_id}?provided_hmac=<hex>`
Return a snapshot: `{valid, error, snapshot_id, hmac, state}`. When
`provided_hmac` is given it must equal the stored signature, else 403. An
unknown id is 404.
