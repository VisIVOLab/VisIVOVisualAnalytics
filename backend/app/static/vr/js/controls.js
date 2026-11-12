// The headset's controls (issue #201): fixed hand roles, explicit modes and
// one meaning per stick and button in each mode. The primary hand (right, or
// left with the left-handed setting) points and acts; the secondary hand holds
// the menu; both grab. The same control map drives the input handler in
// main.js, the labels by the controllers, the menu's Help page and the
// first-run guide, so what a label says is what the control does.
//
// Pure: unit-tested by node (backend/tests/vr/controls.test.mjs).

export const MODES = ['explore', 'select', 'clip'];
export const MODE_NAMES = { explore: 'Explore', select: 'Select', clip: 'Clip' };
/** The tools of each mode (the secondary stick's left/right steps through them). */
export const SUBTOOLS = { explore: [], select: ['probe', 'lasso', 'box'], clip: ['slab', 'box'] };
export const SUB_NAMES = {
  select: { probe: 'spectrum', lasso: 'Kinematic Lasso', box: 'box region' },
  clip: { slab: 'velocity slab', box: 'clipping box' },
};

/** Which hand does what. */
export function roles(leftHanded = false) {
  return leftHanded ? { primary: 'left', secondary: 'right' } : { primary: 'right', secondary: 'left' };
}

/** The face buttons' names as printed on the controller of `hand`. */
export function buttonNames(hand) {
  return hand === 'left' ? { lower: 'X', upper: 'Y' } : { lower: 'A', upper: 'B' };
}

/** The controls a hand object offers, by its WebXR 'xr-standard' gamepad
 *  index (axes 2, 3 the thumbstick). Tracked hands (hands.js) fill 0 (pinch),
 *  1 (fist) and 4 (palm up). */
export const BUTTON_INDEX = { trigger: 0, grip: 1, stick: 3, lower: 4, upper: 5 };

/** The next mode (A on the primary controller), or the one before. */
export function nextMode(mode, step = 1) {
  const i = MODES.indexOf(mode);
  return MODES[(((i < 0 ? 0 : i) + step) % MODES.length + MODES.length) % MODES.length];
}

/** The next tool of a mode (null when the mode has none). */
export function nextSub(mode, sub, step = 1) {
  const list = SUBTOOLS[mode] || [];
  if (!list.length) return null;
  const i = Math.max(0, list.indexOf(sub));
  return list[((i + step) % list.length + list.length) % list.length];
}

/** The mode and tool as one short line: "SELECT · Kinematic Lasso". */
export function modeLine(mode, sub, { pvArmed = false } = {}) {
  const name = (MODE_NAMES[mode] || mode).toUpperCase();
  if (pvArmed) return `${name} · PV path`;
  const s = SUB_NAMES[mode] && SUB_NAMES[mode][sub];
  return s ? `${name} · ${s}` : name;
}

/**
 * The control map for a mode: {primary, secondary} lists of {control, action,
 * label} for the hands in their roles, and `roles`. `input` per hand is
 * 'controller' or 'hand' (tracked): a hand has the pinch (trigger), the fist
 * (grip) and, on the secondary hand, the palm turned up (the menu); it has no
 * sticks and no face buttons, whose actions are on the menu.
 * `pvArmed`: a PV path is being drawn, the trigger adds its points.
 */
