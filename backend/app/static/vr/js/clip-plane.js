// The free clipping plane of a simulation volume (issue #369), as pure
// functions: the plane's state, its conversions between the cube's voxels,
// its box and the page's model space, the grab that moves and turns it with
// a controller, flip / snap / reset, the slab of two parallel planes, what
// the gizmo draws, the shader's numbers (and the GLSL they feed) and the
// render session's `set_clip_plane` command. No DOM, no GL calls but the
// uniform setter: node runs the tests (backend/tests/vr/clip-plane.test.mjs).
//
// Conventions (the desktop's cutting plane and the session's `set_crop`):
// the plane lives in the cube's voxel coordinates, 0-based, voxel i centred
// on i along x (NAXIS1), y (NAXIS2) and z, the cube's faces at -0.5 and
// n - 0.5. `origin` is a point of the plane, `normal` a unit vector in the
// same coordinates, and the plane keeps the voxels v with
// normal · (v - origin) >= 0: the side its normal points to. A slab keeps
// |normal · (v - origin)| <= thickness / 2 instead. Voxel coordinates, not
// the page's model space, so the same voxels are cut whatever the box's
// drawn aspect (a simulation's grid spacing) or the cube's placement.
//
// Model space (the page's, renderer.js): the box centred on 0 with sides
// `extent`, box coordinate t in 0..1 at (t - 0.5) * extent; voxel v at box
// coordinate (v + 0.5) / n. A normal turns between the two by the inverse
// transpose of that map: n_model ∝ n_voxel * dims / extent.

import * as M from './mat4.js';

/** The section drawn on the plane: its opacity where the value is above the threshold. */
export const SECTION_ALPHA = 0.85;
/** The thinnest slab, in voxels of the full cube. */
export const MIN_THICKNESS = 2;
/** A new slab: this fraction of the cube's longest side. */
export const SLAB_FRACTION = 0.1;
/** The normal a new or reset plane has: along x (NAXIS1). */
export const DEFAULT_NORMAL = [1, 0, 0];

const longest = (dims) => Math.max(1, ...dims.map((v) => Number(v) || 1));
const finite3 = (a) => Array.isArray(a) && a.length === 3 && a.every((v) => typeof v === 'number' && Number.isFinite(v));

/** A plane through the centre of a cube of `dims` [nx, ny, nz], its normal
 *  along x, off; a slab, when turned on, a tenth of the longest side thick. */
export function defaultPlane(dims) {
  const d = dims.map((v) => Math.max(1, Number(v) || 1));
  return {
    on: false, dims: d,
    origin: d.map((n) => (n - 1) / 2),
    normal: [...DEFAULT_NORMAL],
    slab: false,
    thickness: Math.max(MIN_THICKNESS, Math.round(SLAB_FRACTION * longest(d))),
  };
}

/** A copy (the plane is treated as a value: every change makes a new one). */
export const copyPlane = (p) => ({ ...p, dims: [...p.dims], origin: [...p.origin], normal: [...p.normal] });

/** Is `p` a plane this module made, for the cube of `dims`? (A cube of
 *  another shape starts with a plane of its own.) */
export function fitsCube(p, dims) {
  return !!p && finite3(p.origin) && finite3(p.normal) && Array.isArray(dims) && dims.length === 3
    && p.dims.every((v, i) => v === Math.max(1, Number(dims[i]) || 1));
}

// ── Conversions ─────────────────────────────────────────────────────────────

/** Box coordinate (0..1, faces at the outer voxel edges) of voxel point v. */
export const voxelToBox = (v, dims) => v.map((x, i) => (x + 0.5) / dims[i]);
export const boxToVoxel = (t, dims) => t.map((x, i) => x * dims[i] - 0.5);
/** Model point of box coordinate t. */
export const boxToModel = (t, extent) => t.map((x, i) => (x - 0.5) * extent[i]);
export const modelToBox = (x, extent) => x.map((v, i) => v / extent[i] + 0.5);

