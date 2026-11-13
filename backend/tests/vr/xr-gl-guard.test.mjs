// The guard against 3-D texture allocations during a VR session (issue #229).
// node --test backend/tests/vr/xr-gl-guard.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import { guardTex3D, TEX3D_ALLOCATIONS } from '../../app/static/vr/js/xr-gl-guard.js';

/** A context stand-in with the methods on its prototype, as WebGL2's are. */
function fakeContext() {
  const reached = [];
  class Ctx {
    texImage3D(...a) { reached.push(['texImage3D', this, a.length]); }
    texStorage3D(...a) { reached.push(['texStorage3D', this, a.length]); }
    texSubImage3D(...a) { reached.push(['texSubImage3D', this, a.length]); }
  }
  return { gl: new Ctx(), reached };
}

test('outside VR every call goes through, with its context and arguments', () => {
  const { gl, reached } = fakeContext();
  let active = false;
  const rec = guardTex3D(gl, () => active);
  gl.texImage3D(1, 0, 2, 4, 4, 4, 0, 3, 5, null);
  gl.texStorage3D(1, 1, 2, 4, 4, 4);
  assert.deepEqual(reached.map((r) => [r[0], r[1] === gl, r[2]]), [['texImage3D', true, 10], ['texStorage3D', true, 6]]);
  assert.deepEqual(rec.calls, { texImage3D: 1, texStorage3D: 1 });
  assert.equal(rec.refused, 0);
  assert.deepEqual(TEX3D_ALLOCATIONS, ['texImage3D', 'texStorage3D']);
});

test('in VR an allocation is refused before the GPU, logged, and thrown as tex3d-in-xr; sub-images still go', () => {
  const { gl, reached } = fakeContext();
  let active = true;
  const seen = [];
  const rec = guardTex3D(gl, () => active, (name) => seen.push(name));
  for (const name of TEX3D_ALLOCATIONS) {
    let err = null;
    try { gl[name](1, 0, 2, 4, 4, 4); } catch (e) { err = e; }
    assert.ok(err, `${name} not refused`);
    assert.equal(err.code, 'tex3d-in-xr');
    assert.equal(err.call, name);
    assert.match(err.message, /^tex3d-in-xr: /);
  }
  gl.texSubImage3D(1, 0, 0, 0, 0, 1, 1, 1, 3, 5, new Uint8Array(1));
  assert.deepEqual(reached.map((r) => r[0]), ['texSubImage3D'], 'only the sub-image reached the GPU');
  assert.deepEqual(seen, ['texImage3D', 'texStorage3D']);
  assert.equal(rec.refused, 2);
  // A log that throws does not let the call through.
  const g2 = fakeContext();
  guardTex3D(g2.gl, () => true, () => { throw new Error('log down'); });
  assert.throws(() => g2.gl.texImage3D(), /tex3d-in-xr/);
  assert.equal(g2.reached.length, 0);
  // After the session: allowed again.
  active = false;
  gl.texStorage3D(1, 1, 2, 4, 4, 4);
  assert.equal(reached.at(-1)[0], 'texStorage3D');
});

test('installed twice on one context, it wraps once', () => {
  const { gl, reached } = fakeContext();
  const a = guardTex3D(gl, () => false);
  const b = guardTex3D(gl, () => true);
  assert.equal(a, b);
  gl.texImage3D();
  assert.equal(reached.length, 1, 'the first guard (not active) decides');
  assert.equal(a.calls.texImage3D, 1);
});
