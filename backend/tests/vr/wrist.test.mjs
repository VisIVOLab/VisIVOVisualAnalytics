// The wrist cards and the toasts (issue #256): the watch gesture (back of the
// wrist towards the eyes, hysteresis, no flicker, never with the palm up),
// the cards' pose and size, their text, the fade, the toasts (node --test).
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import * as WR from '../../app/static/vr/js/wrist.js';
import * as H from '../../app/static/vr/js/hands.js';
import * as C from '../../app/static/vr/js/controls.js';
import * as M from '../../app/static/vr/js/mat4.js';
import { isToastNotice } from '../../app/static/vr/js/wrist.js';
import { noticeFor } from '../../app/static/vr/js/lifecycle.js';

const close = (a, b, eps = 1e-9) => assert.ok(Math.abs(a - b) <= eps, `${a} != ${b} (±${eps})`);
const deg = (d) => (d * Math.PI) / 180;

/** A head at `pos` looking along yaw (0: -z) and pitch (negative: down). */
function headAt(pos = [0, 1.6, 0], yaw = 0, pitch = 0) {
  let m = M.translation(...pos);
  m = M.multiply(m, M.rotation([0, 1, 0], yaw));
  return M.multiply(m, M.rotation([1, 0, 0], pitch));
}

/** A frame (column-major) with axes x, y and z = x × y at p. */
function frame(x, y, p) {
  x = M.normalize(x); y = M.normalize(y);
  const z = M.cross(x, y);
  return new Float32Array([...x, 0, ...y, 0, ...z, 0, ...p, 1]);
}

/**
 * A controller held in `handedness`'s hand at `p`, the back of the hand
 * along `back` (world), the controller's front along `front`. WebXR grip
 * space: +x to the right of the held controller for both hands, so the back
 * of a right hand is +x and that of a left hand -x; -z the front.
 */
function controller(handedness, p, back, front) {
  const x = handedness === 'right' ? back : M.scale3(back, -1);
  const z = M.scale3(M.normalize(front), -1);
  const y = M.cross(z, M.normalize(x));
  return { m: frame(x, y, p), pos: p, isHand: false };
}

// ── The synthetic tracked hand of hands.test.mjs (a right hand, palm towards +z) ──
const BASE = {
  wrist: [0, -0.045, 0],
  'thumb-metacarpal': [0.028, -0.03, 0.012], 'thumb-phalanx-proximal': [0.045, -0.006, 0.022],
  'index-finger-metacarpal': [0.02, 0, 0], 'index-finger-phalanx-proximal': [0.025, 0.045, 0],
  'middle-finger-metacarpal': [0.003, 0, 0], 'middle-finger-phalanx-proximal': [0.005, 0.047, 0],
  'ring-finger-phalanx-proximal': [-0.015, 0.044, 0],
  'pinky-finger-metacarpal': [-0.025, 0, 0], 'pinky-finger-phalanx-proximal': [-0.035, 0.038, 0],
  'thumb-tip': [0.075, 0.03, 0.035], 'index-finger-tip': [0.027, 0.12, 0.005], 'middle-finger-tip': [0.006, 0.13, 0.005],
  'ring-finger-tip': [-0.016, 0.122, 0.005], 'pinky-finger-tip': [-0.037, 0.1, 0.005],
};
/** Joints of a hand moved by `pose`; a left hand is the right one mirrored in x. */
function joints(pose, handedness = 'right') {
  const out = {};
  for (const [n, p0] of Object.entries(BASE)) {
    const p = handedness === 'left' ? [-p0[0], p0[1], p0[2]] : p0;
    out[n] = { p: M.transformPoint(pose, p), r: 0.008 };
  }
  return out;
}
function trackedHand(pose, handedness) {
  const j = joints(pose, handedness);
  const g = { pinch: false, grip: false };
  return H.handObject(j, g, null, new H.PalmUp().update(j, handedness, false));
}

const head = headAt([0, 1.6, 0], 0, deg(-35));       // looking down at the wrists
const EYE = [0, 1.6, 0];
const WRIST_AT = [-0.05, 1.25, -0.32];              // in front of the chest, below the eyes

