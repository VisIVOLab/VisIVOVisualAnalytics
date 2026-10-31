# Exports

The Workspace Exports directory is the backend-side folder where products are
written. It is one folder shared by the whole server, so the routes that
list, download or delete its files need the operator token once per-user
tokens are configured (403 for another token); `copy_dataset` and
`save_map_2d` need any valid token and the dataset's session.

## `GET /v1/exports/list`
Every regular file in the Workspace Exports directory, newest first:
`{valid, error, workspace_dir, entries}`, each entry
`{filename, size, modified_time, absolute_path}` (`modified_time` in ISO 8601
UTC). Files in sub-directories are not listed.

## `GET /v1/exports/download?filename=<name>`
Stream one Workspace file back to the client (`FileResponse` with the file
name set; the media type is always `application/fits`). The name is resolved
inside the Workspace: a name that escapes it is 400, a missing file 404.

## `POST /v1/exports/copy_dataset`
Copy the source file of a dataset open in the caller's session into the
Workspace, for datasets that have no dedicated export path.

```json
{ "dataset_id": "...", "output_basename": "" }
```

An empty `output_basename` keeps the source file name; a collision gets a
numeric suffix. Response: `{valid, error, workspace_filename, output_path}`.
An unknown dataset or a missing source file is `valid: false`.

## `DELETE /v1/exports/{filename}`
Remove one Workspace file. Response: `{valid, error, deleted_filename}`.
Idempotent: a file that does not exist, or a name outside the Workspace,
answers HTTP 200 with `valid: false`.

## `POST /v1/exports/save_map_2d`
Persist a 2-D map the CLIENT computed as a standalone FITS in the workspace.

```json
{
  "dataset_id": "...",
  "output_basename": "field_faraday_depth.fits",
  "width": 512, "height": 512,
  "pixels_base64": "…",
  "bunit": "rad m-2",
  "history": ["RM synthesis, φ -80…80 step 1", "Q from FIELD_Q.fits"],
  "region_origin": [x0, y0]
}
```

The generic export path. `/v1/cube/save_channel_2d` and the moment exporter
re-derive their pixels from a recipe, which only works for products the service
can recompute; everything else the viewer produces — polarised intensity,
position angle, Faraday depth, line-width — is finished pixels and leaves
through here.

The header is built service-side from `dataset_id`, so every export gets the
same treatment: `NAXIS=2` with the celestial axes only, `OBJECT` / `TELESCOP` /
beam carried over, `bunit` overriding the source's, and `history` written as
`HISTORY` cards. `region_origin` shifts `CRPIX` for a cropped map.

Returns `{valid, new_dataset_id, workspace_filename, output_path, width,
height, range_min, range_max}`; the file is registered as a session dataset, so
it can be reopened without going through the filesystem.
