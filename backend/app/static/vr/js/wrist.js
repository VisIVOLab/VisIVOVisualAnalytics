// The wrist cards and the toasts (issue #256). The headset's numbers and
// button help used to stand in view all the time (the frame-rate numbers over
// the secondary hand, a label card above each controller, the Selection
// window as soon as Select mode was on). Now:
// - the secondary wrist (left, right when left-handed) carries a small watch
//   card, 8 cm wide: the cube, the mode and tool, the selection's numbers;
// - the primary wrist carries the legend of the buttons for the tool in use;
// - each card fades in only while its wrist is turned towards the eyes (the
//   watch gesture) and fades out otherwise, so the scene is clear the rest of
//   the time;
// - short messages (a cube shown, a selection done, the mode changed) are a
//   small toast low in the view that fades after about 2 s;
// - the frame-rate and technical numbers only with ?debug (main.js).
//
// The watch gesture: the back of the wrist (where a watch face is) turned
// towards the eyes, the card near the head and near the middle of the view.
// It is the opposite of the hand menu's palm-up gesture (palm towards the
// ceiling): with the palm up the back of the hand faces the floor. A hand
// with its palm up never shows a card, and the secondary card stays hidden
// while the menu is open. The gesture is measured from the hand and the head
// alone: it does not depend on where the menu is drawn.
//
// Pure: unit-tested in backend/tests/vr/wrist.test.mjs. drawWristCard and
// drawToast need only a 2-D context.

import * as M from './mat4.js';

// ── The watch gesture ───────────────────────────────────────────────────────

/** The sign of the controller grip space's x axis that leaves the back of
 *  the hand. The WebXR spec (XRInputSource.gripSpace): "the back of the
 *  user's right hand pointing towards +X and the back of the user's left hand
 *  pointing towards -X"; -Z along the held rod towards the thumb; "+Y roughly
 *  pointing in the direction of the user's arm". */
export const GRIP_BACK_SIGN = { right: 1, left: -1 };
/** From the grip's origin (the centroid of the curled fingers) to the wrist,
 *  in grip space: along +Y, towards the arm. Metres. */
export const GRIP_WRIST = [0, 0.075, 0];
/** The card's lift off the back of the wrist (metres). */
export const LIFT = 0.025;

/**
 * Where the watch face is: {pos, back} in world space, `back` the unit
 * normal that leaves the back of the wrist. `hand`: the page's hand object
 * (controllers: m the grip pose; tracked hands: m the hand frame of hands.js,
 * whose z leaves the palm of a right hand and the back of a left one, and
 * joints.wrist). `handedness`: 'left' | 'right'. Null without a pose.
 */
export function wristFrame(hand, handedness) {
  if (!hand || !hand.m) return null;
  const m = hand.m;
  if (hand.isHand) {
    const z = [m[8], m[9], m[10]];
    const back = M.normalize(handedness === 'left' ? z : M.scale3(z, -1));
    const w = hand.joints && hand.joints.wrist ? hand.joints.wrist.p : [m[12], m[13], m[14]];
    return { pos: M.add(w, M.scale3(back, LIFT * 0.6)), back };
  }
  const x = [m[0], m[1], m[2]];
  const back = M.normalize(M.scale3(x, GRIP_BACK_SIGN[handedness] || 1));
  const w = M.transformPoint(m, GRIP_WRIST);
  return { pos: M.add(w, M.scale3(back, LIFT)), back };
}

/** The gesture's thresholds. facing: back · (card → eye); view: cos of the
 *  angle between the gaze and the card; dist: card to eye (m); ms: how long
 *  the condition must hold before the card changes (no flicker). */
export const WATCH = {
  onFacing: 0.72, offFacing: 0.45,          // within about 44° turns it on, beyond about 63° off
  onView: Math.cos((32 * Math.PI) / 180), offView: Math.cos((45 * Math.PI) / 180),
  onDist: 0.6, offDist: 0.7,
  onMs: 120, offMs: 200,
};

/**
 * What the gesture reads this frame: {facing, view, dist} for a wrist frame
 * `wf` ({pos, back}) and the head's 4×4 transform `head` (column-major, as
 * XRRigidTransform.matrix: the gaze is its -z).
 */