test('the back of the wrist: controllers and tracked hands, both hands', () => {
  // Controllers: the back as given, the card lifted off it.
  for (const handedness of ['left', 'right']) {
    const back = M.normalize([0.2, 1, 0.3]);
    const c = controller(handedness, WRIST_AT, back, [1, 0, 0]);
    const wf = WR.wristFrame(c, handedness);
    close(M.dot(wf.back, back), 1, 1e-6);
    // The wrist is 7.5 cm from the grip along its +Y (the spec: towards the
    // arm), the card lifted off its back.
    const gripY = [c.m[4], c.m[5], c.m[6]];
    const off = M.sub(wf.pos, M.position(c.m));
    close(M.dot(off, gripY), 0.075, 1e-6);
    close(M.dot(off, back), WR.LIFT, 1e-6);
    close(M.dot(off, [c.m[8], c.m[9], c.m[10]]), 0, 1e-6);
  }
  // Tracked hands: palm down (back up) for either hand.
  for (const handedness of ['left', 'right']) {
    // Palm towards the floor (the synthetic palm faces +z; +90° about x turns it down).
    const pose = M.multiply(M.translation(...WRIST_AT), M.rotation([1, 0, 0], deg(90)));
    const right = joints(pose, handedness);
    const h = H.handObject(right, { pinch: false, grip: false }, null, false);
    const wf = WR.wristFrame(h, handedness);

    assert.ok(wf.back[1] > 0.95, `${handedness}: the back of the hand is up (${wf.back})`);
    assert.ok(M.length(M.sub(wf.pos, right.wrist.p)) < 0.03, 'on the wrist joint');
  }
  assert.equal(WR.wristFrame(null, 'left'), null);
});

test('the watch pose turns the card on; pointing, a hanging arm, the palm up do not', () => {
  const meas = (c, handedness, h = head) => WR.watchMeasure(WR.wristFrame(c, handedness), h);
  // Watch pose: forearm across the chest, the back of the wrist towards the eyes.
  const toEye = M.normalize(M.sub(EYE, WRIST_AT));
  const watch = meas(controller('left', WRIST_AT, toEye, [1, 0, 0]), 'left');
  assert.ok(watch.facing > WR.WATCH.onFacing && watch.view > WR.WATCH.onView && watch.dist < WR.WATCH.onDist, JSON.stringify(watch));
  // Pointing at the cube: arm out, palm down, the back of the hand up.
  const point = meas(controller('right', [0.25, 1.35, -0.6], [0, 1, 0], [0, 0, -1]), 'right', headAt());
  assert.ok(point.facing < WR.WATCH.onFacing || point.dist > WR.WATCH.onDist, `pointing: ${JSON.stringify(point)}`);
  // Arm hanging by the side: far below the gaze.
  const hang = meas(controller('left', [-0.25, 0.85, 0], [-1, 0, 0], [0, -1, 0]), 'left', headAt());
  assert.ok(hang.view < WR.WATCH.onView);
  // Palm up (the menu's gesture): the back of the hand faces the floor.
  const up = meas(controller('left', WRIST_AT, [0, -1, 0], [1, 0, 0]), 'left');
  assert.ok(up.facing < 0, 'palm up: the back of the hand is away from the eyes');
  // The same with a tracked hand: the palm up is the menu, never the watch.
  const palmUpPose = M.multiply(M.translation(...WRIST_AT), M.rotation([1, 0, 0], deg(-90)));
  const hand = trackedHand(palmUpPose, 'left');
  assert.equal(hand.palmUp, true, 'the synthetic hand reads palm up');
  const g = new WR.WatchGesture();
  for (let t = 0; t <= 1000; t += 14) assert.equal(g.update(WR.watchMeasure(WR.wristFrame(hand, 'left'), head), t, hand.palmUp), false);
});

