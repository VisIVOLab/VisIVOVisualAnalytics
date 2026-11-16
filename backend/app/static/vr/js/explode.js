// An exploded view of a simulation model (issue #371): its parts pulled
// apart in space to be looked at one by one, then put back.
//
// The pieces are the overlaid fields (#357: density, temperature,
// abundances), each in its own colour map and look, or the value ranges of
// one field (2-4 bands between its threshold and its colour max: shells,
// layers), each in a colour of its own. Every piece is the same resident 3-D
// texture drawn again (the overlay storage, or the single cube's) with its
// own model matrix and per-piece uniforms (the channel shown, a lower and an
// upper bound, its colour window and atlas row): nothing is allocated on the
// GPU, in VR or out of it (#229).
//
// A piece's place in the cube's model frame: the layout's offset (a row
// along x, a column along y, or a ring in the x-y plane) plus what the user
// did to it by hand (a translation and a rotation), all scaled by the
// explode amount e (0 assembled, 1 exploded; animated, eased), and a scale
// that keeps the row within MAX_SPAN times the cube's own size and the
// pieces' area together within the cube's own (n·s² ≤ 1), so the pixels
// ray-marched, and the frame's cost, stay at most those of the cube alone
// (the steps a ray takes are counted in voxels, whatever the scale).
// The animation keeps that bound all the way: first (e < SHRINK) the
// assembled volume, drawn once, shrinks to the pieces' scale; then the
// pieces, already at that scale, part (phases(e)). Pieces never overlap at a
// larger scale, so no frame of the animation costs more than the cube alone.
//
// Pure: no DOM, no GL. Unit-tested in backend/tests/vr/explode.test.mjs.

import { composeTRS, mulInto, slerpInto } from './gltf.js';

/** Pieces at most (the pieces' colour atlas has four rows). */
export const MAX_PIECES = 4;
/** What the pieces are: the overlaid fields, or value ranges of one field. */
export const SOURCES = ['fields', 'bands'];
/** How many value ranges one field is split into. */
export const BAND_COUNTS = [2, 3, 4];
export const DEFAULT_BANDS = 3;
/** The layouts: side by side (along the cube's x), one above the other (y), on a ring (the x-y plane). */
export const LAYOUTS = ['row', 'column', 'ring'];
export const LAYOUT_LABELS = { row: 'Row', column: 'Column', ring: 'Ring' };
/** The gap between pieces, a fraction of a piece's size: the default and the slider's range. */
export const DEFAULT_SPREAD = 0.25;
export const SPREAD_MAX = 1.5;
/** An exploded row spans at most this many times the cube's own size (the pieces are scaled down to fit). */
export const MAX_SPAN = 2.2;
/** Assembling or exploding takes this long (ms). */
export const ANIM_MS = 600;
/** The share of the explode amount spent shrinking the assembled volume to the pieces' scale (the rest parts them). */
export const SHRINK = 0.35;
/** A piece is grabbed with the hand this far outside its box, a fraction of the box (a little margin). */
export const GRAB_MARGIN = 0.08;
/** The value ranges' colour maps, from the lowest range to the highest. */
export const BAND_MAPS = { 2: ['blue', 'red'], 3: ['blue', 'green', 'red'], 4: ['blue', 'green', 'orange', 'red'] };

const clamp01 = (v) => Math.max(0, Math.min(1, v));

/** Smoothstep: 0 → 0, 1 → 1, flat at both ends. */
export function ease(x) {
  const t = clamp01(x);
  return t * t * (3 - 2 * t);
}

/**
 * The explode amount's animation: {from, to, t0, ms}. amountAt(a, t) is the
 * eased amount at time `t` (ms); retarget(a, to, t) starts a new animation
 * from wherever the amount is now, taking as long as the distance left
 * needs (a reversal halfway takes half the time). Both mutate nothing but `a`.
 */
export function newAnim(amount = 0) { return { from: amount, to: amount, t0: 0, ms: 0 }; }
export function amountAt(a, t) {
  if (!(a.ms > 0)) return a.to;
  const x = (t - a.t0) / a.ms;
  if (x >= 1) return a.to;
  if (x <= 0) return a.from;
  return a.from + (a.to - a.from) * ease(x);
}
export function retarget(a, to, t, ms = ANIM_MS) {
  const now = amountAt(a, t);
  a.from = now;
  a.to = to;
  a.t0 = t;
  a.ms = Math.abs(to - now) * ms;
  return a;
}
/** Is the animation still running at `t`? */
export const animating = (a, t) => a.ms > 0 && t - a.t0 < a.ms;

