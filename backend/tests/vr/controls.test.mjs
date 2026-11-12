// The headset's controls (issue #201): hand roles, modes, one meaning per
// control, the left-handed setting, tracked hands, the labels, undo, the
// clipping helpers, haptics and the cursor readout (node --test).
import test from 'node:test';
import assert from 'node:assert/strict';
import * as C from '../../app/static/vr/js/controls.js';
import * as RO from '../../app/static/vr/js/readout.js';
import * as M from '../../app/static/vr/js/mat4.js';

const act = (map, role, control) => C.actionFor(map, role, control);
const close = (a, b, eps = 1e-9) => assert.ok(Math.abs(a - b) <= eps, `${a} != ${b}`);

test('roles: the right hand points by default, the left with the left-handed setting', () => {
  assert.deepEqual(C.roles(false), { primary: 'right', secondary: 'left' });
  assert.deepEqual(C.roles(true), { primary: 'left', secondary: 'right' });
  assert.deepEqual(C.buttonNames('right'), { lower: 'A', upper: 'B' });
  assert.deepEqual(C.buttonNames('left'), { lower: 'X', upper: 'Y' });
});

test('one meaning per control in each mode', () => {
  const ex = C.controlMap({ mode: 'explore' });
  assert.equal(act(ex, 'primary', 'trigger'), 'pin-readout');
  assert.equal(act(ex, 'primary', 'stickY'), 'threshold');
  assert.equal(act(ex, 'primary', 'stickX'), 'opacity');
  assert.equal(act(ex, 'secondary', 'stickX'), 'colormap');
  assert.equal(act(ex, 'secondary', 'stickY'), 'cmax');
  for (const [sub, label] of [['probe', 'spectrum here'], ['lasso', 'lasso seed'], ['box', 'box corner']]) {
    const m = C.controlMap({ mode: 'select', sub });
    assert.equal(act(m, 'primary', 'trigger'), 'select');
    assert.equal(m.primary.find((c) => c.control === 'trigger').label, label);
    assert.equal(act(m, 'primary', 'stickY'), 'threshold');
    assert.equal(act(m, 'primary', 'stickX'), null);
    assert.equal(act(m, 'secondary', 'stickX'), 'next-tool');
  }
  // A PV path being drawn takes the trigger in any mode; an unknown mode is Explore.
  for (const mode of ['explore', 'select', 'clip']) {
    assert.equal(act(C.controlMap({ mode, sub: 'lasso', pvArmed: true }), 'primary', 'trigger'), 'pv-point');
  }
  assert.equal(act(C.controlMap({ mode: 'nope' }), 'primary', 'stickY'), 'threshold');
  const slab = C.controlMap({ mode: 'clip', sub: 'slab' });
  assert.equal(act(slab, 'primary', 'trigger'), 'slab-at');
  assert.equal(act(slab, 'primary', 'stickY'), 'slab-move');
  assert.equal(act(slab, 'primary', 'stickX'), 'slab-width');
  const box = C.controlMap({ mode: 'clip', sub: 'box' });
  assert.equal(act(box, 'primary', 'trigger'), 'clip-drag');
  assert.equal(act(box, 'primary', 'stickY'), 'clip-size');
  // The buttons keep global actions only, the same in every mode.
  for (const opts of [{ mode: 'explore' }, { mode: 'select', sub: 'box' }, { mode: 'clip', sub: 'slab' }]) {
    const m = C.controlMap(opts);
    assert.equal(act(m, 'primary', 'lower'), 'next-mode');
    assert.equal(act(m, 'primary', 'upper'), 'undo');
    assert.equal(act(m, 'secondary', 'lower'), 'menu');
    assert.equal(act(m, 'secondary', 'upper'), 'reset-view');
    assert.equal(act(m, 'primary', 'grip'), 'grab');
    assert.equal(act(m, 'secondary', 'grip'), 'grab');
    assert.equal(act(m, 'secondary', 'trigger'), null, 'the menu hand does not act');
    // No control carries two actions.
    for (const role of ['primary', 'secondary']) {
      const controls = m[role].map((c) => c.control);
      assert.equal(new Set(controls).size, controls.length);
    }
  }
});