test('the gesture holds before it changes and keeps hysteresis: no flicker at the edge', () => {
  const g = new WR.WatchGesture();
  const at = (facing, view = 0.95, dist = 0.4) => ({ facing, view, dist });
  // A glance through the pose shorter than onMs does not show it.
  assert.equal(g.update(at(0.9), 0), false);
  assert.equal(g.update(at(0.9), 100), false);
  assert.equal(g.update(at(0.3), 110), false);
  // Held for onMs: on.
  assert.equal(g.update(at(0.9), 200), false);
  assert.equal(g.update(at(0.9), 200 + WR.WATCH.onMs), true);
  // Between the two thresholds it stays on, whatever the noise.
  let t = 400;
  for (let i = 0; i < 200; i++, t += 11) assert.equal(g.update(at(0.585 + 0.12 * Math.sin(i)), t), true);
  // Below the off threshold for less than offMs: still on; then off.
  assert.equal(g.update(at(0.3), t), true);
  assert.equal(g.update(at(0.3), t + WR.WATCH.offMs - 1), true);
  assert.equal(g.update(at(0.3), t + WR.WATCH.offMs), false);
  // Off, noise between the thresholds never turns it on.
  t += 1000;
  for (let i = 0; i < 200; i++, t += 11) assert.equal(g.update(at(0.585 + 0.12 * Math.sin(i)), t), false);
  // Out of view or too far: off even facing the eyes.
  const g2 = new WR.WatchGesture();
  g2.update(at(0.95, 0.2), 0); assert.equal(g2.update(at(0.95, 0.2), 500), false);
  g2.update(at(0.95, 0.95, 0.9), 600); assert.equal(g2.update(at(0.95, 0.95, 0.9), 1200), false);
  // Blocked (the menu, the palm up) or lost: off at once.
  const g3 = new WR.WatchGesture();
  g3.update(at(0.95), 0); assert.equal(g3.update(at(0.95), 200), true);
  assert.equal(g3.update(at(0.95), 210, true), false);
  g3.update(at(0.95), 300); assert.equal(g3.update(at(0.95), 500), true);
  assert.equal(g3.update(null, 510), false);
});

test('the card: 8 cm wide on the wrist, facing between the wrist and the eye, text upright for the head', () => {
  close(WR.WRIST_SIZE[0], 0.08, 1e-9);
  close(WR.LEGEND_SIZE[0], 0.08, 1e-9);
  assert.ok(WR.WRIST_SIZE[1] <= 0.065 && WR.LEGEND_SIZE[1] <= 0.075);
  // The rows fit their card: seven lines on the cube card, eight controls on the legend.
  assert.equal(WR.WRIST_ROWS, 7);
  assert.equal(WR.LEGEND_ROWS, 8);
  const toEye = M.normalize(M.sub(EYE, WRIST_AT));
  const tilted = M.normalize(M.add(toEye, [0.3, 0, 0.2]));
  for (const back of [toEye, tilted]) {
    const wf = WR.wristFrame(controller('left', WRIST_AT, back, [1, 0, 0]), 'left');
    const m = WR.wristCardModel(wf, EYE, [1, 0, 0]);
    close(M.length(M.sub(M.position(m), wf.pos)), 0, 1e-6);
    const z = [m[8], m[9], m[10]], x = [m[0], m[1], m[2]], y = [m[4], m[5], m[6]];
    // Orthonormal, right-handed.
    for (const v of [x, y, z]) close(M.length(v), 1, 1e-6);
    close(M.dot(x, y), 0, 1e-6); close(M.dot(y, z), 0, 1e-6);
    close(M.dot(M.cross(x, y), z), 1, 1e-6);
    // Facing the eye (and the back of the wrist), text along the head's right.
    assert.ok(M.dot(z, M.normalize(M.sub(EYE, wf.pos))) > 0.9);
    assert.ok(M.dot(z, back) > 0.7);
    assert.ok(x[0] > 0.9, `text right ${x}`);
    // Its text at least 0.9° high from 0.45 m (28-px rows on a 360-px, 6-cm card).
    const rowM = (28 / WR.WRIST_PX[1]) * WR.WRIST_SIZE[1];
    assert.ok((2 * Math.atan(rowM / 2 / 0.45) * 180) / Math.PI > 0.55);
  }
});

test('fade: in quicker than out, a few opacity levels only', () => {
  let a = 0, levels = new Set(), frames = 0;
  while (a < 1) { a = WR.fadeStep(a, 1, 1 / 72); levels.add(WR.alphaLevel(a)); frames++; }
  assert.ok(frames <= Math.ceil(WR.FADE_IN_S * 72) + 1, `${frames} frames`);
  assert.ok(levels.size <= WR.ALPHA_STEPS, `uploads while fading in: ${levels.size}`);
  frames = 0;
  while (a > 0) { a = WR.fadeStep(a, 0, 1 / 72); frames++; }
  assert.ok(frames > Math.floor(WR.FADE_IN_S * 72));
  assert.equal(WR.alphaLevel(0), 0);
  assert.equal(WR.alphaLevel(1e-4), 1 / WR.ALPHA_STEPS);
  assert.equal(WR.alphaLevel(1), 1);
  assert.equal(WR.fadeStep(0.5, 0.5, 1), 0.5);
});

