// Hand tracking in the headset (issue #191): WebXR hand input as the page's
// controllers. A pinch (thumb tip to index tip) is the trigger: it presses the
// panels' buttons, drags their sliders and fires the selection tool along the
// hand's ray. A closed hand (middle, ring and little finger curled) is the
// grip: it grabs the cube, both hands move, turn and scale it. The page builds
// the same hand object for a controller and for a hand, so both work, and
// the headset may switch between them at any time (its input sources change;
// the page reads them every frame).
//
// Pure gesture logic (pinch and grip with hysteresis, the hand's frame, its
// ray and its stabilisation) is tested by node with synthetic joints
// (backend/tests/vr/hands.test.mjs); readJoints and HandTracker.update are the
// thin WebXR layer.

import * as M from './mat4.js';

/** The joints the gestures read (WebXR Hand Input names). */
export const JOINTS = [
  'wrist', 'thumb-metacarpal', 'thumb-phalanx-proximal', 'thumb-tip',
  'index-finger-metacarpal', 'index-finger-phalanx-proximal', 'index-finger-tip',
  'middle-finger-metacarpal', 'middle-finger-phalanx-proximal', 'middle-finger-tip',
  'ring-finger-phalanx-proximal', 'ring-finger-tip',
  'pinky-finger-metacarpal', 'pinky-finger-phalanx-proximal', 'pinky-finger-tip',
];

/** Pinch: the tips' surfaces closer than ON (m) presses, farther than OFF releases. */
export const PINCH_ON = 0.012;
export const PINCH_OFF = 0.028;
/** Grip: the curl (tip-to-palm distance over the palm's length, mean of the
 *  middle, ring and little fingers) below ON closes the hand, above OFF opens it. */
export const CURL_ON = 0.72;
export const CURL_OFF = 0.95;

const dist = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
const mid = (a, b) => [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2, (a[2] + b[2]) / 2];

/** Thumb tip to index tip, surface to surface (m). joints: {name: {p, r}}. */
export function pinchDistance(j) {
  const a = j['thumb-tip'], b = j['index-finger-tip'];
  if (!a || !b) return Infinity;
  return dist(a.p, b.p) - (a.r || 0) - (b.r || 0);
}

/** The palm's centre: between the wrist and the middle finger's knuckle. */
export function palmCentre(j) {
  return mid(j.wrist.p, j['middle-finger-phalanx-proximal'].p);
}

/** How curled the middle, ring and little fingers are (about 1.4 open, 0.5 in a fist). */
export function curl(j) {
  const palm = palmCentre(j);
  const len = dist(j.wrist.p, j['middle-finger-phalanx-proximal'].p);
  if (!(len > 0)) return Infinity;
  const tips = ['middle-finger-tip', 'ring-finger-tip', 'pinky-finger-tip'].filter((n) => j[n]);
  if (!tips.length) return Infinity;
  return tips.reduce((s, n) => s + dist(j[n].p, palm), 0) / tips.length / len;
}

/**
 * The gestures with hysteresis. update(joints) -> {pinch, grip, pinchDist, curl}.
 * A closed hand is a grip, never a pinch (the thumb rests on the index in a
 * fist); a pinch held while the hand closes is released.
 */
export class Gesture {
  // A hand first seen must show its fingertips apart once before a pinch
  // counts (it may appear already closed, or in a fist).
  constructor() { this.pinch = false; this.grip = false; this.needOpen = true; }
  update(j) {
    const pd = pinchDistance(j), c = curl(j);
    if (this.grip) { if (c > CURL_OFF) this.grip = false; } else if (c < CURL_ON) this.grip = true;
    // After a grip, the thumb still rests on the index while the hand opens:
    // no pinch until the fingertips have parted (beyond PINCH_OFF) once.
    if (this.grip) this.needOpen = true;
    else if (this.needOpen && pd > PINCH_OFF) this.needOpen = false;
    if (this.grip || this.needOpen) this.pinch = false;
    else if (this.pinch) { if (pd > PINCH_OFF) this.pinch = false; } else if (pd < PINCH_ON) this.pinch = true;
    return { pinch: this.pinch, grip: this.grip, pinchDist: pd, curl: c };
  }
}

/**
 * A rigid frame of the hand (column-major 4×4): origin at the palm's centre,
 * y along the fingers (wrist to the middle knuckle), x across the knuckles
 * (little finger to index), z = x × y. Grabbing keeps the cube fixed in it.
 */
export function handFrame(j) {
  const o = palmCentre(j);
  const y = M.normalize(M.sub(j['middle-finger-phalanx-proximal'].p, j.wrist.p));
  let x = M.sub(j['index-finger-phalanx-proximal'].p, j['pinky-finger-phalanx-proximal'].p);
  x = M.normalize(M.sub(x, M.scale3(y, M.dot(x, y))));
  const z = M.cross(x, y);
  return new Float32Array([x[0], x[1], x[2], 0, y[0], y[1], y[2], 0, z[0], z[1], z[2], 0, o[0], o[1], o[2], 1]);
}

/**
 * A pointing ray when the headset gives none: from an estimated shoulder
 * (below the head, to the hand's side) through the point between the thumb's
 * and the index's knuckles, starting there. headRight: the head's right
 * vector in the floor plane.
 */
export function handRay(j, headPos, headRight = [1, 0, 0], handedness = 'right') {
  const side = handedness === 'left' ? -1 : 1;
  const shoulder = M.add(headPos, M.add([0, -0.16, 0], M.scale3(headRight, 0.17 * side)));
  const at = mid(j['thumb-phalanx-proximal'].p, j['index-finger-phalanx-proximal'].p);
  return { o: at, d: M.normalize(M.sub(at, shoulder)) };
}