export function watchMeasure(wf, head) {
  const eye = [head[12], head[13], head[14]];
  const to = M.sub(eye, wf.pos);
  const dist = M.length(to);
  if (dist < 1e-6) return { facing: 1, view: 1, dist: 0 };
  const toEye = M.scale3(to, 1 / dist);
  const gaze = M.normalize([-head[8], -head[9], -head[10]]);
  return { facing: M.dot(wf.back, toEye), view: -M.dot(gaze, toEye), dist };
}

/**
 * The watch gesture with hysteresis and a short dwell: on once the wrist has
 * faced the eyes (and been in view, near) for WATCH.onMs, off once it has not
 * for WATCH.offMs. update(measure | null, tMs, blocked) -> boolean; `blocked`
 * (the palm turned up, the menu open on this hand) turns it off at once.
 */
export class WatchGesture {
  constructor(opts = WATCH) { this.o = opts; this.on = false; this.since = null; }
  update(m, t, blocked = false) {
    const o = this.o;
    if (blocked || !m) { this.on = false; this.since = null; return false; }
    const want = this.on
      ? !(m.facing < o.offFacing || m.view < o.offView || m.dist > o.offDist)
      : m.facing > o.onFacing && m.view > o.onView && m.dist < o.onDist;
    if (want === this.on) { this.since = null; return this.on; }
    if (this.since === null) this.since = t;
    if (t - this.since >= (this.on ? o.offMs : o.onMs)) { this.on = want; this.since = null; }
    return this.on;
  }
}

// ── Fading ──────────────────────────────────────────────────────────────────

export const FADE_IN_S = 0.15;
export const FADE_OUT_S = 0.25;
/** The card is redrawn at these opacities only (a few uploads per fade). */
export const ALPHA_STEPS = 4;

/** The opacity after dt seconds towards `target` (0 or 1). */
export function fadeStep(alpha, target, dtS, inS = FADE_IN_S, outS = FADE_OUT_S) {
  const rate = target > alpha ? 1 / inS : 1 / outS;
  const step = Math.max(0, dtS) * rate;
  return target > alpha ? Math.min(target, alpha + step) : Math.max(target, alpha - step);
}

/** The opacity drawn: `alpha` rounded up to one of ALPHA_STEPS levels (0 stays 0). */
export function alphaLevel(alpha, steps = ALPHA_STEPS) {
  if (!(alpha > 0)) return 0;
  return Math.min(1, Math.ceil(alpha * steps - 1e-9) / steps);
}

// ── The cards' pose ─────────────────────────────────────────────────────────

/** The cards: 8 cm wide, their canvas 6000 px/m. The cube card 8 × 6 cm, the
 *  legend 8 × 7.3 cm (up to eight controls). */
export const WRIST_PPM = 6000;
export const WRIST_PX = [480, 360];
export const WRIST_SIZE = [WRIST_PX[0] / WRIST_PPM, WRIST_PX[1] / WRIST_PPM];
export const LEGEND_PX = [480, 440];
export const LEGEND_SIZE = [LEGEND_PX[0] / WRIST_PPM, LEGEND_PX[1] / WRIST_PPM];
/** The rows under the title: the title's line, then ROW_PITCH pixels each. */
const TITLE_Y = 38, FIRST_ROW_Y = 90, ROW_PITCH = 44;
/** How many rows a card of `px` holds under its title. */
export function rowsFor(px) { return Math.floor((px[1] - FIRST_ROW_Y - ROW_PITCH / 2) / ROW_PITCH) + 1; }

/**
 * The card's pose on the wrist frame `wf`: its centre at the watch face, its
 * face halfway between the back of the wrist and the eye (attached to the
 * wrist, still easy to read), its text upright for the head (x along the
 * head's right, `headRight`, in the card's plane).
 */
export function wristCardModel(wf, eye, headRight = [1, 0, 0]) {
  let z = M.sub(eye, wf.pos);
  z = M.length(z) > 1e-6 ? M.normalize(z) : wf.back;
  z = M.add(wf.back, z);
  z = M.length(z) > 1e-6 ? M.normalize(z) : wf.back;
  let x = M.sub(headRight, M.scale3(z, M.dot(headRight, z)));
  if (M.length(x) < 1e-6) x = M.cross([0, 1, 0], z);
  if (M.length(x) < 1e-6) x = [1, 0, 0];
  x = M.normalize(x);
  const y = M.cross(z, x);
  const p = wf.pos;
  return new Float32Array([x[0], x[1], x[2], 0, y[0], y[1], y[2], 0, z[0], z[1], z[2], 0, p[0], p[1], p[2], 1]);
}