/**
 * The value ranges of one field between its threshold `lo` and colour max
 * `hi` (codes 0..1, so on a log-scaled field equal steps in log10 of the
 * values: decades): `n` bands of equal width. [{lo, hi, cmin, cmax}]: a
 * band draws lo <= v < hi, its colour window cmin..cmax its own range; the
 * top band has no upper bound (hi 2: it keeps every value above the colour
 * max too, saturated).
 */
export function bands(lo, hi, n) {
  const k = Math.max(1, Math.min(MAX_PIECES, Math.round(n) || 1));
  const a = clamp01(Number.isFinite(lo) ? lo : 0);
  const b = Math.max(a + 1e-3, Number.isFinite(hi) ? hi : 1);
  const out = [];
  for (let i = 0; i < k; i++) {
    const e0 = a + ((b - a) * i) / k, e1 = a + ((b - a) * (i + 1)) / k;
    out.push({ lo: e0, hi: i === k - 1 ? 2 : e1, cmin: e0, cmax: e1 });
  }
  return out;
}

/** The colour maps of `n` value ranges, lowest first. */
export function bandMaps(n) { return (BAND_MAPS[n] || BAND_MAPS[DEFAULT_BANDS]).slice(0, n); }

/** The scale of `n` pieces: a row of them, `spread` apart, spans at most
 *  MAX_SPAN cube sizes, and together they cover at most the cube's own area
 *  (n·s² ≤ 1: two pieces at 0.71, three at 0.58, four at 0.46). */
export function pieceScale(n, spread = DEFAULT_SPREAD) {
  if (n <= 1) return 1;
  return Math.min(1, MAX_SPAN / (n + (n - 1) * Math.max(0, spread)), 1 / Math.sqrt(n));
}

/**
 * Where `n` pieces go, in the cube's model frame (the units of `extent`, the
 * cube's size; its centre at the origin): {offsets: [[x, y, z]...], scale}.
 * Row: along x, centred; column: along y, the first piece on top; ring: on a
 * circle in the x-y plane, the first piece on top, then clockwise, the
 * neighbours `spread` apart. The scale is pieceScale's for the default
 * spread, so that the spread slider moves the pieces apart without
 * shrinking them.
 */
export function layout(n, kind, extent, spread = DEFAULT_SPREAD) {
  const s = pieceScale(n);
  const g = Math.max(0, spread);
  const offsets = [];
  if (kind === 'ring' && n > 2) {
    const size = Math.max(extent[0], extent[1]) * s;
    const r = (size * (1 + g)) / (2 * Math.sin(Math.PI / n));
    for (let i = 0; i < n; i++) {
      const th = Math.PI / 2 - (2 * Math.PI * i) / n;
      offsets.push([r * Math.cos(th), r * Math.sin(th), 0]);
    }
    return { offsets, scale: s };
  }
  const axis = kind === 'column' ? 1 : 0;
  const pitch = extent[axis] * s * (1 + g);
  for (let i = 0; i < n; i++) {
    const o = [0, 0, 0];
    // A column goes down from the first piece; a row (and a ring of two) left to right.
    o[axis] = (axis === 1 ? -1 : 1) * (i - (n - 1) / 2) * pitch;
    offsets.push(o);
  }
  return { offsets, scale: s };
}

/** A unit quaternion (x, y, z, w) of the rotation in `m`'s upper 3×3 (its columns normalised: a uniform scale is taken out). */
export function quatFromMatrix(m, out = [0, 0, 0, 1]) {
  const sx = Math.hypot(m[0], m[1], m[2]) || 1, sy = Math.hypot(m[4], m[5], m[6]) || 1, sz = Math.hypot(m[8], m[9], m[10]) || 1;
  const m00 = m[0] / sx, m10 = m[1] / sx, m20 = m[2] / sx;
  const m01 = m[4] / sy, m11 = m[5] / sy, m21 = m[6] / sy;
  const m02 = m[8] / sz, m12 = m[9] / sz, m22 = m[10] / sz;
  const tr = m00 + m11 + m22;
  let x, y, z, w;
  if (tr > 0) {
    const S = Math.sqrt(tr + 1) * 2;
    w = 0.25 * S; x = (m21 - m12) / S; y = (m02 - m20) / S; z = (m10 - m01) / S;
  } else if (m00 > m11 && m00 > m22) {
    const S = Math.sqrt(1 + m00 - m11 - m22) * 2;
    w = (m21 - m12) / S; x = 0.25 * S; y = (m01 + m10) / S; z = (m02 + m20) / S;
  } else if (m11 > m22) {
    const S = Math.sqrt(1 + m11 - m00 - m22) * 2;
    w = (m02 - m20) / S; x = (m01 + m10) / S; y = 0.25 * S; z = (m12 + m21) / S;
  } else {
    const S = Math.sqrt(1 + m22 - m00 - m11) * 2;
    w = (m10 - m01) / S; x = (m02 + m20) / S; y = (m12 + m21) / S; z = 0.25 * S;
  }
  const n = Math.hypot(x, y, z, w) || 1;
  out[0] = x / n; out[1] = y / n; out[2] = z / n; out[3] = w / n;
  return out;
}

