// Colour-only frames (issue #187): node --test backend/tests/vr/depth-mode.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';

import { frameReprojection, showLimits, reprojectionHud, POSE_ONLY_MAX_METRES }
  from '../../app/static/vr/js/depth-mode.js';
import { SHOW_DEFAULTS, frameUsable } from '../../app/static/vr/js/refine.js';
import * as M from '../../app/static/vr/js/mat4.js';

const withDepth = { reprojection: 'depth', depth: { width: 8, height: 4 }, depth_unavailable: null };
const colourOnly = { reprojection: 'pose', depth: null,
  depth_unavailable: "the window's depth buffer could not be read (status 2)" };

test('the header says how a frame may be warped', () => {
  assert.deepEqual(frameReprojection(withDepth), { mode: 'depth', reason: '' });
  assert.deepEqual(frameReprojection(colourOnly),
    { mode: 'pose', reason: "the window's depth buffer could not be read (status 2)" });
  // A backend before #187: from the depth alone.
  assert.equal(frameReprojection({ depth: { width: 1 } }).mode, 'depth');
  assert.deepEqual(frameReprojection({ depth: null }), { mode: 'pose', reason: 'the frame carries no depth' });
  assert.equal(frameReprojection(null).mode, 'pose');
});

test('a frame without depth is held to a smaller step of the eye', () => {
  assert.equal(showLimits(withDepth, SHOW_DEFAULTS), SHOW_DEFAULTS);
  const o = showLimits(colourOnly, SHOW_DEFAULTS);
  assert.equal(o.maxMetres, POSE_ONLY_MAX_METRES);
  assert.equal(o.maxDeg, SHOW_DEFAULTS.maxDeg);
  assert.equal(SHOW_DEFAULTS.maxMetres, 0.04);          // the defaults themselves untouched
  // A 2 cm step: shown with depth, not without; a 3 degree turn: shown both ways.
  const a = M.identity();
  const step = Float32Array.from(a); step[12] = 0.02;
  assert.equal(frameUsable(a, step, 'still', showLimits(withDepth, SHOW_DEFAULTS)), true);
  assert.equal(frameUsable(a, step, 'still', showLimits(colourOnly, SHOW_DEFAULTS)), false);
  const c = Math.cos(3 * Math.PI / 180), s = Math.sin(3 * Math.PI / 180);
  const turn = new Float32Array([c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1]);
  assert.equal(frameUsable(a, turn, 'still', showLimits(colourOnly, SHOW_DEFAULTS)), true);
});

test('the HUD says when frames come without depth, and why', () => {
  assert.deepEqual(reprojectionHud(withDepth), []);
  const lines = reprojectionHud(colourOnly);
  assert.equal(lines[0], '  reprojection: pose only (no depth)');
  assert.ok(lines.every((l) => l.length <= 60), JSON.stringify(lines));
  // The whole reason, over the lines, nothing cut.
  assert.equal(lines.slice(1).map((l) => l.trim()).join(' '), `why: ${colourOnly.depth_unavailable}`);
  // A reason without spaces is cut, never lost to an endless loop.
  const long = reprojectionHud({ depth: null, depth_unavailable: 'x'.repeat(500) });
  assert.equal(long.length, 4);
});