/** The plane's point in model space. */
export function modelOrigin(p, extent) { return boxToModel(voxelToBox(p.origin, p.dims), extent); }
/** The plane's unit normal in model space. */
export function modelNormal(p, extent) { return M.normalize(p.normal.map((n, i) => (n * p.dims[i]) / extent[i])); }
/** A voxel-space unit normal from a model-space one. */
export function voxelNormal(nModel, dims, extent) { return M.normalize(nModel.map((n, i) => (n * extent[i]) / dims[i])); }

/** The plane `p` with its point and normal given in model space. */
export function fromModel(p, pointModel, normalModel, extent) {
  const normal = voxelNormal(normalModel, p.dims, extent);
  if (!(M.length(normal) > 0.5)) return copyPlane(p);
  return { ...copyPlane(p), origin: boxToVoxel(modelToBox(pointModel, extent), p.dims), normal };
}

/** Signed distance (voxels, along the normal) of voxel point v from the plane. */
export const signedDistance = (p, v) => M.dot(p.normal, M.sub(v, p.origin));

/** Does the plane keep voxel point v (on, it cuts; off, everything is kept)? */
export function keeps(p, v) {
  if (!p || !p.on) return true;
  const s = signedDistance(p, v);
  return p.slab ? Math.abs(s) <= p.thickness / 2 : s >= 0;
}

/** The eight corners of the cube (voxel coordinates, the outer voxel edges). */
export function corners(dims) {
  const out = [];
  for (let i = 0; i < 8; i++) out.push([0, 1, 2].map((a) => ((i >> a) & 1 ? dims[a] - 0.5 : -0.5)));
  return out;
}

// ── Changes ─────────────────────────────────────────────────────────────────

/**
 * `p` kept on the cube: a plane past the box is brought back to its nearest
 * corner (so it always cuts the box, or touches it), and a point outside the
 * box is moved, within the plane, to the centre of the plane's section of
 * the box (so its handle is where the plane is drawn).
 */
export function keepOnCube(p) {
  const out = copyPlane(p);
  const cs = corners(out.dims);
  const s = cs.map((c) => signedDistance(out, c));
  const lo = Math.min(...s), hi = Math.max(...s);
  // Past the box: through its nearest corner (exactly, not by a shift that rounds).
  if (lo > 0) { out.origin = [...cs[s.indexOf(lo)]]; return out; }
  if (hi < 0) { out.origin = [...cs[s.indexOf(hi)]]; return out; }
  const inside = out.origin.every((v, i) => v >= -0.5 && v <= out.dims[i] - 0.5);
  if (!inside) {
    const poly = sectionPolygon(out.origin, out.normal, cs);
    if (poly.length) out.origin = centroid(poly);
  }
  return out;
}

/** The plane turned the other way round: the other half-space kept. */
export function flip(p) { return { ...copyPlane(p), normal: p.normal.map((v) => (v === 0 ? 0 : -v)) }; }

/** The normal snapped to the box axis nearest to it (its sign kept). */
export function snap(p) {
  const a = p.normal.map(Math.abs);
  const k = a[0] >= a[1] && a[0] >= a[2] ? 0 : a[1] >= a[2] ? 1 : 2;
  const normal = [0, 0, 0];
  normal[k] = p.normal[k] < 0 ? -1 : 1;
  return keepOnCube({ ...copyPlane(p), normal });
}

/** Back through the centre, along x; on/off and slab kept, the slab its default thickness. */
export function reset(p) {
  const d = defaultPlane(p.dims);
  return { ...d, on: p.on, slab: p.slab };
}

/** The plane moved `dv` voxels along its normal (kept on the cube). */
export function nudge(p, dv) {
  return keepOnCube({ ...copyPlane(p), origin: M.add(p.origin, M.scale3(p.normal, dv)) });
}

