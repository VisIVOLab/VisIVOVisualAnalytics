// The real controller models: profile choice and visual responses, on the vendored files.
// node --test backend/tests/vr/controller-profiles.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, statSync, readdirSync } from 'node:fs';
import * as G from '../../app/static/vr/js/gltf.js';
import * as P from '../../app/static/vr/js/controller-profiles.js';

const MODELS = new URL('../../app/static/vr/models/', import.meta.url);
const json = (p) => JSON.parse(readFileSync(new URL(p, MODELS), 'utf8'));
const LIST = json('profilesList.json');
const I = new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);

function rigFor(id, hand) {
  const profile = json(LIST[id].path);
  const layout = P.layoutFor(profile, hand);
  const model = G.readGLTF(G.parseGLB(readFileSync(new URL(`${id}/${layout.assetPath}`, MODELS))));
  return P.makeRig(model, layout);
}

const pad = (n = 7) => ({ buttons: Array.from({ length: n }, () => ({ value: 0, pressed: false, touched: false })), axes: [0, 0, 0, 0] });

test('the vendored profiles: listed, with their files, their licence, and a bounded size', () => {
  assert.deepEqual(Object.keys(LIST).sort(), [...P.VENDORED_PROFILES].sort());
  let bytes = 0;
  for (const id of P.VENDORED_PROFILES) {
    const profile = json(LIST[id].path);
    assert.equal(profile.profileId, id);
    for (const hand of ['left', 'right']) {
      const f = new URL(`${id}/${P.layoutFor(profile, hand).assetPath}`, MODELS);
      bytes += statSync(f).size;
    }
  }
  const licence = readFileSync(new URL('LICENSE.md', MODELS), 'utf8');
  assert.match(licence, /MIT License/);
  assert.match(licence, /Copyright \(c\) 2019 Amazon/);
  // Only what the page uses is vendored (no 'none' handedness model).
  for (const id of P.VENDORED_PROFILES) {
    for (const f of readdirSync(new URL(`${id}/`, MODELS))) assert.ok(['left.glb', 'right.glb', 'profile.json'].includes(f), `${id}/${f}`);
  }
  assert.ok(bytes < 6e6, `${(bytes / 1e6).toFixed(2)} MB of models`);
});

test('profile choice: the first of the source\'s profiles that is vendored, as the registry does', () => {
  // A Quest 3 (Touch Plus).
  assert.equal(P.selectProfileId(['meta-quest-touch-plus', 'generic-trigger-squeeze-thumbstick'], LIST), 'meta-quest-touch-plus');
  // A profile the page lacks first: the next one in the source's own order.
  assert.equal(P.selectProfileId(['meta-quest-touch-plus-v2', 'meta-quest-touch-plus', 'oculus-touch-v3'], LIST), 'meta-quest-touch-plus');
  assert.equal(P.selectProfileId(['meta-quest-touch-pro', 'meta-quest-touch-plus'], LIST), 'meta-quest-touch-pro');
  assert.equal(P.selectProfileId(['oculus-touch-v3', 'oculus-touch'], LIST), 'oculus-touch-v3');
  assert.equal(P.selectProfileId(['htc-vive', 'generic-trigger-squeeze-thumbstick'], LIST), 'generic-trigger-squeeze-thumbstick');
  // Nothing matches, or no list: the page's own model.
  assert.equal(P.selectProfileId(['valve-index', 'generic-trigger-touchpad'], LIST), null);
  assert.equal(P.selectProfileId([], LIST), null);
  assert.equal(P.selectProfileId(undefined, LIST), null);
  assert.equal(P.selectProfileId(['toString', 'constructor'], LIST), null, 'not fooled by inherited keys');
  // Layouts by handedness; 'any' takes the first.
  const plus = json(LIST['meta-quest-touch-plus'].path);
  assert.equal(P.layoutFor(plus, 'left').assetPath, 'left.glb');
  assert.equal(P.layoutFor(plus, 'right').assetPath, 'right.glb');
  assert.equal(P.layoutFor(plus, 'none'), null);
  assert.equal(P.layoutFor(plus, 'any').assetPath, 'left.glb');
});