// ── The cards' text ─────────────────────────────────────────────────────────

function fmt(v, digits = 3) {
  if (v === null || v === undefined || !Number.isFinite(v)) return 'n/a';
  const a = Math.abs(v);
  if (a !== 0 && (a < 1e-3 || a >= 1e5)) return v.toExponential(digits - 1);
  return v.toPrecision(digits);
}
const count = (n) => (Number.isFinite(n) ? Math.round(n).toLocaleString('en-US') : 'n/a');

/**
 * The selection's numbers in at most three short lines, for the secondary
 * wrist and the toasts. `sel`: select-ui.js's state ({tool, busy, error,
 * lasso, box, spectrum, pick}); `bunit` the cube's unit. The tool in use
 * first; in Explore and Clip the lasso, else the box, else the spectrum.
 */
export function selectionLines(sel, { bunit = '' } = {}) {
  if (!sel) return ['No selection'];
  if (sel.busy) return [String(sel.busy).slice(0, 40)];
  if (sel.error) return [`⚠ ${String(sel.error).slice(0, 38)}`];
  const order = { lasso: ['lasso', 'box', 'spectrum'], box: ['box', 'lasso', 'spectrum'], probe: ['spectrum', 'lasso', 'box'] }[sel.tool]
    || ['lasso', 'box', 'spectrum'];
  for (const k of order) {
    if (k === 'lasso' && sel.lasso) {
      const L = sel.lasso, s = L.stats;
      const out = [`Lasso ${count(L.count)} voxels${L.shared ? ` (${L.shared.source})` : ''}`];
      if (s) {
        out.push(`flux ${fmt(s.flux_jy_kms)} Jy km/s`);
        out.push(Number.isFinite(s.velocity_min_kms) && Number.isFinite(s.velocity_max_kms)
          ? `v ${s.velocity_min_kms.toFixed(1)} to ${s.velocity_max_kms.toFixed(1)} km/s`
          : `ch ${s.channel_min}-${s.channel_max}`);
      }
      return out;
    }
    if (k === 'box' && sel.box && sel.box.stats) {
      const s = sel.box.stats;
      return [`Box ${count(s.valid_voxels)} voxels`, `mean ${fmt(s.mean)} rms ${fmt(s.rms)} ${bunit}`.trim(),
        `flux ${fmt(s.flux_jy_kms)} Jy km/s`];
    }
    if (k === 'spectrum' && sel.spectrum) {
      const sp = sel.spectrum;
      const v = sp.values ? sp.values[sp.channel] : null;
      const peak = sp.values ? sp.values.reduce((a, b) => (Number.isFinite(b) && (a === null || b > a) ? b : a), null) : null;
      return [`Spectrum at (${sp.x}, ${sp.y})`, `here ${fmt(v)} ${sp.unit || bunit}`.trim(), `peak ${fmt(peak)}, ${sp.values ? sp.values.length : 0} ch`];
    }
  }
  if (sel.tool === 'box' && sel.corner) return ['Box: second corner next'];
  return ['No selection'];
}

/**
 * The secondary wrist's lines: [cube, its shape, the mode and tool, the
 * selection's numbers...] (at most WRIST_ROWS). `cube`: {name, shape} or null.
 */
export const WRIST_ROWS = rowsFor(WRIST_PX) + 1;
export function infoLines({ cube = null, modeLine = '', selection = [] } = {}) {
  const out = cube ? [cube.name || 'cube', cube.shape || ''] : ['No cube', 'Open one from the computer'];
  out.push(modeLine);
  out.push(...selection);
  return out.filter((l, i) => l || i < 2).slice(0, WRIST_ROWS);
}

/** The secondary hand's controls the legend keeps: those that change with the mode. */
const SECONDARY_KEEP = new Set(['menu', 'next-tool', 'colormap', 'cmax']);

/**
 * The primary wrist's legend: [name, what it does] rows for the tool in use.
 * The primary hand's controls but the grab (the same everywhere), then the
 * secondary hand's that depend on the mode (and its menu button), named with
 * the hand. `map`: controls.js controlMap.
 */