/**
 * Smooths a hand's ray, and keeps it still while a pinch closes: closing the
 * fingers moves the hand, so the ray at the press is the ray of just before
 * the fingers started to close (`hold` ms of history), and it stays there for
 * `settle` ms after the press. Smoothing: exponential, time constant `tau` ms.
 */
export class RayStabilizer {
  constructor({ tau = 35, hold = 90, settle = 140 } = {}) {
    Object.assign(this, { tau, hold, settle });
    this.ray = null; this.hist = []; this.frozen = null; this.until = 0;
  }
  update(ray, t, pressedEdge = false) {
    if (!ray) { this.ray = null; this.hist = []; this.frozen = null; return null; }
    if (!this.ray) this.ray = { o: [...ray.o], d: M.normalize(ray.d) };
    else {
      const dt = Math.max(0, t - this.lastT);
      const a = 1 - Math.exp(-dt / this.tau);
      this.ray = { o: this.ray.o.map((v, i) => v + (ray.o[i] - v) * a),
        d: M.normalize(this.ray.d.map((v, i) => v + (M.normalize(ray.d)[i] - v) * a)) };
    }
    this.lastT = t;
    this.hist.push({ t, ray: { o: [...this.ray.o], d: [...this.ray.d] } });
    while (this.hist.length > 2 && this.hist[1].t <= t - this.hold) this.hist.shift();
    if (pressedEdge) {
      this.frozen = this.hist[0].ray;
      this.until = t + this.settle;
    }
    if (this.frozen && t < this.until) {
      // Held in direction only: the hand may still be carried along.
      return { o: this.ray.o, d: this.frozen.d };
    }
    this.frozen = null;
    return this.ray;
  }
}

/** The joints of an XRHand in reference space `ref`: {name: {p, r}}, or null
 *  when the hand is not tracked this frame. */
export function readJoints(frame, hand, ref) {
  const out = {};
  for (const name of JOINTS) {
    const space = hand.get(name);
    if (!space) continue;
    const pose = frame.getJointPose(space, ref);
    if (!pose) return null;
    const p = pose.transform.position;
    out[name] = { p: [p.x, p.y, p.z], r: pose.radius || 0 };
  }
  return out.wrist && out['thumb-tip'] && out['index-finger-tip'] && out['middle-finger-phalanx-proximal'] ? out : null;
}

/** The page's hand object for a tracked hand: m (grip frame), pos, g (a
 *  gamepad-like object: button 0 the pinch, button 1 the grip), ray. */
export function handObject(joints, gesture, ray) {
  const m = handFrame(joints);
  return {
    m, pos: [m[12], m[13], m[14]], ray, isHand: true, joints, gesture,
    g: { buttons: [{ pressed: gesture.pinch, value: gesture.pinch ? 1 : 0 }, { pressed: gesture.grip, value: gesture.grip ? 1 : 0 }], axes: [] },
  };
}

/** Per-hand gesture and ray state across frames. */
export class HandTracker {
  constructor() { this.hands = new Map(); }

  /** A WebXR hand input source this frame -> the page's hand object, or null. */
  update(frame, src, ref, t, headPos, headRight) {
    const key = src.handedness;
    const joints = readJoints(frame, src.hand, ref);
    if (!joints) { this.hands.delete(key); return null; }
    let h = this.hands.get(key);
    if (!h) { h = { gesture: new Gesture(), stab: new RayStabilizer(), wasPinch: false }; this.hands.set(key, h); }
    const g = h.gesture.update(joints);
    let ray = null;
    const rp = src.targetRaySpace && frame.getPose(src.targetRaySpace, ref);
    if (rp) {
      const r = rp.transform.matrix;
      ray = { o: [r[12], r[13], r[14]], d: [-r[8], -r[9], -r[10]] };
    } else if (headPos) {
      ray = handRay(joints, headPos, headRight, key);
    }
    const ray2 = h.stab.update(ray, t, g.pinch && !h.wasPinch);
    h.wasPinch = g.pinch;
    return handObject(joints, g, ray2);
  }

  /** Forget the hands that are no longer input sources. */
  keep(keys) { for (const k of [...this.hands.keys()]) if (!keys.has(k)) this.hands.delete(k); }
}

/** Segments to draw a tracked hand: the fingertips and the palm, a ring at
 *  the pinch closing (world space). */
export function handSegments(h, color) {
  const out = [];
  const j = h.joints;
  const cross = (p, s, col) => {
    out.push({ a: [p[0] - s, p[1], p[2]], b: [p[0] + s, p[1], p[2]], color: col });
    out.push({ a: [p[0], p[1] - s, p[2]], b: [p[0], p[1] + s, p[2]], color: col });
    out.push({ a: [p[0], p[1], p[2] - s], b: [p[0], p[1], p[2] + s], color: col });
  };
  for (const n of ['thumb-tip', 'index-finger-tip', 'middle-finger-tip', 'ring-finger-tip', 'pinky-finger-tip']) {
    if (j[n]) cross(j[n].p, 0.006, color);
  }
  const pinchCol = h.gesture.pinch ? [1, 0.85, 0.3, 1] : color;
  if (j['thumb-tip'] && j['index-finger-tip']) out.push({ a: j['thumb-tip'].p, b: j['index-finger-tip'].p, color: pinchCol });
  cross(palmCentre(j), h.gesture.grip ? 0.02 : 0.01, h.gesture.grip ? [1, 0.85, 0.3, 1] : color);
  return out;
}