export function controlMap({ mode = 'explore', sub = null, leftHanded = false, pvArmed = false,
  input = { primary: 'controller', secondary: 'controller' } } = {}) {
  if (!MODES.includes(mode)) mode = 'explore';
  const r = roles(leftHanded);
  // A PV path being drawn (the menu's Analysis tab → Draw) takes the trigger in any mode.
  const trig = pvArmed ? ['pv-point', 'PV path point'] : {
    explore: ['pin-readout', 'pin the readout'],
    select: sub === 'lasso' ? ['select', 'lasso seed'] : sub === 'box' ? ['select', 'box corner'] : ['select', 'spectrum here'],
    clip: sub === 'box' ? ['clip-drag', 'hold: drag a face'] : ['slab-at', 'slab to this channel'],
  }[mode];
  const stickY = { explore: ['threshold', 'threshold'], select: ['threshold', 'threshold'],
    clip: sub === 'box' ? ['clip-size', 'box size'] : ['slab-move', 'slab position'] }[mode];
  const stickX = { explore: ['opacity', 'opacity'], select: [null, ''],
    clip: sub === 'box' ? [null, ''] : ['slab-width', 'slab width'] }[mode];
  const p = r.primary, s = r.secondary;
  const P = buttonNames(p), S = buttonNames(s);
  const primary = [
    { control: 'trigger', name: 'Trigger', action: trig[0], label: trig[1] },
    { control: 'grip', name: 'Grip', action: 'grab', label: 'grab' },
    { control: 'stickY', name: 'Stick ↕', action: stickY[0], label: stickY[1] },
    { control: 'stickX', name: 'Stick ↔', action: stickX[0], label: stickX[1] },
    { control: 'lower', name: P.lower, action: 'next-mode', label: 'next mode' },
    { control: 'upper', name: P.upper, action: 'undo', label: 'undo' },
  ];
  const secondary = [
    { control: 'lower', name: `Hold ${S.lower}`, action: 'menu', label: 'menu' },
    { control: 'upper', name: S.upper, action: 'reset-view', label: 'reset view' },
    { control: 'grip', name: 'Grip', action: 'grab', label: 'grab (both: turn, scale)' },
    { control: 'stickX', name: 'Stick ↔', action: mode === 'explore' ? 'colormap' : 'next-tool',
      label: mode === 'explore' ? 'colour map' : 'tool' },
    { control: 'stickY', name: 'Stick ↕', action: mode === 'explore' ? 'cmax' : null, label: mode === 'explore' ? 'colour max' : '' },
  ];
  const forHand = (list, kind, role) => {
    if (kind !== 'hand') return list.filter((c) => c.action);
    // A tracked hand: pinch, fist, and the palm up on the secondary hand.
    const out = [];
    for (const c of list) {
      if (!c.action) continue;
      if (c.control === 'trigger') out.push({ ...c, name: 'Pinch' });
      else if (c.control === 'grip') out.push({ ...c, name: 'Fist' });
      else if (c.control === 'lower' && role === 'secondary') out.push({ ...c, name: 'Palm up' });
    }
    return out;
  };
  return {
    roles: r, mode, sub,
    primary: forHand(primary, input.primary, 'primary'),
    secondary: forHand(secondary, input.secondary, 'secondary'),
  };
}

/** The action bound to `control` of the hand in `role`, or null. */
export function actionFor(map, role, control) {
  const c = (map[role] || []).find((x) => x.control === control);
  return c ? c.action : null;
}

/** The lines of a controller's label card: [name, what it does]. */
export function labelLines(map, role) {
  return (map[role] || []).map((c) => [c.name, c.label]);
}

/** The control map as text, for the menu's Help page and the 2-D page. */
export function helpLines(opts = {}) {
  const map = controlMap(opts);
  const side = (h) => (h === 'right' ? 'right' : 'left');
  const out = [`${modeLine(map.mode, map.sub, opts)}`];
  out.push(`Primary hand (${side(map.roles.primary)}): ${map.primary.map((c) => `${c.name}: ${c.label}`).join(';  ')}`);
  out.push(`Secondary hand (${side(map.roles.secondary)}): ${map.secondary.map((c) => `${c.name}: ${c.label}`).join(';  ')}`);
  return out;
}

// ── Sticks ──────────────────────────────────────────────────────────────────

export const DEAD_ZONE = 0.15;
export const dead = (v) => (Math.abs(v) < DEAD_ZONE ? 0 : v);

/** A stick pushed sideways as a step (colour map, tool): fires once past
 *  `on`, again only after it came back under `off`. */
export class Flick {
  constructor(on = 0.7, off = 0.3) { this.on = on; this.off = off; this.armed = true; }
  update(v) {
    if (this.armed && Math.abs(v) >= this.on) { this.armed = false; return Math.sign(v); }
    if (!this.armed && Math.abs(v) < this.off) this.armed = true;
    return 0;
  }
}

