// Visible controllers and tracked hands (issue #226, point 7). On the Quest
// only the pointer rays could be seen: the controllers were three short
// one-pixel lines and the hands a few crosses, both lost at headset
// resolution. Now each controller is a small solid model made here (a handle,
// the head with its ring, a thumbstick and a trigger; nothing is loaded from
// a CDN), and a tracked hand is drawn as a sphere at each of its joints and
// a thin bar along each bone.
//
// Geometry only, as flat triangle lists of {position, normal, colour}
// (MESH_STRIDE floats a vertex); renderer.js draws them with a little
// shading. Pure: unit-tested in backend/tests/vr/models.test.mjs.

import * as M from './mat4.js';

/** Floats per vertex: position 3, normal 3, colour 4. */
export const MESH_STRIDE = 10;

/** The WebXR hand joints, in the spec's order. */
export const HAND_JOINTS = [
  'wrist',
  'thumb-metacarpal', 'thumb-phalanx-proximal', 'thumb-phalanx-distal', 'thumb-tip',
  'index-finger-metacarpal', 'index-finger-phalanx-proximal', 'index-finger-phalanx-intermediate', 'index-finger-phalanx-distal', 'index-finger-tip',
  'middle-finger-metacarpal', 'middle-finger-phalanx-proximal', 'middle-finger-phalanx-intermediate', 'middle-finger-phalanx-distal', 'middle-finger-tip',
  'ring-finger-metacarpal', 'ring-finger-phalanx-proximal', 'ring-finger-phalanx-intermediate', 'ring-finger-phalanx-distal', 'ring-finger-tip',
  'pinky-finger-metacarpal', 'pinky-finger-phalanx-proximal', 'pinky-finger-phalanx-intermediate', 'pinky-finger-phalanx-distal', 'pinky-finger-tip',
];

/** The bones: each finger from the wrist through its joints to its tip. */
export const HAND_BONES = (() => {
  const out = [];
  for (const f of ['thumb', 'index-finger', 'middle-finger', 'ring-finger', 'pinky-finger']) {
    const chain = HAND_JOINTS.filter((n) => n.startsWith(`${f}-`));
    let prev = 'wrist';
    for (const n of chain) { out.push([prev, n]); prev = n; }
  }
  // The palm's edge, so the hand reads as a hand and not as five sticks.
  out.push(['index-finger-phalanx-proximal', 'middle-finger-phalanx-proximal'],
    ['middle-finger-phalanx-proximal', 'ring-finger-phalanx-proximal'],
    ['ring-finger-phalanx-proximal', 'pinky-finger-phalanx-proximal']);
  return out;
})();

function push(out, p, n, c) { out.push(p[0], p[1], p[2], n[0], n[1], n[2], c[0], c[1], c[2], c[3]); }

function tri(out, a, b, c, col) {
  const n = M.normalize(M.cross(M.sub(b, a), M.sub(c, a)));
  push(out, a, n, col); push(out, b, n, col); push(out, c, n, col);
}

/** A box between two corners of an axis-aligned frame given by `m` (a 4×4, local box corners lo..hi). */
export function box(out, m, lo, hi, col) {
  const c = (x, y, z) => M.transformPoint(m, [x ? hi[0] : lo[0], y ? hi[1] : lo[1], z ? hi[2] : lo[2]]);
  const quads = [[[0, 0, 1], [1, 0, 1], [1, 1, 1], [0, 1, 1]], [[1, 0, 0], [0, 0, 0], [0, 1, 0], [1, 1, 0]],
    [[1, 0, 1], [1, 0, 0], [1, 1, 0], [1, 1, 1]], [[0, 0, 0], [0, 0, 1], [0, 1, 1], [0, 1, 0]],
    [[0, 1, 1], [1, 1, 1], [1, 1, 0], [0, 1, 0]], [[0, 0, 0], [1, 0, 0], [1, 0, 1], [0, 0, 1]]];
  for (const q of quads) {
    const [a, b, cc, d] = q.map((v) => c(...v));
    tri(out, a, b, cc, col); tri(out, a, cc, d, col);
  }
}

/** A short cylinder (n sides) of radius r along local +y from y0 to y1, in frame m. */
export function cylinder(out, m, r, y0, y1, col, n = 12) {
  const P = (a, y) => M.transformPoint(m, [r * Math.cos(a), y, r * Math.sin(a)]);
  const top = M.transformPoint(m, [0, y1, 0]), bot = M.transformPoint(m, [0, y0, 0]);
  for (let i = 0; i < n; i++) {
    const a0 = (2 * Math.PI * i) / n, a1 = (2 * Math.PI * (i + 1)) / n;
    const p00 = P(a0, y0), p01 = P(a0, y1), p10 = P(a1, y0), p11 = P(a1, y1);
    tri(out, p00, p01, p11, col); tri(out, p00, p11, p10, col);
    tri(out, top, p11, p01, col); tri(out, bot, p00, p10, col);
  }
}