/** The slab's thickness times `f`, between MIN_THICKNESS and the cube's diagonal. */
export function scaleThickness(p, f) {
  const max = Math.hypot(...p.dims);
  return { ...copyPlane(p), thickness: Math.max(MIN_THICKNESS, Math.min(max, p.thickness * f)) };
}

// ── Grabbing ────────────────────────────────────────────────────────────────

/** The rotation part of a (similarity) matrix, its columns made unit: 9 numbers, column-major. */
export function rotationOf(m) {
  const out = [];
  for (let c = 0; c < 3; c++) {
    const col = M.normalize([m[4 * c], m[4 * c + 1], m[4 * c + 2]]);
    out.push(...col);
  }
  return out;
}
const rot = (r, v) => [0, 1, 2].map((i) => r[i] * v[0] + r[3 + i] * v[1] + r[6 + i] * v[2]);
const rotT = (r, v) => [0, 1, 2].map((i) => r[3 * i] * v[0] + r[3 * i + 1] * v[1] + r[3 * i + 2] * v[2]);

/** Where the ray (model space) meets the plane ahead: {t, point}, or null. */
export function rayPlane(p, extent, ray) {
  const n = modelNormal(p, extent), c = modelOrigin(p, extent);
  const den = M.dot(n, ray.d);
  if (Math.abs(den) < 1e-9) return null;
  const t = M.dot(n, M.sub(c, ray.o)) / den;
  return t > 0 ? { t, point: M.add(ray.o, M.scale3(ray.d, t)) } : null;
}

/**
 * A grab begun with the ray `ray` and the hand's matrix `hand` (both in
 * model space: the controller held in the cube's frame). The plane is held
 * at the end of a stick: the point the ray meets the plane at (or, the ray
 * missing it, the point of the ray nearest the plane's own point) stays at
 * the same distance along the ray, and the plane turns with the hand about
 * that point. Moving or pointing the controller moves the plane; turning the
 * wrist tilts it.
 */
export function startGrab(p, extent, ray, hand) {
  const hit = rayPlane(p, extent, ray);
  const c = modelOrigin(p, extent);
  const dd = M.dot(ray.d, ray.d);
  const t = hit ? hit.t : dd > 0 ? Math.max(0, M.dot(M.sub(c, ray.o), ray.d) / dd) : 0;
  return {
    plane: copyPlane(p), t, g0: M.add(ray.o, M.scale3(ray.d, t)),
    c0: c, n0: modelNormal(p, extent), r0: rotationOf(hand),
  };
}

/** The plane while the grab goes on, with the ray and hand of now (model space). */
export function dragGrab(grab, extent, ray, hand) {
  const r1 = rotationOf(hand);
  // The hand's turn since the grab: R1 R0ᵀ.
  const turn = (v) => rot(r1, rotT(grab.r0, v));
  const g = M.add(ray.o, M.scale3(ray.d, grab.t));
  const point = M.add(g, turn(M.sub(grab.c0, grab.g0)));
  const normal = turn(grab.n0);
  return keepOnCube(fromModel(grab.plane, point, normal, extent));
}

/** Without a controller (the 2-D page): the plane tilted as the pointer's ray
 *  turned from `d0` to `d1` (model space), `gain` times that angle, about
 *  the axis across the two. */
export function tiltByRays(p, extent, d0, d1, gain = 3) {
  const a = M.normalize(d0), b = M.normalize(d1);
  const axis = M.cross(a, b);
  const s = M.length(axis);
  if (s < 1e-9) return copyPlane(p);
  const angle = gain * Math.atan2(s, M.dot(a, b));
  const R = M.rotation(M.scale3(axis, 1 / s), angle);
  const n = M.normalize(M.transformPoint(R, modelNormal(p, extent)));
  return keepOnCube(fromModel(p, modelOrigin(p, extent), n, extent));
}

/** The parameter u of the point origin + u·normal (model space) nearest the
 *  ray, or null when the ray runs (nearly) along the normal. */