export function legendRows(map) {
  const rows = (map.primary || []).filter((c) => c.action && c.action !== 'grab').map((c) => [c.name, c.label]);
  const side = map.roles && map.roles.secondary === 'right' ? 'R' : 'L';
  for (const c of map.secondary || []) {
    if (!c.action || !SECONDARY_KEEP.has(c.action)) continue;
    rows.push([/^(Hold [XYAB]|[XYAB]|Palm up)$/.test(c.name) ? c.name : `${side} ${c.name.toLowerCase()}`, c.label]);
  }
  return rows.slice(0, LEGEND_ROWS);
}
/** The legend's rows: Explore has eight controls that do something. */
export const LEGEND_ROWS = rowsFor(LEGEND_PX);

// ── Toasts ──────────────────────────────────────────────────────────────────

export const TOAST_MS = 2000;
export const TOAST_FADE_MS = 400;
/** Messages waiting behind the one on show, at most (the oldest go first). */
export const TOAST_QUEUE = 3;
export const TOAST_PX = [960, 144];
/** 3000 px/m: 32 × 4.8 cm; its 40-px text is 1.3 cm high, about 1.1° at 0.7 m. */
export const TOAST_SIZE = [TOAST_PX[0] / 3000, TOAST_PX[1] / 3000];

/**
 * A notice that is only a short message: information that goes by itself,
 * asks nothing but OK, and says what it has to say in its own `toast` text
 * (the notices opt in: lifecycle.js). In the headset it is a toast, not a card.
 */
export function isToastNotice(n) {
  if (!n || n.level !== 'info' || n.sticky || !(n.autoHideMs > 0)) return false;
  if (typeof n.toast !== 'string' || !n.toast.trim()) return false;
  return (n.actions || []).every((a) => a.id === 'dismiss');
}

/**
 * The short messages: one on show at a time, full for TOAST_MS then fading
 * out over TOAST_FADE_MS, the others waiting in order (at most TOAST_QUEUE;
 * the oldest waiting go first). A message pushed with the key of one waiting
 * or on show replaces it (the mode changed twice: only the last is told).
 */
export class Toasts {
  constructor(now = () => Date.now()) { this.now = now; this.queue = []; this.serial = 0; }
  push(text, key = '') {
    const t = String(text || '').trim();
    if (!t) return;
    const item = { text: t, key, at: null, id: ++this.serial };
    const i = key ? this.queue.findIndex((q) => q.key === key) : -1;
    if (i >= 0) this.queue[i] = item;      // on show (i 0) it starts again with the new text
    else this.queue.push(item);
    while (this.queue.length > TOAST_QUEUE + 1) this.queue.splice(1, 1);
  }
  clear() { this.queue = []; }
  /** {text, alpha, id} of the message on show (id changes with the message), or null. */
  current() {
    const t = this.now();
    while (this.queue.length) {
      const q = this.queue[0];
      if (q.at === null) q.at = t;
      const age = t - q.at;
      if (age >= TOAST_MS + TOAST_FADE_MS) { this.queue.shift(); continue; }
      return { text: q.text, alpha: age <= TOAST_MS ? 1 : 1 - (age - TOAST_MS) / TOAST_FADE_MS, id: q.id };
    }
    return null;
  }
}

/**
 * The toast's pose: `dist` metres along the gaze turned `downDeg` down,
 * facing the eye, upright (low in the view wherever the head looks).
 */
export function toastModel(head, dist = 0.7, downDeg = 20) {
  const eye = [head[12], head[13], head[14]];
  const gaze = M.normalize([-head[8], -head[9], -head[10]]);
  let right = [head[0], 0, head[2]];
  right = M.length(right) > 1e-6 ? M.normalize(right) : [1, 0, 0];
  const down = M.rotation(right, (-downDeg * Math.PI) / 180);
  const d = M.normalize(M.transformPoint(down, gaze));
  const p = M.add(eye, M.scale3(d, dist));
  const z = M.normalize(M.sub(eye, p));
  let x = M.sub(right, M.scale3(z, M.dot(right, z)));
  x = M.length(x) > 1e-6 ? M.normalize(x) : right;
  const y = M.cross(z, x);
  return new Float32Array([x[0], x[1], x[2], 0, y[0], y[1], y[2], 0, z[0], z[1], z[2], 0, p[0], p[1], p[2], 1]);
}

/**
 * The toast when the tools' work ended (`kind` from controls.js
 * selectionPulse: 'done' or 'error'), or ''. `lines`: the selection's
 * numbers (selectionLines) when the selection was the work, else null;
 * `busy`: what was under way ("moment of the selection…").
 */