test('component values: states and thresholds as the motion-controllers library', () => {
  const v = {};
  const g = pad();
  P.componentValues({ button: 0 }, g, v);
  assert.deepEqual(v, { state: 'default', button: 0, xAxis: 0, yAxis: 0 });
  g.buttons[0].value = 0.04; P.componentValues({ button: 0 }, g, v);
  assert.equal(v.state, 'default');
  g.buttons[0].value = 0.06; P.componentValues({ button: 0 }, g, v);
  assert.equal(v.state, 'touched');
  g.buttons[0].value = 1; P.componentValues({ button: 0 }, g, v);
  assert.equal(v.state, 'pressed');
  g.buttons[0].value = 0.3; g.buttons[0].pressed = true; P.componentValues({ button: 0 }, g, v);
  assert.equal(v.state, 'pressed'); assert.equal(v.button, 0.3);
  g.buttons[0].value = 7; P.componentValues({ button: 0 }, g, v);
  assert.equal(v.button, 1, 'clamped');
  // Axes: touched past 0.1, clamped to -1..1; a missing index is ignored.
  const s = pad();
  s.axes[2] = 0.05; P.componentValues({ button: 3, xAxis: 2, yAxis: 3 }, s, v);
  assert.equal(v.state, 'default');
  s.axes[3] = -0.5; P.componentValues({ button: 3, xAxis: 2, yAxis: 3 }, s, v);
  assert.equal(v.state, 'touched'); assert.equal(v.yAxis, -0.5);
  s.axes[2] = -3; P.componentValues({ button: 3, xAxis: 2, yAxis: 3 }, s, v);
  assert.equal(v.xAxis, -1);
  P.componentValues({ button: 12 }, s, v);
  assert.equal(v.state, 'default');
});

test('response values: buttons 0..1 in their states, axes 0.5 at rest and inside the unit circle', () => {
  const btn = { componentProperty: 'button', states: ['default', 'touched', 'pressed'] };
  assert.equal(P.responseValue(btn, { state: 'touched', button: 0.4, xAxis: 0, yAxis: 0 }), 0.4);
  assert.equal(P.responseValue({ ...btn, states: ['pressed'] }, { state: 'touched', button: 0.4 }), 0);
  const x = { componentProperty: 'xAxis', states: ['default', 'touched', 'pressed'] };
  const y = { componentProperty: 'yAxis', states: ['default', 'touched', 'pressed'] };
  assert.equal(P.responseValue(x, { state: 'default', xAxis: 0, yAxis: 0 }), 0.5);
  assert.equal(P.responseValue(x, { state: 'touched', xAxis: 1, yAxis: 0 }), 1);
  assert.equal(P.responseValue(y, { state: 'touched', xAxis: 0, yAxis: -1 }), 0);
  // (1, 1) is pulled onto the circle: each 0.5 + 0.5·cos 45°.
  assert.ok(Math.abs(P.responseValue(x, { state: 'touched', xAxis: 1, yAxis: 1 }) - (0.5 + 0.5 * Math.SQRT1_2)) < 1e-9);
  assert.equal(P.responseValue({ componentProperty: 'state', states: ['pressed'] }, { state: 'pressed' }), 1);
  assert.equal(P.responseValue({ componentProperty: 'state', states: ['pressed'] }, { state: 'touched' }), 0);
});

test('the Touch Plus rig: every component bound, the absent menu node left out', () => {
  const rig = rigFor('meta-quest-touch-plus', 'left');
  assert.deepEqual(rig.components.map((c) => c.id).sort(),
    ['thumbrest', 'x-button', 'xr-standard-squeeze', 'xr-standard-thumbstick', 'xr-standard-trigger', 'y-button']);
  assert.deepEqual(rig.skipped, ['menu/menu_pressed']);
  const right = rigFor('meta-quest-touch-plus', 'right');
  assert.ok(right.components.some((c) => c.id === 'a-button') && right.components.some((c) => c.id === 'b-button'));
});

/** The world matrix of a node after the gamepad `g`. */
function nodeAfter(rig, name, g) {
  P.applyResponses(rig, g);
  P.updateWorld(rig, I);
  return Float32Array.from(rig.world[P.nodeIndex(rig, name)]);
}
const maxDiff = (a, b) => Math.max(...a.map((v, i) => Math.abs(v - b[i])));