export function alongNormal(p, extent, ray) {
  const c = modelOrigin(p, extent), e = modelNormal(p, extent);
  const w = M.sub(ray.o, c);
  const a = M.dot(ray.d, ray.d), b = M.dot(ray.d, e), dw = M.dot(ray.d, w), ew = M.dot(e, w);
  const den = a - b * b;
  if (!(a > 0) || !(den / a > 0.05)) return null;
  return (a * ew - b * dw) / den;
}

/** The plane moved along its normal by `du` model units (a drag on the 2-D page). */
export function moveModel(p, extent, du) {
  const c = modelOrigin(p, extent), n = modelNormal(p, extent);
  return keepOnCube(fromModel(p, M.add(c, M.scale3(n, du)), n, extent));
}

// ── What is drawn ───────────────────────────────────────────────────────────

/** Mean of a list of points. */
function centroid(pts) {
  const s = pts.reduce((a, q) => M.add(a, q), [0, 0, 0]);
  return M.scale3(s, 1 / pts.length);
}

/**
 * The convex polygon where the plane through `c` with normal `n` cuts the
 * box whose eight corners are `cs` (in the order corners() gives: bit a of
 * the index the high end of axis a), in the box's own coordinates, its
 * vertices in order around it; [] when it misses.
 */
export function sectionPolygon(c, n, cs) {
  const s = cs.map((q) => M.dot(n, M.sub(q, c)));
  const pts = [];
  for (let i = 0; i < 8; i++) {
    for (const bit of [1, 2, 4]) {
      if (i & bit) continue;
      const j = i | bit;
      const a = s[i], b = s[j];
      if ((a < 0 && b < 0) || (a > 0 && b > 0)) continue;
      if (a === 0 && b === 0) { pts.push(cs[i], cs[j]); continue; }
      const f = a / (a - b);
      pts.push(M.add(cs[i], M.scale3(M.sub(cs[j], cs[i]), f)));
    }
  }
  // Duplicates (a corner on the plane is met by three edges) out.
  const uniq = [];
  for (const q of pts) if (!uniq.some((u) => M.length(M.sub(u, q)) < 1e-9)) uniq.push(q);
  if (uniq.length < 3) return [];
  const m = centroid(uniq);
  const u = M.normalize(M.sub(uniq[0], m));
  const v = M.cross(M.normalize(n), u);
  return uniq.map((q) => ({ q, a: Math.atan2(M.dot(M.sub(q, m), v), M.dot(M.sub(q, m), u)) }))
    .sort((x, y) => x.a - y.a).map((x) => x.q);
}

/** The corners of the crop `clip` ({min, max} box fractions) in model space, corners() order. */
function clipCorners(clip, extent) {
  const out = [];
  for (let i = 0; i < 8; i++) out.push(boxToModel([0, 1, 2].map((a) => ((i >> a) & 1 ? clip.max[a] : clip.min[a])), extent));
  return out;
}

const EDGE = [1.0, 0.85, 0.25, 0.95];
const EDGE_HOT = [1.0, 0.95, 0.6, 1.0];
const ARROW = [1.0, 0.85, 0.25, 0.9];

/**
 * What the gizmo draws for plane `p` in the crop `clip` (model space):
 * {edges: segments (the section's outline, both faces of a slab, the arrow
 * of the kept side), handle: the plane's point, polygon: the section's
 * outline (the plane itself), empty: the plane misses the crop}.
 */
