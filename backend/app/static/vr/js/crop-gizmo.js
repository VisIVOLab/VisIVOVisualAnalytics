// The spatial crop as something to see and grab (issue #277). While the
// Spatial crop tool is on, the crop box is drawn (its edges and a faint fill)
// with a handle on each of its six faces. The handle under the ray (or
// nearest the controller) lights up before the trigger is pressed; dragging
// it moves that face. Dragging the inside of a crop moves the whole crop;
// dragging anywhere else (or anywhere on a box not yet cropped) draws a new
// crop from two corners, as Box region does in Select: across the two axes
// facing the ray, the box's depth along the third kept as it was.
//
// The crop is the page's state.clip, {min, max} as fractions 0..1 of the box
// on show: the cube's voxels (x, y and the velocity axis, which is the
// velocity range), or a point table's or velocity field's bounds (culled in
// their shaders). A scene's frame maps those fractions to model space,
// model = a + t·b per axis, so one set of maths serves all three.
//
// Pure (rays in model space, the model a similarity), unit-tested by node
// (backend/tests/vr/crop-gizmo.test.mjs); main.js draws what crops() says.

import * as M from './mat4.js';
import { moveFace } from './controls.js';

/** The thinnest a crop may be on an axis (fraction of the box). */
export const GAP = 0.01;
/** A new crop narrower than this on a drawn axis is not one: a click, not a drag. */
export const MIN_NEW = 0.02;
/** A handle's dot, its diameter in metres in the world; it lights up within HIT_M of the ray. */
export const HANDLE_M = 0.03;
export const HIT_M = 0.045;
/** The controller's tip this close to a handle (metres) lights it up too. */
export const TIP_M = 0.06;

/** The six faces: {axis, end}, x min first. */
export const FACES = [0, 1, 2].flatMap((axis) => ['min', 'max'].map((end) => ({ axis, end })));
export const faceId = (f) => `${'xyz'[f.axis]}-${f.end}`;

/** A cube's frame: box fractions over its model-space extent, centred. */
export function cubeFrame(extent) {
  return { a: extent.map((e) => -e / 2), b: [...extent] };
}
/** A point table's or velocity field's frame: fractions of its bounds
 *  {lo, hi, centre, side}, drawn as (p - centre) / side. */
export function sceneFrame(box) {
  return { a: [0, 1, 2].map((i) => (box.lo[i] - box.centre[i]) / box.side), b: [0, 1, 2].map((i) => (box.hi[i] - box.lo[i]) / box.side) };
}
export const toModel = (f, t) => t.map((v, i) => f.a[i] + v * f.b[i]);
export const toFrac = (f, p) => p.map((v, i) => (f.b[i] ? (v - f.a[i]) / f.b[i] : 0));

const copy = (clip) => ({ min: [...clip.min], max: [...clip.max] });
const clamp01 = (v) => Math.max(0, Math.min(1, v));

/** Is `clip` less than the whole box? */
export function isCropped(clip) {
  return !!clip && [0, 1, 2].some((i) => clip.min[i] > 0 || clip.max[i] < 1);
}

/** The handles: each face's centre (fractions), with its face and id. */
export function handlePoints(clip) {
  return FACES.map((face) => {
    const p = [0, 1, 2].map((i) => (clip.min[i] + clip.max[i]) / 2);
    p[face.axis] = clip[face.end][face.axis];
    return { face, id: faceId(face), p };
  });
}

/** A world ray ({o, d}) in the model space of `model` (a similarity: the
 *  parameter along the ray is the same in both). */
export function modelRay(model, ray) {
  const inv = M.invert(model);
  const o = M.transformPoint(inv, ray.o);
  const e = M.transformPoint(inv, M.add(ray.o, ray.d));
  return { o, d: M.sub(e, o) };
}

/** [enter, exit] of the ray o + s·d through the box lo..hi, or null. */
export function rayAabb(o, d, lo, hi) {
  let t0 = -Infinity, t1 = Infinity;
  for (let i = 0; i < 3; i++) {
    if (Math.abs(d[i]) < 1e-12) {
      if (o[i] < lo[i] || o[i] > hi[i]) return null;
      continue;
    }
    let a = (lo[i] - o[i]) / d[i], b = (hi[i] - o[i]) / d[i];
    if (a > b) [a, b] = [b, a];
    t0 = Math.max(t0, a);
    t1 = Math.min(t1, b);
    if (t0 > t1) return null;
  }
  return [t0, t1];
}

