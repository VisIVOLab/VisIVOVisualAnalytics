# Shared selections

The selection hub of a session
([#188](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/188)): the
VR headset's page (`/vr/`) and the desktop's cube viewer, working in the same
backend session (the desktop's pairing link hands its session to the
headset), publish the Kinematic Lasso or box they make here, and each waits
for the other's with a long poll. Code: `backend/app/selection_hub.py`,
`backend/app/routers/share.py`.

Every route takes the token in `X-Visivo-Token` and the session in
`X-Visivo-Session`, as every dataset route does: only the clients of a session
see its selections (another token's session is a 404, as for its datasets).

**What is kept.** Per session and per cube, the **latest selection only**: a
new one replaces it, whichever side made it. *Latest* is the order in which
the publishes **arrive**: a selection's `seq` is taken when its request
arrives, before the backend measures it, and a selection never replaces one
with a higher `seq` (a high-water mark per cube that a clear or the cube's
closing leaves in place), so a slow earlier publish cannot overwrite a faster
later one, nor land after a clear (it is answered 409). The cube is the dataset's file,
not its id, so a headset that opened the desktop's path again under another
dataset id of the same session sees the same selection. A lasso's mask is kept
bit-packed over its bounding box (a 16 Mi voxel mask is 2 MiB), a box as its
corners; with it, the statistics measured on the full cube when it was
published (so both sides show the same numbers), which side and which client
made it, and the creator's record.

**Bounds.** A selection's box holds at most 16 Mi voxels (a 64 MiB float32
body); a session keeps at most 4 cubes' selections (the oldest goes); the
masks of all sessions together take at most 64 MiB (the oldest selections go
first); at most 8 long polls of a session wait at once (429 beyond). A
selection lives as long as its session and its cube: a session evicted after
its idle time (`VISIVO_SESSION_TTL`, 1800 s by default) takes its selections
along, and the selection of a cube closed in the session is dropped at the
session's next request to the hub. A waiting long poll keeps its session in
use, as any request does.

## `POST /v1/share/selection`

Publishes a selection. Query parameters: `dataset_id`; `kind` (`lasso`, the
default, or `box`); `source` (`headset` or `desktop`); `client` (the
publisher's own id, 1-64 characters, which it uses to ignore its echo);
`origin_x`, `origin_y`, `origin_z` (`>= 0`) and `width`, `height`, `depth`
(`>= 1`), the box in cube voxels; `meta`, the creator's record as a JSON
object (at most 6000 characters; `NaN` and `Infinity` are refused).

The body of a lasso is its mask as `kinematic_select_bin` returns it: raw
little-endian `float32` (a voxel is selected when its value is > 0.5, as
`mask_stats_bin` and `mask_apply_bin` read a mask; the count, the statistics
and the mask handed on all use this rule), x fastest, over
the box, so the grow's response body is posted on with the origin and
dimensions of its headers. A box has no body: every voxel of the box is
selected. The declared `Content-Length` is checked before the body is read and
the body is never read past the expected size. The mask is packed off the
event loop. For a box, a `meta.box` that is not the box given by the origin and
dimensions is refused (422): the box is what was measured.

The backend then measures the selection on the full cube, as
[`mask_stats_bin`](cube.md) does (in the session's compute slots: 429 when all
stay busy), and keeps it. When the statistics cannot be computed (a cube whose
spectral axis is not its third numpy axis) the selection is still kept, with
`stats.valid` false and `stats.error` saying why.

`meta`, as the two clients write it:

| Field | Written by | Meaning |
|---|---|---|
| `grow` | both, for a lasso | the request the mask was grown with, as `POST /v1/cube/kinematic_select_bin` takes it (`seed_z`, `seed_y`, `seed_x`, `reach`, `lambda_v`, `sigma`, `nsigma`, `roi_radius`, `reach_max`, and `add_seeds`, `sub_seeds`, `smooth_v`, `smooth_xy` when set) |
| `box` | the page, for a box | its range `[[x0, x1], [y0, y1], [z0, z1]]`, inclusive |
| `parameters` | both | the Provenance labels of the selection, as the creator shows them |
| `trim_box` | both | `> 0`: the grow's domain was cut to a seed-centred box of this half-width |
| `hand_edited` | the desktop | the mask was refined by hand on the 2-D map after the grow |
| `made_in` | both | `VR page` or `desktop` |
| `nonce` | both | a fresh random id per publish, echoed back: what a client compares with what it shows (see [How the clients follow the hub](#how-the-clients-follow-the-hub)) |

Response: `{"valid": true, "seq": <int>, "instance": <id>, "count": <selected voxels>, "stats": {…}}`.
`instance` names this backend process (the `seq`s begin again at 1 when it
restarts); every answer of the hub carries it.
`seq` increases over the whole backend, so a later selection always has a
larger one.

Errors: 409 `stale: …` when a publish that arrived later is already the cube's
selection (this one is not kept); 422 for a body of the wrong size, an empty
mask, a box outside the cube or over the voxel limit, a `meta` that is not a
JSON object or whose `box` is not the box, a `kind` or `source` not listed; 404
for an unknown dataset; 401 without the token.

## `GET /v1/share/selection`

The cube's latest selection. Query parameters: `dataset_id`; `after` (default
0): answer only with a selection whose `seq` is larger; `wait` (0 to 25
seconds, default 0): how long to wait for one. With `wait` the request is a
long poll: it answers as soon as a selection is published (within the round
trip of the publish), or with **204** when none came. Clients ask again with
the `seq` they last handled as `after`.

How the two clients use it is under
[How the clients follow the hub](#how-the-clients-follow-the-hub).

Response (JSON): `seq`, `kind`, `source`, `client`, `dataset_id` (the
publisher's), `origin` and `dims` (`[x, y, z]`, `[width, height, depth]`),
`full_shape` (`[width, height, depth]` of the cube), `count`, `age_s`, `instance`,
`stats` (the `mask_stats_bin` response, plus `valid` and `error`) and `meta`.

Errors: 429 when the session already has 8 polls waiting; 404 for an unknown
dataset; 401 without the token.

## `GET /v1/share/selection/mask`

The mask of the selection `seq` (query parameters `dataset_id`, `seq`), as
`kinematic_select_bin` answers: little-endian `float32` 0/1 over the box (a
box: all ones; unpacked off the event loop, once per selection however many
clients ask at once, at most two selections at once; the latest unpacked
mask, at most 64 MiB and not counted in the 64 MiB budget of packed masks, is
kept until its selection leaves the hub),
with the headers `X-Visivo-Width`, `-Height`, `-Depth`,
`-Origin`, `-Full-Shape`, `-Scalar-Type` (`float32`), `-Selected-Count`,
`-Bbox` (`x0,x1,y0,y1,z0,z1` of the selected voxels), `-Byte-Order`, plus
`X-Visivo-Selection-Seq` and `X-Visivo-Selection-Kind`. A client reads it as
it reads its own grow.

Errors: 404 when `seq` is no longer the latest (a newer one replaced it: the
next poll has it) or has expired.

## How the clients follow the hub

The VR page (`static/vr/js/share.js`) and the cube viewer
(`src/app/SharedSelectionLink`) follow one invariant: **what a client shows
converges to the hub's latest entry for its cube.** The rule is one pure
function on each side (`reconcileDecision` and `confirmDraw` in `share.js`,
`reconcileDecision` and `confirmShareDraw` in `src/app/SharedSelection.h`),
unit-tested on both against one set of cases,
`backend/tests/vr/share_rule_cases.json` (read by `backend/tests/vr/share.test.mjs`
and `tests/test_shared_selection.cpp`).

Each client remembers the nonce of what it shows (its own selection or a
received one), the nonce of its latest own selection, and a floor: the
entries at or below it were read and then replaced by a selection made
locally, or were already in the hub when the client started following it.
Publishing a selection makes it what the client shows and raises the floor to
every entry read so far; a local selection that is not published, or a Clear,
raises the floor too.

*Reconcile* reads the latest entry (`after=0&wait=0`) and:

| The latest entry | Action |
|---|---|
| none, unreadable (a seq, a known kind, origin, dims and full shape of three integers each), or its nonce is the one shown | nothing (the cursor moves past it; an unreadable one, or one whose mask does not match its description, is never asked for again) |
| this client's, but an older one | skip: a newer local one is about to replace it |
| this client's latest own one, not shown | draw it back (another one was drawn meanwhile) |
| another client's, at or below the floor, of another cube (compared only once the client knows its cube's shape), or with a box outside the cube or a negative origin | nothing |
| another client's, above the floor | fetch its mask, read the latest again, draw it only if it is still the latest and nothing local happened meanwhile |

It runs after every long-poll answer (a new entry, or the 20 s timeout, which
is the safety net), after every publish settles (kept, refused 409, failed,
timed out after 30 s, or a 5xx after the server may have kept it) and before
anything received is drawn. The long poll never waits on a publish; reconcile
settles the order. Every request has a time limit (a publish and a read of the
latest entry 30 s, a mask 120 s, the long poll its wait plus 10 s), and
stopping the link aborts what is in flight.

**The exception: a publish the hub refuses for good.** A 4xx other than 409
(for example 422 for a selection over the voxel limit) means the hub will
never hold it: the selection stays on screen, local; the client says *not
shared:* with the reason, and it is no longer the client's "latest own" (so
its older own entry in the hub is skipped, not drawn back). Nothing else
changes: an entry another client publishes later still replaces it. A 429
(the session's compute slots busy) is retried once after 1 s, then treated
the same way. The retry is sent only if, after that second, the selection is
still the client's latest own (its nonce) and the client follows the same cube
in the same link: otherwise it is dropped, since a late retry would take a new
seq and replace the newer selection in the hub, leaving the other clients on
the older one.

**A backend restart.** When an answer names another `instance`, the client
forgets its cursor, floor and nonces (they counted the old instance's seqs)
and follows the new one from scratch. The cube viewer's link also restarts
whenever its window starts it for another dataset or session. The first read of a client takes the hub's entry as its
cursor and floor without drawing it. A client treats only a 405 or FastAPI's
bare `{"detail": "Not Found"}` as a backend without the hub; a 404 for an
unknown session or cube is retried with a growing delay (a reload or a new
pairing recovers it). The page offers a selection that arrives before its cube
is drawn again a second later.

For example, the desktop's B arrives at the hub first (seq 6) and the page's A
second (seq 7), but B is measured first: the page, which shows A, reads B as
the latest and fetches it; when it reads again A is the latest, its own and on
screen, so B is not drawn; the desktop draws A. Had A been refused (409), both
would end on B.

## `DELETE /v1/share/selection`

Forgets the cube's selection (`dataset_id`), and refuses every publish for
that cube already under way when the clear arrived, with 409 and a detail
starting `cleared:` (a later selection gives `stale:`): none brings a cleared
selection back. The clients treat a `cleared:` 409 like a refusal: the
selection stays on screen, local, with a note. Response `{"cleared": true}` when
there was one. Waiting polls are not answered by a clear.

## Timings

Backend on an Apple M5 Max, HCG 44 (700 × 700 × 158), the page in headless
Chrome on the same Mac (`validation/vr_share_e2e.mjs`): publishing NGC 3187's
lasso (13 216 voxels) with its statistics takes 45-94 ms (three runs); a
desktop waiting on the long poll has the description and the mask 48-98 ms
after the page's lasso ended. A desktop lasso of 10 684 voxels is published in 46 ms and drawn by the
page 7 ms later.
