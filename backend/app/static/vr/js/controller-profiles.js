// The real controller models (WebXR Input Profiles, backend/app/static/vr/models/):
// which profile a controller gets, and how its buttons move the model.
//
// Profile choice: as the WebXR Input Profiles registry does it, the first id
// of the XRInputSource's `profiles` list (most specific first) that the page
// has an asset for. A Quest 3 reports meta-quest-touch-plus; the Touch Pro,
// the Quest 2's oculus-touch-v3 and generic-trigger-squeeze-thumbstick are
// the fallbacks vendored. Nothing matches: the page's own model (models.js).
//
// Visual responses: each component of the profile (trigger, squeeze,
// thumbstick, buttons, touchpad) reads its gamepad button and axes into a
// state (default, touched, pressed) and values; each visual response turns
// one of them into a number in 0..1 that places its value node between its
// min and max nodes (position lerped, rotation slerped) or shows/hides it.
// The same rules as the project's motion-controllers library, written here
// so the page needs no library.
//
// Pure (no DOM, no GL). A rig holds preallocated arrays: updating it each
// frame allocates nothing. Unit-tested in backend/tests/vr/controller-profiles.test.mjs.

import { nodeOrder, composeTRS, mulInto, slerpInto, lerpInto } from './gltf.js';

/** The profiles whose assets are vendored, as the registry's profilesList.json names them. */
export const VENDORED_PROFILES = ['meta-quest-touch-plus', 'meta-quest-touch-pro', 'oculus-touch-v3', 'generic-trigger-squeeze-thumbstick'];

/** As @webxr-input-profiles/motion-controllers. */
export const BUTTON_TOUCH_THRESHOLD = 0.05;
export const AXIS_TOUCH_THRESHOLD = 0.1;

/**
 * The profile for an input source: the first of `profiles` (its own order)
 * that `supported` (the profilesList.json object, id → {path}) has, with
 * the layout for `handedness` ('any' takes the first layout, as the registry
 * library does). Null when nothing matches. The profile JSON is fetched by
 * the caller; this only picks the id.
 */
export function selectProfileId(profiles, supported) {
  for (const id of profiles || []) {
    if (typeof id === 'string' && supported && Object.prototype.hasOwnProperty.call(supported, id)) return id;
  }
  return null;
}

/** The layout of a profile for one hand, or null. */
export function layoutFor(profile, handedness) {
  const layouts = (profile && profile.layouts) || {};
  if (handedness === 'any') { const k = Object.keys(layouts)[0]; return k ? layouts[k] : null; }
  return layouts[handedness] || null;
}

/** Axis values in -1..1 to 0..1, the pair kept inside the unit circle (motion-controllers' normalizeAxes). */
export function normalizeAxes(x = 0, y = 0, out = [0, 0]) {
  let ax = x, ay = y;
  const h = Math.sqrt(x * x + y * y);
  if (h > 1) { const th = Math.atan2(y, x); ax = Math.cos(th); ay = Math.sin(th); }
  out[0] = ax * 0.5 + 0.5; out[1] = ay * 0.5 + 0.5;
  return out;
}

const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);

/**
 * A component's values from a gamepad, into `out` {state, button, xAxis, yAxis}:
 * button clamped to 0..1; pressed when the button is pressed or at 1; touched
 * when touched, past 0.05, or an axis past 0.1.
 */
export function componentValues(indices, gamepad, out) {
  out.state = 'default'; out.button = 0; out.xAxis = 0; out.yAxis = 0;
  const buttons = (gamepad && gamepad.buttons) || [];
  const axes = (gamepad && gamepad.axes) || [];
  if (indices.button !== undefined && buttons.length > indices.button) {
    const b = buttons[indices.button];
    out.button = clamp(Number(b && b.value) || 0, 0, 1);
    if ((b && b.pressed) || out.button === 1) out.state = 'pressed';
    else if ((b && b.touched) || out.button > BUTTON_TOUCH_THRESHOLD) out.state = 'touched';
  }
  if (indices.xAxis !== undefined && axes.length > indices.xAxis) {
    out.xAxis = clamp(Number(axes[indices.xAxis]) || 0, -1, 1);
    if (out.state === 'default' && Math.abs(out.xAxis) > AXIS_TOUCH_THRESHOLD) out.state = 'touched';
  }
  if (indices.yAxis !== undefined && axes.length > indices.yAxis) {
    out.yAxis = clamp(Number(axes[indices.yAxis]) || 0, -1, 1);
    if (out.state === 'default' && Math.abs(out.yAxis) > AXIS_TOUCH_THRESHOLD) out.state = 'touched';
  }
  return out;
}

