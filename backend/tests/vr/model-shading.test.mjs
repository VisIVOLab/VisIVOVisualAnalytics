// The real controller models' shaders (renderer.js MODEL_VS / MODEL_FS).
// node --test backend/tests/vr/model-shading.test.mjs
//
// On the Quest's GPU mediump is half precision: with the models' 0.01 node
// scale the world-space normal's squared length (1e-4, less between
// vertices) flushes to 0 and normalize() gives Inf/NaN, so the dark plastic
// saturated to white where the normals bend (it followed the view). A Mac's
// GPU runs mediump at full precision and cannot show it, hence these checks
// on the sources; validation/vr_controller_shading.mjs checks the pixels.
import test from 'node:test';
import assert from 'node:assert/strict';
import { MODEL_VS, MODEL_FS, MODEL_TEXTURE_MAX, MODEL_ANISOTROPY, mirrors } from '../../app/static/vr/js/renderer.js';

const code = (src) => src.replace(/\/\/.*$/gm, '');

test('the controller-model shaders run in full precision', () => {
  for (const [name, src] of [['MODEL_VS', MODEL_VS], ['MODEL_FS', MODEL_FS]]) {
    assert.match(src, /^#version 300 es\nprecision highp float;/, `${name}: highp by default`);
    assert.doesNotMatch(code(src), /\b(mediump|lowp)\b/, `${name}: nothing in half precision`);
  }
});

test('the normal is made unit length in the vertex shader, with the inverse transpose', () => {
  const vs = code(MODEL_VS);
  assert.match(vs, /transpose\(inverse\(mat3\(uModel\)\)\)\s*\*\s*aNormal/);
  assert.match(vs, /inversesqrt/);
  // A degenerate normal never reaches the fragment shader as 0/0.
  assert.match(vs, /l2 > 0\.0/);
});

test('the fragment shader: sRGB out, alpha 1 unless BLEND, back faces lit from their side', () => {
  const fs = code(MODEL_FS);
  assert.match(fs, /toSrgb/);
  assert.match(fs, /uAlphaMode == 2 \? vec4\(srgb \* base\.a, base\.a\) : vec4\(srgb, 1\.0\)/);
  assert.match(fs, /uAlphaMode == 1 && base\.a < uCutoff\) discard/);
  // glTF: back-facing triangles reverse the normal (drawModels sets the
  // winding per node), rather than an abs() of the lighting terms.
  assert.match(fs, /if \(!gl_FrontFacing\) n = -n/);
  assert.doesNotMatch(fs, /abs\(dot/);
});

test('texture budget: 1024² with 4× anisotropy', () => {
  assert.equal(MODEL_TEXTURE_MAX, 1024);
  assert.equal(MODEL_ANISOTROPY, 4);
});

test('mirrors: a negative determinant is a mirroring transform', () => {
  const I = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
  assert.equal(mirrors(I), false);
  assert.equal(mirrors([-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]), true);
  assert.equal(mirrors([-1, 0, 0, 0, 0, -1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1]), true, 'the generic profile’s scale −1');
  assert.equal(mirrors([0.01, 0, 0, 0, 0, 0.01, 0, 0, 0, 0, 0.01, 0, 5, 6, 7, 1]), false);
});

test('the pixel checks of validation/vr_controller_shading.mjs fail on black, white, translucent or wrongly culled models', async () => {
  const { checkShading, DIRECTIONS } = await import('../../../validation/vr_controller_shading.mjs');
  const good = { covered: 9000, darkN: 3000, darkMedian: 30, darkP95: 50, darkWhite: 0, lightN: 6000, lightMedian: 170, lightP10: 130, lightP90: 220, lightClipped: 0, opaque: 1, cullDiff: 0.001 };
  const run = (patch, stateKept = true) => {
    const views = Object.keys(DIRECTIONS).map((dir) => ({ profile: 'p', hand: 'left', dir, ...good, ...patch }));
    const failed = [];
    checkShading({ views, stateKept }, (name, ok) => { if (!ok) failed.push(name); });
    return failed;
  };
  assert.deepEqual(run({}), []);
  assert.ok(run({ darkMedian: 0, darkP95: 0, lightMedian: 0, lightP10: 0, lightP90: 0 }).length, 'all black');
  assert.ok(run({ darkP95: 240, darkWhite: 0.4 }).length, 'dark plastic turned white');
  assert.ok(run({ lightClipped: 0.6 }).length, 'light plastic clipped');
  assert.ok(run({ opaque: 0.9 }).length, 'translucent');
  assert.ok(run({ cullDiff: 0.4 }).length, 'the front culled');
  assert.ok(run({}, false).length, 'GL state changed');
  assert.ok(run({ darkN: 0, lightN: 0 }).length, 'nothing seen');
});