/** What the user did to a piece by hand: none yet. */
export const newUser = () => ({ t: [0, 0, 0], q: [0, 0, 0, 1] });
const QI = [0, 0, 0, 1];

/** The two phases of explode amount `e`: `shrink` (0..1 over e in
 *  [0, SHRINK]: the assembled volume to the pieces' scale) and `spread`
 *  (0..1 over e in [SHRINK, 1]: the pieces parting). */
export function phases(e) {
  return { shrink: clamp01(e / SHRINK), spread: clamp01((e - SHRINK) / (1 - SHRINK)) };
}
/** Are the pieces drawn at amount `e` (else the assembled volume, shrunk)? */
export const piecesDrawn = (e) => e >= SHRINK;
/** The assembled volume's scale at amount `e` while it shrinks (pieces of scale `scale`). */
export const wholeScale = (e, scale) => 1 + (scale - 1) * clamp01(e / SHRINK);
/** The pixels ray-marched at amount `e`, in units of the cube's own (n pieces of scale `scale`): never above 1. */
export function coverageAt(n, e, scale) {
  if (!piecesDrawn(e)) { const w = wholeScale(e, scale); return w * w; }
  return n * scale * scale;
}

/**
 * A piece's matrix in the cube's model frame, into `out` (16 numbers), with
 * no allocation (`scratch` {t: [3], q: [4], s: [3]}): with p = phases(e),
 * the translation p.spread·(offset + user.t), the rotation slerp(identity,
 * user.q, p.spread) and the scale 1 + (scale − 1)·p.shrink, about the
 * piece's centre. At e = 0 every piece is the cube itself; from SHRINK on it
 * has its own scale.
 */
export function pieceMatrix(out, offset, user, scale, e, scratch) {
  const t = scratch.t, q = scratch.q, s = scratch.s;
  const sp = clamp01((e - SHRINK) / (1 - SHRINK)), sh = clamp01(e / SHRINK);
  for (let i = 0; i < 3; i++) t[i] = sp * (offset[i] + user.t[i]);
  slerpInto(q, QI, user.q, sp);
  s[0] = s[1] = s[2] = 1 + (scale - 1) * sh;
  return composeTRS(out, t, q, s);
}

/** The inverse of 4×4 `m` into `out` (column-major; no allocation). Returns false when singular. */
export function invertInto(out, m) {
  const a00 = m[0], a01 = m[1], a02 = m[2], a03 = m[3], a10 = m[4], a11 = m[5], a12 = m[6], a13 = m[7];
  const a20 = m[8], a21 = m[9], a22 = m[10], a23 = m[11], a30 = m[12], a31 = m[13], a32 = m[14], a33 = m[15];
  const b00 = a00 * a11 - a01 * a10, b01 = a00 * a12 - a02 * a10, b02 = a00 * a13 - a03 * a10;
  const b03 = a01 * a12 - a02 * a11, b04 = a01 * a13 - a03 * a11, b05 = a02 * a13 - a03 * a12;
  const b06 = a20 * a31 - a21 * a30, b07 = a20 * a32 - a22 * a30, b08 = a20 * a33 - a23 * a30;
  const b09 = a21 * a32 - a22 * a31, b10 = a21 * a33 - a23 * a31, b11 = a22 * a33 - a23 * a32;
  let det = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06;
  if (!det) return false;
  det = 1 / det;
  out[0] = (a11 * b11 - a12 * b10 + a13 * b09) * det;
  out[1] = (a02 * b10 - a01 * b11 - a03 * b09) * det;
  out[2] = (a31 * b05 - a32 * b04 + a33 * b03) * det;
  out[3] = (a22 * b04 - a21 * b05 - a23 * b03) * det;
  out[4] = (a12 * b08 - a10 * b11 - a13 * b07) * det;
  out[5] = (a00 * b11 - a02 * b08 + a03 * b07) * det;
  out[6] = (a32 * b02 - a30 * b05 - a33 * b01) * det;
  out[7] = (a20 * b05 - a22 * b02 + a23 * b01) * det;
  out[8] = (a10 * b10 - a11 * b08 + a13 * b06) * det;
  out[9] = (a01 * b08 - a00 * b10 - a03 * b06) * det;
  out[10] = (a30 * b04 - a31 * b02 + a33 * b00) * det;
  out[11] = (a21 * b02 - a20 * b04 - a23 * b00) * det;
  out[12] = (a11 * b07 - a10 * b09 - a12 * b06) * det;
  out[13] = (a00 * b09 - a01 * b07 + a02 * b06) * det;
  out[14] = (a31 * b01 - a30 * b03 - a32 * b00) * det;
  out[15] = (a20 * b03 - a21 * b01 + a22 * b00) * det;
  return true;
}