export function gizmo(p, extent, clip, { hot = false } = {}) {
  const n = modelNormal(p, extent), c = modelOrigin(p, extent);
  const cs = clipCorners(clip, extent);
  const color = hot ? EDGE_HOT : EDGE;
  const edges = [];
  const ring = (poly, col) => poly.forEach((q, i) => edges.push({ a: q, b: poly[(i + 1) % poly.length], color: col }));
  const polygon = sectionPolygon(c, n, cs);
  // The slab's faces, at ± half its thickness (voxels to model units along the normal).
  const half = p.slab ? (p.thickness / 2) * modelPerVoxel(p, extent) : 0;
  if (p.slab) {
    for (const k of [-1, 1]) ring(sectionPolygon(M.add(c, M.scale3(n, k * half)), n, cs), color);
    ring(polygon, [color[0], color[1], color[2], 0.35]);
  } else {
    ring(polygon, color);
  }
  // The arrow: from the plane's point towards the side kept (a slab keeps what lies between its faces).
  if (!p.slab) {
    const L = 0.12 * Math.max(...extent);
    const side = M.normalize(Math.abs(n[0]) < 0.9 ? M.cross(n, [1, 0, 0]) : M.cross(n, [0, 1, 0]));
    const tip = M.add(c, M.scale3(n, L));
    edges.push({ a: c, b: tip, color: ARROW });
    for (const s of [-1, 1]) edges.push({ a: tip, b: M.add(tip, M.add(M.scale3(n, -0.3 * L), M.scale3(side, s * 0.18 * L))), color: ARROW });
  }
  return { edges, handle: c, polygon, empty: polygon.length === 0 };
}

/** Model units per voxel along the plane's normal (the voxel normal's own length there). */
export function modelPerVoxel(p, extent) {
  // A step of one voxel along the voxel normal, measured along the model normal.
  const n = modelNormal(p, extent);
  const step = p.normal.map((v, i) => (v * extent[i]) / p.dims[i]);
  return Math.abs(M.dot(step, n));
}

// ── The shader ──────────────────────────────────────────────────────────────

/**
 * The GLSL of the plane, for a ray marcher over box coordinates t (0..1):
 * s(t) = dot(uClipPlane.xyz, t) + uClipPlane.w is the signed distance from
 * the plane in units of the cube's longest side in voxels (a value of order
 * 1, so the maths is safe at mediump too); uClipMode 0 no plane, 1 keep
 * s >= 0, 2 keep |s| <= uClipHalf (a slab). clipRay narrows the ray's
 * [tn, tf] to what is kept: -1 nothing is, 1 the ray enters through a face
 * of the plane (the section is drawn at tn), 0 otherwise. clipKept answers
 * for one point (a surface, issue #370, may discard with it).
 */
export const CLIP_GLSL = `
uniform int uClipMode;
uniform vec4 uClipPlane;
uniform float uClipHalf, uSectionA;
float clipS(vec3 t) { return dot(uClipPlane.xyz, t) + uClipPlane.w; }
bool clipKept(vec3 t) {
  if (uClipMode == 0) return true;
  float s = clipS(t);
  return uClipMode == 1 ? s >= 0.0 : abs(s) <= uClipHalf;
}
float clipRay(vec3 o, vec3 d, inout float tn, inout float tf) {
  if (uClipMode == 0) return 0.0;
  float s0 = clipS(o), sd = dot(uClipPlane.xyz, d);
  bool face = false;
  if (abs(sd) < 1e-7) {
    if (!clipKept(o)) return -1.0;
  } else if (uClipMode == 1) {
    float ta = -s0 / sd;
    if (sd > 0.0) { if (ta > tn) { tn = ta; face = true; } }
    else tf = min(tf, ta);
  } else {
    float ta = (-uClipHalf - s0) / sd, tb = (uClipHalf - s0) / sd;
    float t0 = min(ta, tb), t1 = max(ta, tb);
    if (t0 > tn) { tn = t0; face = true; }
    tf = min(tf, t1);
  }
  if (tf <= tn) return -1.0;
  return face ? 1.0 : 0.0;
}
`;

/**
 * The shader's numbers for plane `p` (null when off): {mode 1 half-space or
 * 2 slab, plane [a, b, c, d] on box coordinates in units of the longest
 * side, half (the slab's half thickness in those units), sectionA}.
 */