// ── Undo ────────────────────────────────────────────────────────────────────

/** Snapshots of the look (threshold, colours, opacity, render mode, clipping),
 *  pushed before a change starts; B (or the menu's Undo) puts back the last. */
export class UndoStack {
  constructor(max = 30) { this.max = max; this.items = []; }
  push(snap) {
    const key = JSON.stringify(snap);
    if (this.items.length && this.items[this.items.length - 1].key === key) return false;
    this.items.push({ key, snap: JSON.parse(key) });
    if (this.items.length > this.max) this.items.shift();
    return true;
  }
  pop() { const it = this.items.pop(); return it ? it.snap : null; }
  get size() { return this.items.length; }
  clear() { this.items.length = 0; }
}

// ── Clipping ────────────────────────────────────────────────────────────────

const clamp01 = (v) => Math.max(0, Math.min(1, v));

/** The face of the clipping box nearest to box point p: {axis, end}. */
export function nearestFace(p, clip) {
  let best = null;
  for (let i = 0; i < 3; i++) {
    for (const end of ['min', 'max']) {
      const d = Math.abs(p[i] - clip[end][i]);
      if (!best || d < best.d) best = { axis: i, end, d };
    }
  }
  return { axis: best.axis, end: best.end };
}

/** A copy of `clip` with one face at `value` (0..1), kept `gap` from the other. */
export function moveFace(clip, face, value, gap = 0.01) {
  const out = { min: [...clip.min], max: [...clip.max] };
  const i = face.axis;
  if (face.end === 'min') out.min[i] = Math.max(0, Math.min(out.max[i] - gap, value));
  else out.max[i] = Math.min(1, Math.max(out.min[i] + gap, value));
  return out;
}

/** A copy of `clip` scaled by `f` about its centre (each side at least `gap`). */
export function scaleClip(clip, f, gap = 0.02) {
  const out = { min: [0, 0, 0], max: [1, 1, 1] };
  for (let i = 0; i < 3; i++) {
    const c = (clip.min[i] + clip.max[i]) / 2;
    const h = Math.max(gap / 2, ((clip.max[i] - clip.min[i]) / 2) * f);
    out.min[i] = clamp01(c - h);
    out.max[i] = clamp01(c + h);
  }
  return out;
}

/** The velocity slab {c, w} (centre, half width, box coordinates) kept inside 0..1. */
export function clampSlab(c, w) {
  const ww = Math.max(0.005, Math.min(0.5, w));
  return { c: Math.max(ww, Math.min(1 - ww, c)), w: ww };
}

// ── Haptics ─────────────────────────────────────────────────────────────────

/** Pulses: a press, a result arrived, an error (two pulses). */
export const PULSES = {
  press: [{ intensity: 0.35, ms: 20 }],
  done: [{ intensity: 0.6, ms: 45 }],
  error: [{ intensity: 0.85, ms: 60 }, { intensity: 0.85, ms: 60, after: 120 }],
};

/** Plays pulse `kind` on a gamepad's first haptic actuator; false when it has none. */
export function pulse(gamepad, kind, schedule = (f, ms) => setTimeout(f, ms)) {
  const acts = gamepad && gamepad.hapticActuators;
  const a = acts && acts.length ? acts[0] : null;
  if (!a || typeof a.pulse !== 'function') return false;
  for (const p of PULSES[kind] || []) {
    const go = () => { try { const r = a.pulse(p.intensity, p.ms); if (r && r.catch) r.catch(() => {}); } catch { /* gone */ } };
    if (p.after) schedule(go, p.after); else go();
  }
  return true;
}

/**
 * What a selection's state change should feel like: 'done' when work in
 * progress ended without an error, 'error' when an error appeared, else null.
 * `prev`, `now`: {busy, error}.
 */
export function selectionPulse(prev, now) {
  if (now.error && now.error !== prev.error) return 'error';
  if (prev.busy && !now.busy && !now.error) return 'done';
  return null;
}