/** A piece's matrix in the world: model × piece (into `out`). */
export const pieceWorld = (out, model, piece) => mulInto(out, model, piece);

/**
 * The user's part of a piece (into `user`) from where the hand put it: `P`
 * the piece's matrix in the model frame at e = 1 (model⁻¹ × hand × the
 * grab's relation), `offset` its layout's offset. The scale stays the
 * layout's (a hand moves and turns a piece, two hands scale the whole model).
 */
export function userFromPiece(user, P, offset) {
  user.t[0] = P[12] - offset[0];
  user.t[1] = P[13] - offset[1];
  user.t[2] = P[14] - offset[2];
  quatFromMatrix(P, user.q);
  return user;
}

/** Point `p` (world) in the box coordinates (0..1 over the cube) of a piece whose world matrix's inverse is `inv`. */
export function toBox(inv, p, extent) {
  const x = inv[0] * p[0] + inv[4] * p[1] + inv[8] * p[2] + inv[12];
  const y = inv[1] * p[0] + inv[5] * p[1] + inv[9] * p[2] + inv[13];
  const z = inv[2] * p[0] + inv[6] * p[1] + inv[10] * p[2] + inv[14];
  return [x / extent[0] + 0.5, y / extent[1] + 0.5, z / extent[2] + 0.5];
}

/**
 * The piece a hand at world point `p` holds: the shown piece whose box (the
 * crop `box` {min, max} in box coordinates, widened by GRAB_MARGIN of its
 * size) holds the point, the one whose centre is nearest when several do;
 * -1 when none. `invs` the pieces' inverse world matrices, `hidden` their
 * hidden flags.
 */
export function pieceAt(p, invs, extent, box, hidden = []) {
  let best = -1, bestD = Infinity;
  for (let i = 0; i < invs.length; i++) {
    if (hidden[i] || !invs[i]) continue;
    const b = toBox(invs[i], p, extent);
    let inside = true, d = 0;
    for (let k = 0; k < 3; k++) {
      const lo = box.min[k], hi = box.max[k], m = GRAB_MARGIN * Math.max(hi - lo, 1e-3);
      if (b[k] < lo - m || b[k] > hi + m) { inside = false; break; }
      const c = (lo + hi) / 2;
      d += ((b[k] - c) * extent[k]) ** 2;
    }
    if (inside && d < bestD) { best = i; bestD = d; }
  }
  return best;
}

/**
 * The order to draw `n` pieces in, the farthest from the eye first (each is
 * blended over what is behind it): written into `order` (indices) with no
 * allocation. `centres` holds 3 numbers a piece (world), `eye` [x, y, z].
 */
export function farthestFirst(order, centres, n, eye) {
  for (let i = 0; i < n; i++) order[i] = i;
  const d = (i) => (centres[3 * i] - eye[0]) ** 2 + (centres[3 * i + 1] - eye[1]) ** 2 + (centres[3 * i + 2] - eye[2]) ** 2;
  for (let i = 1; i < n; i++) {
    const k = order[i], dk = d(k);
    let j = i - 1;
    while (j >= 0 && d(order[j]) < dk) { order[j + 1] = order[j]; j--; }
    order[j + 1] = k;
  }
  return order;
}

/**
 * The per-piece uniforms (renderer.js drawPiece's p.ov), written into
 * `out` (made by pieceUniforms(), Float32Arrays reused): channel `c` drawn
 * between `lo` and `hi` with its colour window `cmin`..`cmax`, `stretch`
 * code and parameter, `opacity`, colour from atlas row `row`; the other
 * channels never drawn (a threshold of 2).
 */