test('the secondary card: cube, mode and the selection\'s numbers', () => {
  const lasso = { tool: 'lasso', busy: '', error: '', lasso: { count: 12345, stats: { flux_jy_kms: 3.21, velocity_min_kms: 512.3, velocity_max_kms: 640.04 } } };
  assert.deepEqual(WR.selectionLines(lasso), ['Lasso 12,345 voxels', 'flux 3.21 Jy km/s', 'v 512.3 to 640.0 km/s']);
  const box = { tool: 'box', box: { stats: { valid_voxels: 800, mean: 0.0123, rms: 0.002, flux_jy_kms: 1.5 } } };
  assert.deepEqual(WR.selectionLines(box, { bunit: 'Jy/beam' }), ['Box 800 voxels', 'mean 0.0123 rms 0.00200 Jy/beam', 'flux 1.50 Jy km/s']);
  const spec = { tool: 'probe', spectrum: { x: 10, y: 20, values: [1, 5, 2], channel: 2, unit: 'Jy/beam' } };
  assert.deepEqual(WR.selectionLines(spec), ['Spectrum at (10, 20)', 'here 2.00 Jy/beam', 'peak 5.00, 3 ch']);
  // The tool in use first, others as fallbacks; busy and errors first of all.
  assert.match(WR.selectionLines({ ...box, lasso: lasso.lasso, tool: 'box' })[0], /^Box/);
  assert.match(WR.selectionLines({ ...box, lasso: lasso.lasso, tool: 'probe' })[0], /^Lasso/);
  assert.deepEqual(WR.selectionLines({ ...lasso, busy: 'Kinematic Lasso on the full cube…' }), ['Kinematic Lasso on the full cube…']);
  assert.match(WR.selectionLines({ ...lasso, error: 'no emission there' })[0], /^⚠ no emission/);
  assert.deepEqual(WR.selectionLines({ tool: 'probe' }), ['No selection']);
  assert.deepEqual(WR.selectionLines(null), ['No selection']);
  const lines = WR.infoLines({ cube: { name: 'ngc3137.fits', shape: '512×512×300, bin 2×2×1' }, modeLine: 'SELECT · Kinematic Lasso', selection: WR.selectionLines(lasso) });
  assert.deepEqual(lines, ['ngc3137.fits', '512×512×300, bin 2×2×1', 'SELECT · Kinematic Lasso', 'Lasso 12,345 voxels', 'flux 3.21 Jy km/s', 'v 512.3 to 640.0 km/s']);
  // More lines than the card holds: cut to its rows.
  const many = WR.infoLines({ cube: { name: 'a', shape: 'b' }, modeLine: 'm', selection: ['1', '2', '3', '4', '5', '6'] });
  assert.equal(many.length, WR.WRIST_ROWS);
  assert.equal(WR.infoLines({ modeLine: 'EXPLORE' })[0], 'No cube');
  assert.ok(lines.length <= WR.WRIST_ROWS);
});