/**
 * What the ray points at on the crop: {kind: 'face', face, id, t, point} (a
 * handle within HIT_M of the ray, the nearest along it; or, failing that, a
 * handle within TIP_M of the controller's tip), {kind: 'inside', t, point}
 * (the crop box, when it is cropped across the ray, so that it can move
 * there), or {kind: 'new'} (anything else, and the whole box when nothing is
 * cropped across the ray yet, e.g. a cube with only a velocity range seen
 * along the velocity axis: a drag there draws a crop).
 * `ray` and `tip` in model space; `scale` the model's metres per model unit.
 */
export function hitCrop(frame, clip, ray, { scale = 1, tip = null } = {}) {
  const { o, d } = ray;
  const dd = M.dot(d, d);
  const r = HIT_M / scale;
  let best = null;
  if (dd > 0) {
    for (const h of handlePoints(clip)) {
      const c = toModel(frame, h.p);
      const s = M.dot(M.sub(c, o), d) / dd;
      if (s < 0) continue;
      const dist = M.length(M.sub(M.add(o, M.scale3(d, s)), c));
      if (dist <= r && (!best || s < best.t)) best = { kind: 'face', face: h.face, id: h.id, t: s, point: c };
    }
  }
  if (!best && tip) {
    let near = TIP_M / scale;
    for (const h of handlePoints(clip)) {
      const c = toModel(frame, h.p);
      const dist = M.length(M.sub(tip, c));
      if (dist <= near) { near = dist; best = { kind: 'face', face: h.face, id: h.id, t: dd > 0 ? Math.max(0, M.dot(M.sub(c, o), d) / dd) : 0, point: c }; }
    }
  }
  if (best) return best;
  const k = dd > 0 ? depthAxis(d) : 0;
  const across = [0, 1, 2].some((i) => i !== k && (clip.min[i] > 0 || clip.max[i] < 1));
  if (dd > 0 && across) {
    const span = rayAabb(o, d, toModel(frame, clip.min), toModel(frame, clip.max));
    if (span && span[1] >= 0) {
      const t = Math.max(0, span[0]);
      return { kind: 'inside', t, point: M.add(o, M.scale3(d, t)) };
    }
  }
  return { kind: 'new' };
}

/** The parameter u along the line p + u·e_axis (model space) closest to the
 *  ray, or null when the ray runs along the line or the point is behind it. */
export function axisParam(ray, p, axis) {
  const { o, d } = ray;
  const e = [0, 0, 0];
  e[axis] = 1;
  const w = M.sub(o, p);
  const a = M.dot(d, d), b = M.dot(d, e), dw = M.dot(d, w), ew = M.dot(e, w);
  const den = a - b * b;
  if (!(den > 1e-9 * a)) return null;
  const s = (b * ew - dw) / den;
  if (s < 0) return null;
  return (a * ew - b * dw) / den;
}

/** The axis the ray runs most along (model space): a new crop keeps its depth on it. */
export function depthAxis(d) {
  const a = d.map(Math.abs);
  return a[0] >= a[1] && a[0] >= a[2] ? 0 : a[1] >= a[2] ? 1 : 2;
}

/** Where the ray meets the plane across axis k at model coordinate `at`, as
 *  box fractions (clamped to the box), or null when it does not ahead. */
export function planePoint(frame, ray, k, at) {
  if (Math.abs(ray.d[k]) < 1e-12) return null;
  const s = (at - ray.o[k]) / ray.d[k];
  if (!(s > 0)) return null;
  return toFrac(frame, M.add(ray.o, M.scale3(ray.d, s))).map(clamp01);
}

/** A crop from two opposite corners (fractions) across the axes other than
 *  k; axis k as in `clip0`. */
export function clipFromCorners(clip0, k, A, B) {
  const out = copy(clip0);
  for (let i = 0; i < 3; i++) {
    if (i === k) continue;
    out.min[i] = clamp01(Math.min(A[i], B[i]));
    out.max[i] = clamp01(Math.max(A[i], B[i]));
  }
  return out;
}

/**
 * A drag begun at `hit` (hitCrop) with the ray `ray` (model space): what the
 * next frames need, or null when nothing can be dragged there.
 */
export function startDrag(hit, clip, ray, frame) {
  const clip0 = copy(clip);
  if (hit.kind === 'face') {
    const u0 = axisParam(ray, hit.point, hit.face.axis);
    return { kind: 'face', face: hit.face, id: hit.id, clip0, point: hit.point, u0, v0: clip[hit.face.end][hit.face.axis] };
  }
  if (hit.kind === 'inside') return { kind: 'move', clip0, t: hit.t, p0: toFrac(frame, hit.point) };
  const k = depthAxis(ray.d);
  const at = toModel(frame, [0, 1, 2].map((i) => (clip.min[i] + clip.max[i]) / 2))[k];
  const A = planePoint(frame, ray, k, at);
  return A ? { kind: 'new', clip0, k, at, A, B: A } : null;
}