const axesScratch = [0, 0];
/**
 * A visual response's value for a component's values: for 'transform' a
 * number in 0..1 (axes 0.5 at rest; the button 0 outside its states); for
 * 'visibility' 1 (shown) or 0.
 */
export function responseValue(resp, values) {
  const inState = resp.states.includes(values.state);
  switch (resp.componentProperty) {
    case 'xAxis': normalizeAxes(values.xAxis, values.yAxis, axesScratch); return inState ? axesScratch[0] : 0.5;
    case 'yAxis': normalizeAxes(values.xAxis, values.yAxis, axesScratch); return inState ? axesScratch[1] : 0.5;
    case 'button': return inState ? values.button : 0;
    case 'state': return inState ? 1 : 0;
    default: return 0;
  }
}

/**
 * A rig for a loaded model and its layout: the node order, each node's base
 * transform, local and world matrices (preallocated), visibility, and the
 * visual responses bound to node indices. Responses whose nodes the file
 * lacks (the Touch Plus profile names a 'menu' the model does not have) are
 * left out; `skipped` lists them.
 */
export function makeRig(model, layout) {
  const N = model.nodes.length;
  const byName = new Map();
  model.nodes.forEach((n, i) => { if (n.name && !byName.has(n.name)) byName.set(n.name, i); });
  const base = model.nodes.map((n) => ({ t: Float32Array.from(n.t), r: Float32Array.from(n.r), s: Float32Array.from(n.s), matrix: n.matrix }));
  const rig = {
    model,
    order: nodeOrder(model),
    base,
    t: base.map((b) => Float32Array.from(b.t)),
    r: base.map((b) => Float32Array.from(b.r)),
    local: Array.from({ length: N }, () => new Float32Array(16)),
    world: Array.from({ length: N }, () => new Float32Array(16)),
    visible: new Uint8Array(N).fill(1),
    drawn: new Uint8Array(N),          // visible and every ancestor visible
    components: [],
    animated: new Set(),
    skipped: [],
    values: { state: 'default', button: 0, xAxis: 0, yAxis: 0 },
  };
  const comps = (layout && layout.components) || {};
  for (const [id, c] of Object.entries(comps)) {
    const comp = { id, type: c.type, indices: c.gamepadIndices || {}, responses: [] };
    for (const [rid, vr] of Object.entries(c.visualResponses || {})) {
      const prop = vr.valueNodeProperty || 'transform';
      const value = byName.get(vr.valueNodeName);
      const min = byName.get(vr.minNodeName), max = byName.get(vr.maxNodeName);
      const ok = value !== undefined && (prop === 'visibility' || (min !== undefined && max !== undefined
        && !base[min].matrix && !base[max].matrix && !base[value].matrix));
      if (!ok) { rig.skipped.push(`${id}/${rid}`); continue; }
      comp.responses.push({ id: rid, componentProperty: vr.componentProperty, states: vr.states || [], valueNodeProperty: prop, value, min, max });
      rig.animated.add(value);
    }
    if (comp.responses.length) rig.components.push(comp);
  }
  rig.animated = Array.from(rig.animated);
  return rig;
}

/** Moves the rig's nodes for this gamepad (no allocation). Returns the rig. */
export function applyResponses(rig, gamepad) {
  for (const i of rig.animated) {
    rig.t[i].set(rig.base[i].t); rig.r[i].set(rig.base[i].r); rig.visible[i] = 1;
  }
  const v = rig.values;
  for (const comp of rig.components) {
    componentValues(comp.indices, gamepad, v);
    for (const resp of comp.responses) {
      const x = responseValue(resp, v);
      if (resp.valueNodeProperty === 'visibility') { rig.visible[resp.value] = x ? 1 : 0; continue; }
      lerpInto(rig.t[resp.value], rig.base[resp.min].t, rig.base[resp.max].t, x);
      slerpInto(rig.r[resp.value], rig.base[resp.min].r, rig.base[resp.max].r, x);
    }
  }
  return rig;
}

/** The world matrix of every node: root (the grip pose, 4×4) × the node chain. No allocation. */
export function updateWorld(rig, root) {
  const { nodes } = rig.model;
  for (const i of rig.order) {
    const b = rig.base[i];
    if (b.matrix) rig.local[i].set(b.matrix);
    else composeTRS(rig.local[i], rig.t[i], rig.r[i], b.s);
    const p = nodes[i].parent;
    mulInto(rig.world[i], p >= 0 ? rig.world[p] : root, rig.local[i]);
    rig.drawn[i] = rig.visible[i] && (p < 0 || rig.drawn[p]) ? 1 : 0;
  }
  return rig;
}

/** The index of a node by name, or -1 (tests and the page's checks). */
export function nodeIndex(rig, name) {
  return rig.model.nodes.findIndex((n) => n.name === name);
}
