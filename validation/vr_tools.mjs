// The VR page's analysis tools (issue #191), run by node for the identity
// check (vr_analysis_identity.py): the same modules the headset loads.
//
// One JSON request on stdin, one JSON answer on stdout:
//   {op: "seeds", steps: [{mode, seed: [x, y, z]}], params} -> {seeds, request}
//       the seeds and the grow request after each trigger press, in order
//   {op: "apply", request, datasetId, origin, dims, mode, trimBox, count, file}
//       -> {query, step, params}: the mask_apply_bin call of Isolate/Remove
//   {op: "momentRequest", order, depth} -> {request}
//   {op: "moment", order, depth, response: <file of the route's JSON>, bbox,
//    out: <file>} -> {request, params, replay, box, range, unit, n}; writes the
//       decoded map (float32, row-major) to `out`
//   {op: "pvVertex", cube, dims, encoding, info, ray, thr, mode} -> {vertex, p}
//       the vertex a trigger press adds (the pick, else the ray's middle)
//   {op: "pvRequest", vertices, width} -> {request}
//   {op: "pv", vertices, width, response: <file>, info, out} -> {request, params,
//       origin, order, velocities, samples, depth}; writes the rows bottom-up
//       (float32, depth × samples) to `out`
import { readFileSync, writeFileSync } from 'node:fs';
import * as S from '../backend/app/static/vr/js/selection.js';
import * as V from '../backend/app/static/vr/js/volume.js';
import * as TL from '../backend/app/static/vr/js/tools-lasso.js';
import * as TM from '../backend/app/static/vr/js/tools-moment.js';
import * as TP from '../backend/app/static/vr/js/tools-pv.js';
import * as PC from '../backend/app/static/vr/js/product-canvas.js';

const req = JSON.parse(readFileSync(0, 'utf8'));
let out;
if (req.op === 'seeds') {
  let seeds = null;
  const steps = [];
  for (const s of req.steps) {
    const [x, y, z] = s.seed;
    seeds = TL.nextSeeds(seeds, s.mode, [z, y, x]);
    steps.push({ seeds, request: TL.growRequest(seeds, req.params || {}) });
  }
  out = { steps };
} else if (req.op === 'apply') {
  out = {
    query: TL.applyQuery(req.datasetId, req.origin, req.dims, req.mode, TL.historyRecipe(req.request, req.trimBox || 0)),
    step: TL.applyStep(req.mode),
    params: TL.applyParams(req.request, req.mode, req.count, req.trimBox || 0, req.file),
  };
} else if (req.op === 'momentRequest') {
  out = { request: TM.momentRequest(req.order, req.depth) };
} else if (req.op === 'moment') {
  const request = TM.momentRequest(req.order, req.depth);
  const r = JSON.parse(readFileSync(req.response, 'utf8'));
  const values = PC.f32FromBase64(r.data_base64);
  writeFileSync(req.out, Buffer.from(values.buffer, values.byteOffset, values.byteLength));
  const box = TM.cropBox(req.bbox, r.width, r.height);
  const cut = PC.crop(values, r.width, box[0], box[1], box[2], box[3]);
  out = { request, params: TM.momentParams(request, r.moment_unit, req.file || ''), replay: TM.momentReplay(req.grow || {}, request),
    box, range: TM.colourRange(req.order, cut), unit: r.moment_unit, display: PC.displayUnit(r.moment_unit), n: values.length };
} else if (req.op === 'pvVertex') {
  const b = readFileSync(req.cube);
  const data = new Uint8Array(b.buffer, b.byteOffset, b.byteLength);
  const info = req.info;
  const vol = { data, dims: req.dims, encoding: req.encoding, texScale: V.texScale(info.full_shape, info.factors, info.reduced_shape) };
  const r = S.pickRay(vol, req.ray.o, req.ray.d, { thr: req.thr, mode: req.mode || 0 });
  let p = r ? r.hit.p : null;
  if (!p) {
    const span = S.rayBox(req.ray.o, req.ray.d, [0, 0, 0], [1, 1, 1]);
    if (span) p = [0, 1, 2].map((i) => req.ray.o[i] + req.ray.d[i] * 0.5 * (span.tn + span.tf));
  }
  out = { p, picked: !!r, vertex: p ? TP.boxToVertex(p, info.full_shape) : null };
} else if (req.op === 'pvRequest') {
  out = { request: TP.pvRequest(req.vertices, req.width) };
} else if (req.op === 'pv') {
  const request = TP.pvRequest(req.vertices, req.width);
  const r = JSON.parse(readFileSync(req.response, 'utf8'));
  const d = await TP.decodePv(r);
  const order = TP.rowOrder(d.spectral);
  const rows = new Float32Array(d.values.length);
  for (let i = 0; i < d.depth; i++) rows.set(d.values.subarray(order[i] * d.samples, (order[i] + 1) * d.samples), i * d.samples);
  writeFileSync(req.out, Buffer.from(rows.buffer));
  // What the page draws: its colourise output for these rows (canvas row 0 at the top).
  const range = PC.finiteRange(d.values) || [0, 1];
  const table = PC.lut('viridis');
  if (req.rgbaOut) writeFileSync(req.rgbaOut, Buffer.from(PC.colourise(d.values, d.samples, d.depth, range, table, order)));
  out = { request, params: TP.pvParams(req.vertices, request.width_pixels), origin: TP.pvOrigin(req.vertices, request.width_pixels),
    order, range, lut: Array.from(PC.lut('viridis')), velocities: TP.rowVelocities(order, req.info.axes.z.full_velocity_kms), samples: d.samples, depth: d.depth,
    positionUnit: d.positionUnit, positionsFirstLast: [d.positions[0], d.positions[d.samples - 1]] };
} else {
  throw new Error(`unknown op ${req.op}`);
}
process.stdout.write(JSON.stringify(out));