test('the primary card: the legend of the tool in use, for either hand, controllers or hands', () => {
  for (const leftHanded of [false, true]) {
    const r = C.roles(leftHanded), P = C.buttonNames(r.primary), S = C.buttonNames(r.secondary);
    const lasso = WR.legendRows(C.controlMap({ mode: 'select', sub: 'lasso', leftHanded }));
    assert.deepEqual(lasso[0], ['Trigger', 'lasso seed']);
    assert.ok(!lasso.some(([, t]) => t === 'grab'), 'the grab is the same everywhere: not in the legend');
    assert.ok(lasso.some(([n, t]) => n === P.lower && t === 'next mode'));
    assert.ok(lasso.some(([n, t]) => n === `Hold ${S.lower}` && t === 'menu'));
    assert.ok(lasso.some(([n, t]) => n === `${r.secondary === 'right' ? 'R' : 'L'} stick ↔` && t === 'tool'));
    const L = r.secondary === 'right' ? 'R' : 'L';
    // Every mode and tool: every control that does something now is in the
    // legend (none cut away), but the grab, and the secondary's upper button
    // (reset view, the same in every mode).
    for (const [mode, sub] of [['explore', null], ['select', 'probe'], ['select', 'lasso'], ['select', 'box'], ['clip', 'slab'], ['clip', 'box']]) {
      const map = C.controlMap({ mode, sub, leftHanded });
      const rows = WR.legendRows(map);
      assert.ok(rows.length <= WR.LEGEND_ROWS, `${mode} ${sub}: ${rows.length} rows`);
      const want = map.primary.filter((c) => c.action !== 'grab').length
        + map.secondary.filter((c) => ['menu', 'next-tool', 'colormap', 'cmax'].includes(c.action)).length;
      assert.equal(rows.length, want, `${mode} ${sub}: ${JSON.stringify(rows)}`);
    }
    const explore = WR.legendRows(C.controlMap({ mode: 'explore', leftHanded }));
    assert.ok(explore.some(([n, t]) => n === `${L} stick ↔` && t === 'colour map'));
    assert.ok(explore.some(([n, t]) => n === `${L} stick ↕` && t === 'colour max'));
    const slab = WR.legendRows(C.controlMap({ mode: 'clip', sub: 'slab', leftHanded }));
    assert.deepEqual(slab.slice(0, 3), [['Trigger', 'range to this channel'], ['Stick ↕', 'range position'], ['Stick ↔', 'range width']]);
    assert.ok(slab.some(([n, t]) => n === `${L} stick ↔` && t === 'tool'));
  }
  const hands = WR.legendRows(C.controlMap({ mode: 'explore', input: { primary: 'hand', secondary: 'hand' } }));
  assert.deepEqual(hands, [['Pinch', 'pin the readout'], ['Palm up', 'menu']]);
});

test('toasts: the short notices that opt in, one after the other, each about 2 s', () => {
  const desk = noticeFor('cube-from-desktop', { detail: 'a.fits', done: true });
  assert.ok(isToastNotice(desk));
  assert.match(desk.toast, /^Showing a\.fits from the computer$/);
  // The recovery notices keep their instruction in the toast.
  for (const k of ['restarted', 'reopened']) {
    assert.ok(isToastNotice(noticeFor(k)), k);
    assert.match(noticeFor(k).toast, /Repeat an unfinished selection/);
  }
  // Anything that asks for a choice, an error, a sticky notice, the guide, or
  // a notice without its own toast text (its body matters) stays a card.
  for (const n of [noticeFor('xr-ended'), noticeFor('auth', {}), noticeFor('cube-waiting', { inVR: true }), noticeFor('cube-waiting', {}),
    noticeFor('budget', {}), { id: 'guide', level: 'guide', actions: [] },
    { id: 'x', level: 'info', sticky: true, autoHideMs: 10, toast: 'x', actions: [] },
    { id: 'measure-msg', level: 'info', autoHideMs: 5000, title: 'Measurement run', lines: ['Not started: no cube.'], actions: [{ id: 'dismiss' }] }]) {
    assert.equal(isToastNotice(n), false, n.id);
  }
  let now = 0;
  const t = new WR.Toasts(() => now);
  assert.equal(t.current(), null);
  t.push('  ');
  assert.equal(t.current(), null, 'nothing to say: no toast');
  t.push('Lasso 12,345 voxels');
  const first = t.current();
  assert.equal(first.text, 'Lasso 12,345 voxels'); assert.equal(first.alpha, 1);
  now = WR.TOAST_MS; assert.equal(t.current().alpha, 1);
  now = WR.TOAST_MS + WR.TOAST_FADE_MS / 2; close(t.current().alpha, 0.5);
  now = WR.TOAST_MS + WR.TOAST_FADE_MS; assert.equal(t.current(), null);
  // Two messages: both told, in order, the second from the end of the first.
  now = 10000;
  t.push('a'); t.push('b');
  const a = t.current();
  assert.equal(a.text, 'a');
  now += WR.TOAST_MS + WR.TOAST_FADE_MS;
  const b = t.current();
  assert.equal(b.text, 'b'); assert.equal(b.alpha, 1); assert.notEqual(b.id, a.id);
  now += WR.TOAST_MS + WR.TOAST_FADE_MS; assert.equal(t.current(), null);
  // The same key replaces: the mode changed three times, the last one is told.
  t.push('EXPLORE', 'mode'); t.push('lasso done', 'result'); t.push('SELECT', 'mode'); t.push('CLIP', 'mode');
  assert.equal(t.current().text, 'CLIP');
  now += WR.TOAST_MS + WR.TOAST_FADE_MS;
  assert.equal(t.current().text, 'lasso done');
  // A burst: at most TOAST_QUEUE wait behind the one on show, the oldest waiting go.
  t.clear();
  for (let i = 0; i < 10; i++) t.push(`m${i}`);
  const told = [];
  for (let k = 0; k < 12; k++) { const c = t.current(); if (!c) break; told.push(c.text); now += WR.TOAST_MS + WR.TOAST_FADE_MS; }
  assert.deepEqual(told, ['m0', 'm7', 'm8', 'm9']);
  assert.ok(WR.TOAST_MS >= 1500 && WR.TOAST_MS <= 2500);
});