test('left-handed: the same map on the other hands, with their buttons\' names', () => {
  const right = C.controlMap({ mode: 'select', sub: 'lasso' });
  const left = C.controlMap({ mode: 'select', sub: 'lasso', leftHanded: true });
  assert.deepEqual(left.roles, { primary: 'left', secondary: 'right' });
  assert.deepEqual(left.primary.map((c) => c.action), right.primary.map((c) => c.action));
  assert.deepEqual(left.secondary.map((c) => c.action), right.secondary.map((c) => c.action));
  assert.deepEqual(C.labelLines(right, 'primary').filter(([n]) => n.length === 1), [['A', 'next mode'], ['B', 'undo']]);
  assert.deepEqual(C.labelLines(left, 'primary').filter(([n]) => n.length === 1), [['X', 'next mode'], ['Y', 'undo']]);
  assert.deepEqual(C.labelLines(left, 'secondary').slice(0, 2), [['Hold A', 'menu'], ['B', 'reset view']]);
  assert.deepEqual(C.labelLines(right, 'secondary').slice(0, 2), [['Hold X', 'menu'], ['Y', 'reset view']]);
});

test('tracked hands: pinch acts, fist grabs, the secondary palm up opens the menu; nothing else', () => {
  for (const leftHanded of [false, true]) {
    const m = C.controlMap({ mode: 'select', sub: 'lasso', leftHanded, input: { primary: 'hand', secondary: 'hand' } });
    assert.deepEqual(m.primary.map((c) => [c.control, c.name, c.action]),
      [['trigger', 'Pinch', 'select'], ['grip', 'Fist', 'grab']]);
    assert.deepEqual(m.secondary.map((c) => [c.control, c.name, c.action]),
      [['lower', 'Palm up', 'menu'], ['grip', 'Fist', 'grab']]);
    // The primary hand's palm (its button 4 too) is no mode switch.
    assert.equal(act(m, 'primary', 'lower'), null);
  }
  // One hand on a controller, the other tracked: each keeps its own controls.
  const mixed = C.controlMap({ mode: 'explore', input: { primary: 'controller', secondary: 'hand' } });
  assert.equal(act(mixed, 'primary', 'stickY'), 'threshold');
  assert.equal(act(mixed, 'secondary', 'lower'), 'menu');
  assert.equal(act(mixed, 'secondary', 'stickX'), null);
  // The tracked hand's buttons sit where a controller's are (hands.js handObject).
  assert.equal(C.BUTTON_INDEX.trigger, 0);
  assert.equal(C.BUTTON_INDEX.grip, 1);
  assert.equal(C.BUTTON_INDEX.lower, 4);
});

test('modes and tools step around; the mode line names both', () => {
  assert.equal(C.nextMode('explore'), 'select');
  assert.equal(C.nextMode('clip'), 'explore');
  assert.equal(C.nextMode('explore', -1), 'clip');
  assert.equal(C.nextSub('select', 'box'), 'probe');
  assert.equal(C.nextSub('select', 'probe', -1), 'box');
  assert.equal(C.nextSub('clip', 'slab'), 'box');
  assert.equal(C.nextSub('explore', null), null);
  assert.equal(C.modeLine('select', 'lasso'), 'SELECT · Kinematic Lasso');
  assert.equal(C.modeLine('select', 'lasso', { pvArmed: true }), 'SELECT · PV path');
  assert.equal(C.modeLine('clip', 'box'), 'CLIP · clipping box');
  assert.equal(C.modeLine('explore', null), 'EXPLORE');
  const h = C.helpLines({ mode: 'clip', sub: 'slab' });
  assert.equal(h[0], 'CLIP · velocity slab');
  assert.match(h[1], /Primary hand \(right\).*Stick ↕: slab position/);
  assert.match(h[2], /Secondary hand \(left\).*Hold X: menu/);
});

test('a flick steps once per push, and re-arms only near the centre', () => {
  const f = new C.Flick();
  const seq = [0, 0.5, 0.8, 0.9, 0.6, 0.4, 0.2, -0.75, -0.9, 0, 0.71].map((v) => f.update(v));
  assert.deepEqual(seq, [0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 1]);
  assert.equal(C.dead(0.1), 0);
  assert.equal(C.dead(-0.5), -0.5);
});