test('visual responses: the trigger, squeeze, buttons and thumbstick move between their min and max nodes', () => {
  for (const id of P.VENDORED_PROFILES) {
    for (const hand of ['left', 'right']) {
      const rig = rigFor(id, hand);
      const g = pad();
      const cases = [['xr_standard_trigger_pressed_value', 0], ['xr_standard_squeeze_pressed_value', 1]];
      if (id !== 'generic-trigger-squeeze-thumbstick') cases.push([`${hand === 'left' ? 'x' : 'a'}_button_pressed_value`, 4]);
      for (const [name, b] of cases) {
        const rest = nodeAfter(rig, name, g);
        g.buttons[b].value = 1; g.buttons[b].pressed = true;
        const full = nodeAfter(rig, name, g);
        g.buttons[b].value = 0.5; g.buttons[b].pressed = false;
        const half = nodeAfter(rig, name, g);
        g.buttons[b].value = 0;
        const back = nodeAfter(rig, name, g);
        assert.ok(maxDiff(rest, full) > 3e-4, `${id} ${hand} ${name} moves (${maxDiff(rest, full)})`);
        assert.ok(maxDiff(rest, back) < 1e-6, `${id} ${hand} ${name} comes back`);
        // Half way is between the two (closer to neither end than the ends to each other).
        assert.ok(maxDiff(rest, half) < maxDiff(rest, full) && maxDiff(full, half) < maxDiff(rest, full), `${id} ${hand} ${name} half way`);
      }
      // The thumbstick tilts with its axes.
      const rest = nodeAfter(rig, 'xr_standard_thumbstick_xaxis_pressed_value', g);
      g.axes[2] = 1;
      const tilted = nodeAfter(rig, 'xr_standard_thumbstick_xaxis_pressed_value', g);
      assert.ok(maxDiff(rest, tilted) > 1e-3, `${id} ${hand} thumbstick x`);
      g.axes[2] = 0; g.axes[3] = 1;
      const ty = nodeAfter(rig, 'xr_standard_thumbstick_yaxis_pressed_value', g);
      g.axes[3] = 0;
      assert.ok(maxDiff(nodeAfter(rig, 'xr_standard_thumbstick_yaxis_pressed_value', g), ty) > 1e-3, `${id} ${hand} thumbstick y`);
    }
  }
});

test('the value node lands exactly on the min and max nodes at 0 and 1', () => {
  const rig = rigFor('meta-quest-touch-plus', 'right');
  const comp = rig.components.find((c) => c.id === 'xr-standard-trigger');
  const r = comp.responses[0];
  const g = pad();
  g.buttons[0].value = 1; g.buttons[0].pressed = true;
  P.applyResponses(rig, g);
  for (let k = 0; k < 3; k++) assert.ok(Math.abs(rig.t[r.value][k] - rig.base[r.max].t[k]) < 1e-6);
  const dot = Math.abs(rig.r[r.value].reduce((a, v, k) => a + v * rig.base[r.max].r[k], 0));
  assert.ok(Math.abs(dot - 1) < 1e-6);
  g.buttons[0].value = 0; g.buttons[0].pressed = false;
  P.applyResponses(rig, g);
  const dot0 = Math.abs(rig.r[r.value].reduce((a, v, k) => a + v * rig.base[r.min].r[k], 0));
  assert.ok(Math.abs(dot0 - 1) < 1e-6);
});

test('a frame of responses and world matrices allocates nothing new and follows the grip', () => {
  const rig = rigFor('meta-quest-touch-plus', 'right');
  const world0 = rig.world[0], t0 = rig.t[0];
  const g = pad();
  g.buttons[0].value = 0.7;
  const grip = Float32Array.from(I); grip[12] = 0.3; grip[13] = 1.2; grip[14] = -0.4;
  P.applyResponses(rig, g);
  P.updateWorld(rig, grip);
  assert.equal(rig.world[0], world0); assert.equal(rig.t[0], t0);
  const at = P.nodeIndex(rig, 'root');
  P.updateWorld(rig, I);
  const here = Float32Array.from(rig.world[at]);
  P.updateWorld(rig, grip);
  assert.ok(Math.abs(rig.world[at][12] - here[12] - 0.3) < 1e-6 && Math.abs(rig.world[at][14] - here[14] + 0.4) < 1e-6);
  assert.ok(rig.drawn.every((d, i) => d === 1 || !rig.order.includes(i)));
});

test('a visibility response hides its node and the node\'s children', () => {
  const model = {
    nodes: [
      { name: 'root', mesh: -1, children: [1], parent: -1, t: [0, 0, 0], r: [0, 0, 0, 1], s: [1, 1, 1], matrix: null },
      { name: 'dot', mesh: -1, children: [2], parent: 0, t: [0, 0, 0], r: [0, 0, 0, 1], s: [1, 1, 1], matrix: null },
      { name: 'dot_mesh', mesh: -1, children: [], parent: 1, t: [0, 0, 0], r: [0, 0, 0, 1], s: [1, 1, 1], matrix: null },
    ],
    roots: [0], meshes: [], materials: [], images: [],
  };
  const layout = { components: { 'xr-standard-touchpad': { type: 'touchpad', gamepadIndices: { button: 2, xAxis: 0, yAxis: 1 },
    visualResponses: { touched: { componentProperty: 'state', states: ['touched', 'pressed'], valueNodeProperty: 'visibility', valueNodeName: 'dot' } } } } };
  const rig = P.makeRig(model, layout);
  const g = pad();
  P.applyResponses(rig, g); P.updateWorld(rig, I);
  assert.deepEqual([...rig.drawn], [1, 0, 0]);
  g.axes[0] = 0.5;
  P.applyResponses(rig, g); P.updateWorld(rig, I);
  assert.deepEqual([...rig.drawn], [1, 1, 1]);
});
