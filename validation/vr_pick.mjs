// The VR page's own selection code, run by node for the identity check
// (vr_selection_identity.py): the same modules the headset loads, no copy.
//
// Reads one JSON request on stdin and writes one JSON answer on stdout:
//   {op: "pick", cube: <file of the reduced_bin body>, dims, encoding, info
//    (reduced_info), ray: {o, d} (box coordinates), thr (normalised), mode,
//    seedAt: "peak" | "hit"}
//     -> {hit, peak, picked (full pixel), reduced, block}
//   {op: "brightest", values: <file of float32>, dims, origin} -> {seed}
//   {op: "request", seed: [x, y, z], params} -> the lasso and probe requests
//   {op: "reduceMask", mask: <file of float32>, dims, origin, factors, reducedShape}
//     -> {count, dims, origin}
//   {op: "box", a, b, fullShape, rayCorners, zClip} -> {range, steps} (the call)
import { readFileSync } from 'node:fs';
import * as S from '../backend/app/static/vr/js/selection.js';
import * as Rc from '../backend/app/static/vr/js/recipe.js';
import * as V from '../backend/app/static/vr/js/volume.js';

const req = JSON.parse(readFileSync(0, 'utf8'));
const f32 = (file) => { const b = readFileSync(file); return new Float32Array(b.buffer, b.byteOffset, b.byteLength / 4); };
let out;
if (req.op === 'pick') {
  const b = readFileSync(req.cube);
  const data = req.encoding === 'float16'
    ? new Uint16Array(b.buffer, b.byteOffset, b.byteLength / 2) : new Uint8Array(b.buffer, b.byteOffset, b.byteLength);
  const info = req.info;
  const vol = { data, dims: req.dims, encoding: req.encoding,
    texScale: V.texScale(info.full_shape, info.factors, info.reduced_shape) };
  const r = S.pickRay(vol, req.ray.o, req.ray.d, { thr: req.thr, mode: req.mode || 0 });
  if (!r) out = { hit: null };
  else {
    const at = req.seedAt === 'hit' ? r.hit : r.peak;
    const picked = S.boxToFullPixel(at.p, info.full_shape);
    const reduced = S.reducedVoxelOf(picked, info.factors);
    out = { hit: r.hit, peak: r.peak, picked, reduced,
      block: S.blockOfReduced(reduced, info.factors, info.full_shape),
      firstHitPixel: S.boxToFullPixel(r.hit.p, info.full_shape) };
  }
} else if (req.op === 'brightest') {
  out = { seed: S.brightestInBlock(f32(req.values), req.dims, req.origin) };
} else if (req.op === 'request') {
  const [x, y, z] = req.seed;
  out = { lasso: Rc.lassoGrowRequest([z, y, x], req.params || {}), probe: Rc.probeRequest(x, y),
    replay: Rc.lassoReplay(Rc.lassoGrowRequest([z, y, x], req.params || {})) };
} else if (req.op === 'reduceMask') {
  const r = S.reduceMask(f32(req.mask), req.dims, req.origin, req.factors, req.reducedShape);
  out = { count: r.count, dims: r.dims, origin: r.origin };
} else if (req.op === 'box') {
  // The page's box from two corners, and the call that measures it.
  const range = req.rayCorners
    ? S.boxFromRayCorners(req.a, req.b, req.fullShape, req.zClip)
    : S.boxFromCorners(req.a, req.b, req.fullShape);
  out = { range, steps: Rc.boxReplay(range) };
} else {
  throw new Error(`unknown op ${req.op}`);
}
process.stdout.write(JSON.stringify(out));