/** An octahedron (a sphere at this size) of radius r at p. */
export function octa(out, p, r, col) {
  const v = [[r, 0, 0], [-r, 0, 0], [0, r, 0], [0, -r, 0], [0, 0, r], [0, 0, -r]].map((d) => M.add(p, d));
  const faces = [[0, 2, 4], [2, 1, 4], [1, 3, 4], [3, 0, 4], [2, 0, 5], [1, 2, 5], [3, 1, 5], [0, 3, 5]];
  for (const [a, b, c] of faces) tri(out, v[a], v[b], v[c], col);
}

/** A bar of half-width w from a to b (a square prism). */
export function bar(out, a, b, w, col) {
  const d = M.sub(b, a);
  const len = Math.hypot(...d);
  if (len < 1e-5) return;
  const z = M.scale3(d, 1 / len);
  const ref = Math.abs(z[1]) < 0.9 ? [0, 1, 0] : [1, 0, 0];
  const x = M.normalize(M.cross(ref, z)), y = M.cross(z, x);
  const m = new Float32Array([...x, 0, ...y, 0, ...z, 0, ...a, 1]);
  box(out, m, [-w, -w, 0], [w, w, len], col);
}

const shade = (c, k) => [c[0] * k, c[1] * k, c[2] * k, c[3]];

/**
 * A controller in its grip space (WebXR: the origin in the closed hand, -z
 * along the barrel, +y up the thumb's side), `grip` its pose (4×4, world),
 * `col` the role's colour; `pressed` {trigger, grip} light those parts.
 * Appended to `out` in world space. About 7 cm by 13 cm, like a Touch Plus.
 */
export function controllerMesh(out, grip, col, { pressed = {}, handedness = 'right' } = {}) {
  const side = handedness === 'left' ? -1 : 1;
  const body = shade(col, 0.55), dark = [0.12, 0.13, 0.16, 1];
  // The handle, tilted forward a little as the real one is.
  const tilt = M.multiply(grip, M.rotation([1, 0, 0], -0.35));
  box(out, tilt, [-0.016, -0.022, -0.03], [0.016, 0.016, 0.075], body);
  // The head: a disc above the hand with the face buttons and a ring around it.
  const head = M.multiply(grip, M.multiply(M.translation(0, 0.02, -0.045), M.rotation([1, 0, 0], 0.5)));
  cylinder(out, head, 0.03, -0.012, 0.012, body, 16);
  cylinder(out, head, 0.039, -0.006, 0.006, col, 16);                 // the ring
  // The thumbstick on the head's top, and a face button.
  const stick = M.multiply(head, M.translation(-0.006 * side, 0.012, 0.004));
  cylinder(out, stick, 0.007, 0, 0.012, dark, 8);
  const btn = M.multiply(head, M.translation(0.011 * side, 0.012, -0.004));
  cylinder(out, btn, 0.005, 0, 0.004, dark, 8);
  // The trigger under the head (lit while pulled) and the grip button on the side.
  box(out, M.multiply(grip, M.translation(0, -0.012, -0.05)), [-0.007, -0.016, -0.008], [0.007, 0.004, 0.008],
    pressed.trigger ? [1, 0.9, 0.35, 1] : dark);
  box(out, M.multiply(grip, M.translation(-0.017 * side, -0.004, 0.012)), [-0.004, -0.012, -0.018], [0.004, 0.012, 0.018],
    pressed.grip ? [1, 0.9, 0.35, 1] : dark);
  return out;
}

/**
 * A tracked hand: a sphere at each joint (its radius from the browser, or
 * 8 mm) and a bar along each bone; `joints` {name: {p, r}} in world space.
 * The thumb and index tips turn yellow while pinching, the palm while gripping.
 */
export function handMesh(out, joints, col, { pinch = false, grip = false } = {}) {
  const hot = [1, 0.85, 0.3, 1];
  const boneCol = shade(col, 0.7);
  for (const [a, b] of HAND_BONES) {
    if (joints[a] && joints[b]) bar(out, joints[a].p, joints[b].p, 0.0035, grip ? hot : boneCol);
  }
  for (const name of HAND_JOINTS) {
    const j = joints[name];
    if (!j) continue;
    const tipHot = pinch && (name === 'thumb-tip' || name === 'index-finger-tip');
    const r = Math.max(0.004, Math.min(0.012, j.r || 0.008)) * (name.endsWith('-tip') ? 0.9 : 1);
    octa(out, j.p, r, tipHot ? hot : col);
  }
  return out;
}

/** The number of vertices in a mesh array. */
export function vertexCount(arr) { return Math.floor(arr.length / MESH_STRIDE); }