export function shaderParams(p) {
  if (!p || !p.on) return null;
  const L = longest(p.dims);
  const g = p.normal.map((n, i) => (n * p.dims[i]) / L);
  const t0 = voxelToBox(p.origin, p.dims);
  return { mode: p.slab ? 2 : 1, plane: [g[0], g[1], g[2], -M.dot(g, t0)], half: p.slab ? p.thickness / 2 / L : 0, sectionA: SECTION_ALPHA };
}

/** The plane's uniforms on program uniforms `u` (CLIP_GLSL's), from shaderParams (null: no plane). */
export function setClipUniforms(gl, u, c) {
  gl.uniform1i(u.uClipMode, c ? c.mode : 0);
  if (!c) return;
  gl.uniform4f(u.uClipPlane, c.plane[0], c.plane[1], c.plane[2], c.plane[3]);
  gl.uniform1f(u.uClipHalf, c.half);
  gl.uniform1f(u.uSectionA, c.sectionA);
}

/** The shader's signed distance, in JS: what CLIP_GLSL's clipS gives at box coordinate t. */
export const shaderS = (c, t) => c.plane[0] * t[0] + c.plane[1] * t[1] + c.plane[2] * t[2] + c.plane[3];

/** CLIP_GLSL's clipRay in JS (for the tests): [tn, tf, face] or null when nothing is kept. */
export function clipRayJs(c, o, d, tn, tf) {
  if (!c) return [tn, tf, 0];
  const s0 = shaderS(c, o), sd = c.plane[0] * d[0] + c.plane[1] * d[1] + c.plane[2] * d[2];
  let face = 0;
  if (Math.abs(sd) < 1e-7) {
    const kept = c.mode === 1 ? s0 >= 0 : Math.abs(s0) <= c.half;
    if (!kept) return null;
  } else if (c.mode === 1) {
    const ta = -s0 / sd;
    if (sd > 0) { if (ta > tn) { tn = ta; face = 1; } } else tf = Math.min(tf, ta);
  } else {
    const ta = (-c.half - s0) / sd, tb = (c.half - s0) / sd;
    if (Math.min(ta, tb) > tn) { tn = Math.min(ta, tb); face = 1; }
    tf = Math.min(tf, Math.max(ta, tb));
  }
  return tf <= tn ? null : [tn, tf, face];
}

// ── The render session ──────────────────────────────────────────────────────

/** The plane as the session's `set_clip_plane` command (plane null when off). */
export function command(p) {
  if (!p || !p.on) return { type: 'set_clip_plane', plane: null };
  return { type: 'set_clip_plane', plane: { origin: [...p.origin], normal: [...p.normal],
    thickness: p.slab ? p.thickness : null, section_opacity: SECTION_ALPHA } };
}

/** Why the backend would refuse this plane (its parse rule), or ''. */
export function planeError(p) {
  if (!p || !p.on) return '';
  if (!finite3(p.origin)) return 'the point is not three finite numbers';
  if (!finite3(p.normal) || !(M.length(p.normal) > 1e-6)) return 'the normal is not a non-zero vector';
  if (p.slab && !(Number.isFinite(p.thickness) && p.thickness > 0)) return 'the slab has no thickness';
  return '';
}

/** The plane in words, for the Range page and the hub: where it is and what it keeps (voxels, as sent). */
export function describe(p) {
  if (!p || !p.on) return 'Plane off.';
  const f = (v) => (Math.abs(v) >= 100 ? v.toFixed(0) : v.toFixed(1));
  const nn = p.normal.map((v) => (Math.abs(v) < 5e-4 ? 0 : v).toFixed(2)).join(', ');
  const what = p.slab ? `slab ${f(p.thickness)} px thick` : 'keeps the arrow\'s side';
  return `Plane at (${p.origin.map(f).join(', ')}) px, normal (${nn}); ${what}.`;
}