test('undo: snapshots pushed before changes, a repeat not twice, bounded', () => {
  const u = new C.UndoStack(3);
  assert.ok(u.push({ a: 1 }));
  assert.ok(!u.push({ a: 1 }), 'the same look is one step');
  u.push({ a: 2 }); u.push({ a: 3 }); u.push({ a: 4 });
  assert.equal(u.size, 3);
  const s = u.pop();
  s.a = 99;                                   // a copy: changing it does not change the stack
  assert.deepEqual(u.pop(), { a: 3 });
  assert.deepEqual(u.pop(), { a: 2 });
  assert.equal(u.pop(), null);
});

test('clipping: the nearest face follows, faces stay apart, the box scales about its centre', () => {
  const clip = { min: [0.1, 0.2, 0.3], max: [0.9, 0.8, 0.7] };
  assert.deepEqual(C.nearestFace([0.12, 0.5, 0.5], clip), { axis: 0, end: 'min' });
  assert.deepEqual(C.nearestFace([0.5, 0.5, 0.69], clip), { axis: 2, end: 'max' });
  assert.deepEqual(C.nearestFace([0.5, 0.79, 0.5], clip), { axis: 1, end: 'max' });
  const m = C.moveFace(clip, { axis: 2, end: 'max' }, 0.1);
  close(m.max[2], 0.31); close(m.min[2], 0.3);
  assert.deepEqual(clip.max, [0.9, 0.8, 0.7], 'not changed in place');
  close(C.moveFace(clip, { axis: 0, end: 'min' }, -2).min[0], 0);
  const s = C.scaleClip(clip, 0.5);
  close(s.min[0], 0.3); close(s.max[0], 0.7); close(s.min[2], 0.4); close(s.max[2], 0.6);
  const g = C.scaleClip({ min: [0, 0, 0], max: [1, 1, 1] }, 3);
  assert.deepEqual(g, { min: [0, 0, 0], max: [1, 1, 1] });
  const k = C.clampSlab(0.98, 0.1);
  close(k.c, 0.9); close(k.w, 0.1);
  close(C.clampSlab(0.5, 2).w, 0.5);
});

test('haptics: one pulse on a press, two on an error, none without an actuator', () => {
  const calls = [];
  const later = [];
  const gp = { hapticActuators: [{ pulse: (i, ms) => { calls.push([i, ms]); return Promise.resolve(true); } }] };
  assert.ok(C.pulse(gp, 'press', (f, ms) => later.push([f, ms])));
  assert.equal(calls.length, 1);
  assert.ok(C.pulse(gp, 'error', (f, ms) => later.push([f, ms])));
  assert.equal(calls.length, 2);
  assert.equal(later.length, 1);
  assert.equal(later[0][1], 120);
  later[0][0]();
  assert.equal(calls.length, 3);
  assert.equal(C.pulse({ hapticActuators: [] }, 'press'), false);
  assert.equal(C.pulse({}, 'press'), false);
  assert.equal(C.pulse(null, 'press'), false);
  assert.equal(C.pulse({ hapticActuators: [{ pulse() { throw new Error('gone'); } }] }, 'press'), true);
  assert.equal(C.selectionPulse({ busy: 'lasso…', error: '' }, { busy: '', error: '' }), 'done');
  assert.equal(C.selectionPulse({ busy: 'lasso…', error: '' }, { busy: '', error: 'no backend' }), 'error');
  assert.equal(C.selectionPulse({ busy: '', error: 'x' }, { busy: '', error: 'x' }), null);
  assert.equal(C.selectionPulse({ busy: '', error: '' }, { busy: 'picking…', error: '' }), null);
});

// A small cube with RA/Dec/velocity axes as /v1/cube/reduced_info gives them.
function cubeFixture() {
  const dims = [4, 4, 4];
  const data = new Uint8Array(64).fill(1);
  data[(2 * 4 + 1) * 4 + 3] = 255;            // voxel x 3, y 1, z 2 at its brightest
  const lin = (a, b, n) => Array.from({ length: n }, (_, i) => a + (b - a) * (i / (n - 1)));
  const info = {
    full_shape: [8, 8, 4], factors: [2, 2, 1], reduced_shape: [4, 4, 4],
    axes: {
      x: { name: 'RA', unit: 'deg', world: lin(150.02, 149.98, 4) },
      y: { name: 'DEC', unit: 'deg', world: lin(1.98, 2.02, 4) },
      z: { name: 'VRAD', unit: 'm/s', world: lin(1e6, 1.015e6, 4), velocity_kms: lin(1000, 1015, 4),
        full_velocity_kms: lin(1000, 1015, 4), velocity_convention: 'radio' },
    },
  };
  const cube = { data, dims, encoding: 'uint8', lo: 0, hi: 2, bunit: 'Jy/beam', factors: [2, 2, 1] };
  return { info, cube };
}