test('the shell hands short notices to the toasts, oldest first, and keeps the others as cards', async () => {
  const { createShell } = await import('../../app/static/vr/js/shell.js');
  let clock = 1000;
  const realNow = Date.now;
  Date.now = () => clock;
  try {
    const canvas = () => ({ getContext: () => new Proxy({}, { get: () => () => ({ width: 1 }), set: () => true }) });
    const shell = createShell({
      page: { build: 'test', api: 1 }, renderer: () => ({ createPanel: () => ({}), updatePanel() {} }), backend: () => null, token: () => '',
      ctx: { inVR: () => true, summary: () => ({}) },
      doc: { createElement: canvas, getElementById: () => null },
      win: { location: { hostname: 'localhost', protocol: 'http:', host: 'localhost', pathname: '/vr/' }, navigator: { userAgent: 'node' },
        addEventListener() {}, setInterval() {} },
    });
    const told = [];
    shell.notify('reopened'); clock += 10;
    shell.notify('cube-from-desktop', { detail: 'new.fits', done: true });
    assert.equal(shell.cardForFrame(M.identity(), { onToast: (text, key) => told.push([text, key]) }), null);
    assert.deepEqual(told.map((t) => t[1]), ['reopened', 'cube-from-desktop']);
    assert.match(told[1][0], /new\.fits/);
    // Shown at the same instant: still in the order they were shown.
    told.length = 0;
    shell.notify('reopened'); shell.notify('cube-from-desktop', { detail: 'b.fits', done: true });
    shell.cardForFrame(M.identity(), { onToast: (text, key) => told.push(key) });
    assert.deepEqual(told, ['reopened', 'cube-from-desktop']);
    told.length = 0;
    shell.notify('cube-from-desktop', { detail: 'c.fits', done: true }); shell.notify('restarted');
    shell.cardForFrame(M.identity(), { onToast: (text, key) => told.push(key) });
    assert.deepEqual(told, ['cube-from-desktop', 'restarted']);
    // A notice that asks for a choice is still the card.
    shell.notify('xr-ended');
    const card = shell.cardForFrame(M.identity(), { onToast: () => assert.fail('not a toast') });
    assert.equal(card && card.id, 'xr-ended');
    // Without onToast (the desktop's 3-D view) the short notice is the card, as before.
    shell.clearNotice('xr-ended');
    shell.notify('reopened');
    assert.equal(shell.cardForFrame(M.identity()).id, 'reopened');
  } finally {
    Date.now = realNow;
  }
});

test('the toast stands low in the view, facing the eye, wherever the head looks', () => {
  for (const [yaw, pitch] of [[0, 0], [deg(70), 0], [deg(-120), deg(-30)], [0, deg(25)]]) {
    const h = headAt([0.3, 1.5, -0.2], yaw, pitch);
    const m = WR.toastModel(h);
    const eye = [h[12], h[13], h[14]];
    const to = M.normalize(M.sub(M.position(m), eye));
    const gaze = M.normalize([-h[8], -h[9], -h[10]]);
    const up = [h[4], h[5], h[6]];
    const angle = (Math.acos(M.dot(to, gaze)) * 180) / Math.PI;
    close(angle, 20, 1);
    assert.ok(M.dot(to, up) < 0, 'below the gaze');
    close(M.length(M.sub(M.position(m), eye)), 0.7, 1e-6);
    assert.ok(M.dot([m[8], m[9], m[10]], M.scale3(to, -1)) > 0.999, 'facing the eye');
  }
  close(WR.TOAST_SIZE[0], 0.32, 1e-9);
});