export function pieceUniforms() {
  return { thr: new Float32Array(3), thrHi: new Float32Array(3), cmin: new Float32Array(3), cmax: new Float32Array(3),
    stretchA: new Float32Array(3), opacity: new Float32Array(3), rows: new Float32Array(3), stretch: new Int32Array(3), n: 1 };
}
export function setPieceUniforms(out, c, { lo, hi, cmin, cmax, stretch, stretchA, opacity, row }) {
  out.thr.fill(2); out.thrHi.fill(2); out.cmin.fill(0); out.cmax.fill(1); out.stretchA.fill(1); out.opacity.fill(0); out.rows.fill(row); out.stretch.fill(0);
  out.thr[c] = clamp01(lo);
  out.thrHi[c] = hi;
  out.cmin[c] = cmin;
  out.cmax[c] = Math.max(cmax, cmin + 1e-3);
  out.stretch[c] = stretch | 0;
  out.stretchA[c] = Number.isFinite(stretchA) ? stretchA : 1;
  out.opacity[c] = opacity;
  out.n = c + 1;
  return out;
}

/** The atlas row of piece i (overlay.js lutRow's: four rows). */
export const pieceRow = (i) => (i + 0.5) / MAX_PIECES;

/**
 * The Explode page (a sub-page of Display): what the pieces are (the
 * overlaid fields, or value ranges of one field), how many ranges, the
 * layout, the spread, one chip a piece (pressed: hidden or shown again),
 * Explode / Assemble and Reset. `ctx.ex` (explode-ui.js menuContext) or
 * null; `kit` hand-menu.js's layout helpers {btn, span, weighted, rowY, X0,
 * RIGHT, WH}. Returns {rows, widgets, status}.
 */
export function explodePage(ctx, kit, why = {}) {
  const { btn, span, rowY, X0, RIGHT, WH } = kit;
  const ex = ctx.ex;
  const none = ctx.kind === 'cube' && ctx.simulation && ex ? '' : (why.noExplode || 'An exploded view is for a simulation volume.');
  const rows = [['source', 'Pieces'], ['bands', 'Value ranges'], ['layout', 'Layout'], ['spread', 'Spread'], ['pieces', 'Shown'], ['go', '']];
  const w = [];
  const src = ex ? ex.source : 'bands';
  w.push(btn('ex-src:fields', span(0, 2, 0), 'Fields', { group: 'ex-src', value: 'fields', on: src === 'fields', disabled: none || (ex.fieldsWhy || ''),
    tip: 'The overlaid fields, each its own piece in its own colours' }));
  w.push(btn('ex-src:bands', span(0, 2, 1), 'Value ranges', { group: 'ex-src', value: 'bands', on: src === 'bands', disabled: none,
    tip: 'One field split into ranges of its values (shells, layers), each a piece' }));
  const noBands = none || (src === 'bands' ? '' : (why.bandsOnly || 'For value ranges only.'));
  BAND_COUNTS.forEach((k, i) => w.push(btn(`ex-bands:${k}`, span(1, BAND_COUNTS.length, i), `${k}`, { group: 'ex-bands', value: k, on: !!ex && ex.bands === k,
    disabled: noBands, tip: `${k} ranges of equal width between the threshold and the colour max (on a log scale: equal in decades)` })));
  LAYOUTS.forEach((k, i) => w.push(btn(`ex-layout:${k}`, span(2, LAYOUTS.length, i), LAYOUT_LABELS[k], { group: 'ex-layout', value: k, on: !!ex && ex.layout === k,
    disabled: none, tip: k === 'row' ? 'Side by side' : k === 'column' ? 'One above the other' : 'Around a circle' })));
  w.push({ id: 'ex-spread', kind: 'slider', rect: [X0, rowY(3), RIGHT - X0, WH], disabled: none });
  const pieces = ex ? ex.pieces : [];
  if (pieces.length) {
    pieces.forEach((p, i) => w.push(btn(`ex-piece:${i}`, span(4, pieces.length, i), p.label, { group: 'ex-piece', value: i, on: !p.hidden, free: true,
      disabled: none, tip: `${p.label}: ${p.hidden ? 'hidden, pressed shown again' : 'shown, pressed hidden'}` })));
  }
  const exploded = !!ex && ex.exploded;
  const noPieces = none || (pieces.length ? '' : (ex && ex.why) || 'Nothing to take apart yet.');
  w.push(btn('ex-go', span(5, 2, 0), exploded ? 'Assemble' : 'Explode', { on: exploded, armed: !!ex && ex.animating, disabled: noPieces,
    tip: exploded ? 'Put the pieces back together' : 'Pull the pieces apart' }));
  w.push(btn('ex-reset', span(5, 2, 1), 'Reset', { disabled: none, tip: 'Every piece back in its place in the layout, all shown' }));
  return { rows, widgets: w, status: 6, placeholder: { pieces: (ex && ex.why) || 'The pieces, once there are some.' } };
}