/** The crop while the drag goes on, the ray now (model space). */
export function dragCrop(drag, ray, frame) {
  if (drag.kind === 'face') {
    const u = axisParam(ray, drag.point, drag.face.axis);
    // Along the face's own axis the ray cannot say where: it stays.
    if (u === null || drag.u0 === null) return copy(drag.last || drag.clip0);
    const out = moveFace(drag.clip0, drag.face, drag.v0 + (u - drag.u0) / frame.b[drag.face.axis], GAP);
    drag.last = out;
    return out;
  }
  if (drag.kind === 'move') {
    const p = toFrac(frame, M.add(ray.o, M.scale3(ray.d, drag.t)));
    const out = copy(drag.clip0);
    for (let i = 0; i < 3; i++) {
      // The whole crop shifts, its size kept, inside the box.
      const dv = Math.max(-drag.clip0.min[i], Math.min(1 - drag.clip0.max[i], p[i] - drag.p0[i]));
      out.min[i] = drag.clip0.min[i] + dv;
      out.max[i] = drag.clip0.max[i] + dv;
    }
    return out;
  }
  const B = planePoint(frame, ray, drag.k, drag.at);
  if (B) drag.B = B;
  return clipFromCorners(drag.clip0, drag.k, drag.A, drag.B);
}

/** The crop when the trigger is let go: a new crop too thin on a drawn axis
 *  (a click, not a drag) leaves the crop as it was. */
export function endDrag(drag, clip) {
  if (drag.kind !== 'new') return copy(clip);
  for (let i = 0; i < 3; i++) if (i !== drag.k && clip.max[i] - clip.min[i] < MIN_NEW) return copy(drag.clip0);
  return copy(clip);
}

// ── What is drawn ───────────────────────────────────────────────────────────

const EDGE = [0.55, 0.8, 1.0, 0.95];
const EDGE_HOT = [1.0, 0.85, 0.25, 1.0];
const PREVIEW = [1.0, 0.85, 0.25, 0.9];

/** The 12 edges of the box lo..hi (fractions) as model-space segments. */
export function boxEdges(frame, lo, hi, color = EDGE) {
  const c = (x, y, z) => toModel(frame, [x ? hi[0] : lo[0], y ? hi[1] : lo[1], z ? hi[2] : lo[2]]);
  const out = [];
  for (let i = 0; i < 8; i++) {
    for (const bit of [1, 2, 4]) {
      if (i & bit) continue;
      const j = i | bit;
      out.push({ a: c(i & 1, i & 2, i & 4), b: c(j & 1, j & 2, j & 4), color });
    }
  }
  return out;
}

/**
 * What the gizmo draws for `clip` with `hover` (a hitCrop result, or a drag):
 * {edges: model-space segments, faces: [{m (model-space matrix of the face
 * quad, centred), size [w, h]}] for the fill, handles: [{id, point (model),
 * hot}], fillHot (the inside is pointed at)}.
 */
export function gizmo(frame, clip, hover = null) {
  const hotId = hover && hover.kind === 'face' ? hover.id : hover && hover.id ? hover.id : null;
  const moving = hover && (hover.kind === 'inside' || hover.kind === 'move');
  const edges = boxEdges(frame, clip.min, clip.max, moving ? EDGE_HOT : EDGE);
  const faces = FACES.map((f) => {
    const [u, v] = [0, 1, 2].filter((i) => i !== f.axis);
    const lo = toModel(frame, clip.min), hi = toModel(frame, clip.max);
    const c = [0, 1, 2].map((i) => (lo[i] + hi[i]) / 2);
    c[f.axis] = f.end === 'min' ? lo[f.axis] : hi[f.axis];
    const ex = [0, 0, 0], ey = [0, 0, 0], ez = [0, 0, 0];
    ex[u] = 1; ey[v] = 1; ez[f.axis] = 1;
    return { id: faceId(f), m: new Float32Array([...ex, 0, ...ey, 0, ...ez, 0, ...c, 1]), size: [Math.abs(hi[u] - lo[u]), Math.abs(hi[v] - lo[v])] };
  });
  const handles = handlePoints(clip).map((h) => ({ id: h.id, point: toModel(frame, h.p), hot: h.id === hotId }));
  return { edges, faces, handles, fillHot: !!moving };
}

/** The new crop being drawn, as segments (the drag's preview before it is let go). */
export function previewEdges(frame, clip) { return boxEdges(frame, clip.min, clip.max, PREVIEW); }

/** The toast when the tool is picked (the wrist legend's line is controls.js CROP_LEGEND). */
export const CROP_TOAST = 'Crop: drag a dot to move a face, the inside to move it, elsewhere to draw a new one.';