test('result toasts: the selection\'s numbers, what ended, or the error', () => {
  assert.equal(WR.resultToast('done', ['Lasso 12 voxels', 'flux 1 Jy km/s', 'v']), 'Lasso 12 voxels · flux 1 Jy km/s');
  assert.equal(WR.resultToast('done', null, '', 'moment of the selection…'), 'Done: moment of the selection');
  assert.equal(WR.resultToast('done', ['No selection']), 'Done');
  assert.equal(WR.resultToast('error', null, 'the backend refused'), '⚠ the backend refused');
  assert.equal(WR.resultToast(null, ['x']), '');
});

test('toast text: one line, or two with the rest of the words in the second', () => {
  const m = (t) => t.length * 10;
  assert.deepEqual(WR.toastLines('short one', 200, m), ['short one']);
  assert.deepEqual(WR.toastLines('The cube was opened again. Repeat an unfinished selection.', 300, m),
    ['The cube was opened again.', 'Repeat an unfinished selection.']);
  const three = WR.toastLines('aaaa bbbb cccc dddd eeee ffff', 80, m);
  assert.equal(three.length, 2);
  assert.equal(three[1], 'bbbb cccc dddd eeee ffff', 'the second line keeps the rest (drawn cut with …)');
  assert.deepEqual(WR.toastLines('', 100, m), []);
});

test('drawing runs on a 2-D context stand-in', () => {
  const calls = [];
  const c = new Proxy({}, {
    get: (o, k) => (k in o ? o[k] : k === 'measureText' ? (t) => ({ width: String(t).length * 14 }) : () => calls.push(k)),
    set: (o, k, v) => { o[k] = v; return true; },
  });
  WR.drawWristCard(c, 'SELECT · Kinematic Lasso', WR.legendRows(C.controlMap({ mode: 'select', sub: 'lasso' })), WR.LEGEND_PX);
  WR.drawWristCard(c, 'ngc3137.fits', ['512×512×300', 'a very long line that does not fit on the card at all, it is cut']);
  WR.drawToast(c, 'Showing ngc3137.fits from the computer');
  const dst = { width: 4, height: 4, getContext: () => c };
  WR.drawFaded(dst, {}, 0.5);
  assert.equal(c.globalAlpha, 1);
  assert.ok(calls.includes('fillText') && calls.includes('drawImage'));
});

test('main.js: no card above the controllers, the numbers only with ?debug, toasts from the notices', () => {
  const main = readFileSync(new URL('../../app/static/vr/js/main.js', import.meta.url), 'utf8');
  assert.ok(!/RO\.labelModel|RO\.drawLabels|modeTagTexture/.test(main), 'the controller labels are gone');
  assert.match(main, /const hudOn = \(DEBUG_HUD \|\| !!xr\.bench\) && !CUT\.nohud;/);
  assert.match(main, /shell\.cardForFrame\(pose\.transform\.matrix, \{ onToast: toast \}\)/);
  // The secondary card makes way for the menu and for the palm up.
  assert.match(main, /h && h\.palmUp\) \|\| \(card\.role === 'secondary' && ui\.menu\.open\)/);
  // A new session starts clean: no gesture, opacity or waiting toast carried over.
  const sync = main.slice(main.indexOf('function syncWristSession()'), main.indexOf('function toast('));
  assert.ok(/card\.alpha = 0/.test(sync) && /new WR\.WatchGesture\(\)/.test(sync) && /toasts\.clear\(\)/.test(sync), sync);
  assert.match(main.slice(main.indexOf('function toast('), main.indexOf('function wristCube')), /syncWristSession\(\);\s*toasts\.push/);
  // Card textures are 2-D panels (issue #229: no 3-D allocation in a session).
  const tail = main.slice(main.indexOf('// ── The wrist cards and the toast'), main.indexOf('/** The primary trigger'));
  assert.ok(!/texImage3D|texStorage3D/.test(tail));
});
