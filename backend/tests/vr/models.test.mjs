// Visible controllers and tracked hands (issue #226, point 7).
// node --test backend/tests/vr/models.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import * as MD from '../../app/static/vr/js/models.js';
import * as M from '../../app/static/vr/js/mat4.js';
import { readFileSync } from 'node:fs';

function bounds(arr) {
  const lo = [Infinity, Infinity, Infinity], hi = [-Infinity, -Infinity, -Infinity];
  for (let i = 0; i < arr.length; i += MD.MESH_STRIDE) {
    for (let k = 0; k < 3; k++) { lo[k] = Math.min(lo[k], arr[i + k]); hi[k] = Math.max(hi[k], arr[i + k]); }
  }
  return { lo, hi };
}

test('a controller is a solid model of controller size, at its grip pose', () => {
  const grip = M.multiply(M.translation(0.3, 1.1, -0.4), M.rotation([0, 1, 0], 0.4));
  const out = MD.controllerMesh([], grip, [1, 0.6, 0.2, 1]);
  const n = MD.vertexCount(out);
  assert.ok(n > 300 && n % 3 === 0, `${n} vertices, whole triangles`);
  assert.ok(out.every(Number.isFinite));
  // Within 15 cm of the grip, and at least 6 cm across: visible, not a dot.
  const { lo, hi } = bounds(out);
  for (let k = 0; k < 3; k++) {
    assert.ok(lo[k] > [0.3, 1.1, -0.4][k] - 0.15 && hi[k] < [0.3, 1.1, -0.4][k] + 0.15, `axis ${k}`);
  }
  assert.ok(Math.max(hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]) > 0.06);
  // Normals are unit length.
  for (let i = 0; i < out.length; i += MD.MESH_STRIDE) {
    assert.ok(Math.abs(Math.hypot(out[i + 3], out[i + 4], out[i + 5]) - 1) < 1e-4);
  }
  // The trigger lights while pulled.
  const pulled = MD.controllerMesh([], grip, [1, 0.6, 0.2, 1], { pressed: { trigger: true } });
  assert.notDeepEqual(pulled, out);
});

test('a tracked hand: a sphere at every joint and a bar along every bone', () => {
  assert.equal(MD.HAND_JOINTS.length, 25);
  assert.equal(MD.HAND_BONES.length, 24 + 3);
  for (const [a, b] of MD.HAND_BONES) assert.ok(MD.HAND_JOINTS.includes(a) && MD.HAND_JOINTS.includes(b));
  const joints = Object.fromEntries(MD.HAND_JOINTS.map((n, i) => [n, { p: [0.01 * (i % 5), 1 + 0.02 * Math.floor(i / 5), -0.3], r: 0.008 }]));
  const out = MD.handMesh([], joints, [0.4, 0.7, 1, 1]);
  const tris = MD.vertexCount(out) / 3;
  assert.equal(tris, 25 * 8 + MD.HAND_BONES.length * 12, 'eight faces a joint, twelve a bone');
  // Missing joints (a hand partly out of view) are left out, not drawn at the origin.
  const some = { wrist: joints.wrist, 'thumb-metacarpal': joints['thumb-metacarpal'] };
  const part = MD.handMesh([], some, [1, 1, 1, 1]);
  assert.equal(MD.vertexCount(part) / 3, 2 * 8 + 12);
  const { lo } = bounds(part);
  assert.ok(lo[1] > 0.9);
  // Pinch lights the thumb and index tips.
  assert.notDeepEqual(MD.handMesh([], joints, [0.4, 0.7, 1, 1], { pinch: true }), out);
});

test('nothing is loaded from a CDN: the real models are served with the page, the fallback made in it', () => {
  for (const f of ['models.js', 'main.js', 'renderer.js', 'gltf.js', 'controller-profiles.js', 'controller-models.js']) {
    const src = readFileSync(new URL(`../../app/static/vr/js/${f}`, import.meta.url), 'utf8');
    assert.ok(!/https?:\/\//i.test(src.replace(/\/\/ .*$|^\s*\*.*$/gm, '')), `${f} names no remote address in its code`);
  }
  const loader = readFileSync(new URL('../../app/static/vr/js/controller-models.js', import.meta.url), 'utf8');
  assert.match(loader, /new URL\('\.\.\/models\/', import\.meta\.url\)/, 'the models beside the page');
  const main = readFileSync(new URL('../../app/static/vr/js/main.js', import.meta.url), 'utf8');
  assert.match(main, /R\.setMesh\(inputMesh\(\)\)/);
  assert.match(main, /R\.drawModels\(v, modelDraws, modelDrawCount\)/);
  assert.match(main, /R\.drawMesh\(v\)/);
});

test('the hand tracker reads every joint the browser tracks', async () => {
  const H = await import('../../app/static/vr/js/hands.js');
  const asked = [];
  const frame = { getJointPose: (s) => { asked.push(s.joint); return { transform: { position: { x: 0, y: 1, z: 0 } }, radius: 0.01 }; } };
  const hand = { get: (n) => ({ joint: n }) };
  const j = H.readJoints(frame, hand, {});
  assert.equal(asked.length, 25);
  assert.equal(Object.keys(j).length, 25);
});