test('the readout: sky, velocity, value and unit at the point, the mode first', () => {
  const { info, cube } = cubeFixture();
  const p = [(3 + 0.5) / 4, (1 + 0.5) / 4, (2 + 0.5) / 4];        // the centre of reduced voxel (3, 1, 2)
  const lines = RO.readoutLines('SELECT · Kinematic Lasso', p, { info, cube, texScale: [1, 1, 1] });
  assert.equal(lines[0], 'SELECT · Kinematic Lasso');
  assert.match(lines[1], /^RA 09:59:5\d\.\d\d {2}Dec \+0[12]:\d\d:\d\d\.\d$/);
  assert.match(lines[2], /^v 1010\.00 km\/s \(radio\) {2}ch 2$/);
  assert.equal(lines[3], '2.000 Jy/beam (binned 2×2×1)');
  assert.deepEqual(RO.readoutLines('EXPLORE', null, { info, cube }), ['EXPLORE']);
  // A blank voxel (code 0) reads as blank.
  cube.data.fill(0);
  assert.match(RO.readoutLines('X', p, { info, cube, texScale: [1, 1, 1] })[3], /^blank/);
});

test('the readout stays cheap: well under a tenth of a millisecond a call', () => {
  const { info, cube } = cubeFixture();
  const t0 = performance.now();
  for (let i = 0; i < 2000; i++) RO.readoutLines('EXPLORE', [(i % 7) / 7, 0.3, 0.6], { info, cube, texScale: [1, 1, 1] });
  const us = (1000 * (performance.now() - t0)) / 2000;
  assert.ok(us < 100, `${us.toFixed(1)} µs per call`);
});

test('cards face the eye: the readout above its point, the labels above the controller', () => {
  const eye = [0, 1.6, 0];
  const at = [0.2, 1.3, -0.8];
  const m = RO.readoutModel(at, eye);
  const z = M.normalize(M.sub(M.transformPoint(m, [0, 0, 1]), M.position(m)));
  const toEye = M.normalize(M.sub(eye, M.position(m)));
  assert.ok(M.dot(z, toEye) > 0.999);
  assert.ok(M.position(m)[1] > at[1]);
  const grip = M.multiply(M.translation(0.25, 1.1, -0.4), M.rotation([1, 0, 0], -0.5));
  const lm = RO.labelModel(grip, eye);
  assert.ok(M.position(lm)[1] > 1.1, 'above the controller');
  const up = M.normalize(M.sub(M.transformPoint(lm, [0, 1, 0]), M.position(lm)));
  const across = M.normalize(M.sub(M.transformPoint(lm, [1, 0, 0]), M.position(lm)));
  assert.ok(up[1] > 0.5 && Math.abs(across[1]) < 1e-6, 'level, tilted toward the eye');
  const lz = M.normalize(M.sub(M.transformPoint(lm, [0, 0, 1]), M.position(lm)));
  assert.ok(M.dot(lz, M.normalize(M.sub(eye, M.position(lm)))) > 0.999);
});

test('drawReadout and drawLabels run on a 2-D context stand-in', () => {
  const calls = [];
  const c = new Proxy({}, { get: (t, k) => (k in t ? t[k] : (...a) => calls.push([k, a])), set: (t, k, v) => { t[k] = v; return true; } });
  RO.drawReadout(c, ['EXPLORE', 'RA 1', 'v 2', '3 Jy']);
  RO.drawLabels(c, 'EXPLORE', C.labelLines(C.controlMap({ mode: 'explore' }), 'primary'));
  assert.ok(calls.filter(([k]) => k === 'fillText').length >= 4 + 1 + 2 * 6);
});