export function resultToast(kind, lines, error = '', busy = '') {
  if (kind === 'error') return `⚠ ${String(error || 'failed').slice(0, 48)}`;
  if (kind !== 'done') return '';
  if (lines && lines[0] && lines[0] !== 'No selection') return lines.slice(0, 2).join(' · ');
  const what = String(busy || '').replace(/[….\s]+$/u, '');
  return what ? `Done: ${what}` : 'Done';
}

// ── Drawing ─────────────────────────────────────────────────────────────────

function roundRect(c, x, y, w, h, r) {
  c.beginPath();
  c.moveTo(x + r, y);
  c.arcTo(x + w, y, x + w, y + h, r);
  c.arcTo(x + w, y + h, x, y + h, r);
  c.arcTo(x, y + h, x, y, r);
  c.arcTo(x, y, x + w, y, r);
  c.closePath();
}

function fit(c, text, maxW) {
  if (c.measureText(text).width <= maxW) return text;
  let t = text;
  while (t.length > 1 && c.measureText(`${t}…`).width > maxW) t = t.slice(0, -1);
  return `${t}…`;
}

/**
 * A wrist card: `title` (bold, amber), then `rows`, each a string or [name,
 * text] (a legend row: the name in blue, the text after it).
 */
export function drawWristCard(c, title, rows, px = WRIST_PX) {
  const [W, H] = px;
  c.clearRect(0, 0, W, H);
  c.fillStyle = 'rgba(8, 11, 18, 0.92)';
  roundRect(c, 0, 0, W, H, 28);
  c.fill();
  c.strokeStyle = 'rgba(159, 208, 255, 0.85)';
  c.lineWidth = 3;
  roundRect(c, 2, 2, W - 4, H - 4, 26);
  c.stroke();
  c.textBaseline = 'middle';
  c.fillStyle = '#ffd27a';
  c.font = 'bold 32px -apple-system, system-ui, sans-serif';
  c.fillText(fit(c, title || '', W - 40), 20, TITLE_Y);
  let y = FIRST_ROW_Y;
  for (const r of rows.slice(0, rowsFor(px))) {
    if (Array.isArray(r)) {
      c.font = '600 28px -apple-system, system-ui, sans-serif';
      c.fillStyle = '#9fd0ff';
      const name = fit(c, r[0], 150);
      c.fillText(name, 20, y);
      c.fillStyle = '#ffffff';
      c.font = '28px -apple-system, system-ui, sans-serif';
      c.fillText(fit(c, r[1], W - 190), 176, y);
    } else {
      c.fillStyle = '#ffffff';
      c.font = '28px -apple-system, system-ui, sans-serif';
      c.fillText(fit(c, r, W - 40), 20, y);
    }
    y += ROW_PITCH;
  }
}

/** At most `n` lines of at most `maxW` (as `measure` says), the last cut with … */
export function toastLines(text, maxW, measure, n = 2) {
  const out = [];
  let line = '';
  const words = String(text || '').split(/\s+/).filter(Boolean);
  for (let i = 0; i < words.length; i++) {
    const next = line ? `${line} ${words[i]}` : words[i];
    if (measure(next) <= maxW || !line) { line = next; continue; }
    if (out.length === n - 1) { line = `${line} ${words.slice(i).join(' ')}`; break; }
    out.push(line);
    line = words[i];
  }
  if (line) out.push(line);
  return out;
}

/** The toast: one or two lines, centred. */
export function drawToast(c, text) {
  const [W, H] = TOAST_PX;
  c.clearRect(0, 0, W, H);
  c.fillStyle = 'rgba(8, 11, 18, 0.9)';
  roundRect(c, 0, 0, W, H, 48);
  c.fill();
  c.fillStyle = '#ffffff';
  c.font = '600 40px -apple-system, system-ui, sans-serif';
  c.textAlign = 'center';
  c.textBaseline = 'middle';
  const maxW = W - 72;
  const lines = toastLines(text, maxW, (t) => c.measureText(t).width);
  lines.forEach((l, i) => c.fillText(fit(c, l, maxW), W / 2, H / 2 + 1 + (i - (lines.length - 1) / 2) * 50));
  c.textAlign = 'left';
}

/** `src` copied into `dst` at opacity `alpha` (a fading card's frame). */
export function drawFaded(dst, src, alpha) {
  const c = dst.getContext('2d');
  c.clearRect(0, 0, dst.width, dst.height);
  c.globalAlpha = Math.max(0, Math.min(1, alpha));
  c.drawImage(src, 0, 0);
  c.globalAlpha = 1;
}
