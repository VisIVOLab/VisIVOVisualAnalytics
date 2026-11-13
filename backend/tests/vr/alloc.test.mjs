// GPU allocations under repeated selections (issue #226, point 8): the
// renderer driven through a counting stand-in for WebGL2.
// node --test backend/tests/vr/alloc.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import { Renderer } from '../../app/static/vr/js/renderer.js';

/** A WebGL2 stand-in: every call is counted (with its arguments in `log`);
 *  objects are plain tokens. Properties set on it (the page's guard) win. */
function fakeGL() {
  const calls = {};
  const log = [];
  const live = new Set();
  let next = 1;
  const consts = { MAX_3D_TEXTURE_SIZE: 'MAX_3D_TEXTURE_SIZE' };
  const gl = new Proxy({}, {
    get(t, k) {
      if (typeof k !== 'string') return undefined;
      if (Object.prototype.hasOwnProperty.call(t, k)) return t[k];
      if (k in consts) return consts[k];
      if (/^[A-Z0-9_]+$/.test(k)) return k;            // other constants: their names
      return (...a) => {
        calls[k] = (calls[k] || 0) + 1;
        if (/^tex(Image|SubImage|Storage)3D$/.test(k)) log.push([k, ...a]);
        if (/^create(Texture|Buffer|VertexArray|Program|Shader|Query)$/.test(k)) { const o = { k, id: next++ }; live.add(o); return o; }
        if (/^delete(Texture|Buffer|VertexArray|Query)$/.test(k)) { live.delete(a[0]); return undefined; }
        if (k === 'getShaderParameter' || k === 'getProgramParameter') return a[1] === 'ACTIVE_UNIFORMS' ? 0 : true;
        if (k === 'getParameter') return a[0] === 'MAX_3D_TEXTURE_SIZE' ? 2048 : 0;
        if (k === 'getAttribLocation') return 0;
        if (k === 'getExtension') return null;
        return undefined;
      };
    },
  });
  const textures = () => [...live].filter((o) => o.k === 'createTexture').length;
  return { gl, calls, log, textures, reset: () => { for (const k of Object.keys(calls)) delete calls[k]; log.length = 0; } };
}

const canvas = (w, h) => ({ width: w, height: h });

test('the mask storage covers the cube\'s grid from the install; each selection is a sub-image upload (issue #229)', () => {
  const { gl, calls, log, textures, reset } = fakeGL();
  const R = new Renderer(gl);
  assert.equal(calls.texImage3D || 0, 0, 'the placeholder is immutable storage too');
  reset();
  R.setVolume({ data: new Uint8Array(64 * 64 * 40).fill(7), dims: [64, 64, 40], encoding: 'uint8' });   // non-zero: only the mask is zeros
  // The cube, its skip grid and the mask: three immutable storages, the mask
  // on the whole reduced grid and zeroed (nothing left for the browser to clear).
  assert.equal(calls.texImage3D || 0, 0);
  assert.equal(calls.texStorage3D, 3);
  assert.ok(log.some((c) => c[0] === 'texStorage3D' && c[3] === 'R8' && c.slice(4).join() === '64,64,40'), 'the cube R8');
  assert.equal(log.filter((c) => c[0] === 'texStorage3D' && c[3] === 'R8' && c.slice(4).join() === '64,64,40').length, 2, 'the cube and its mask');
  const zeroed = log.filter((c) => c[0] === 'texSubImage3D' && c[11] instanceof Uint8Array && c[11].every((v) => v === 0))
    .reduce((a, c) => a + c[6] * c[7] * c[8], 0);
  assert.equal(zeroed, 64 * 64 * 40, 'the mask, every voxel of it, zeroed');
  assert.equal(R.gpuStats().maskBytes, 64 * 64 * 40, 'one byte a reduced voxel');
  const before = textures();
  reset();
  // VR from here: fifty selections of varying size, as fast repeated lasso picks make them.
  R.xrActive = true;
  for (let i = 0; i < 50; i++) {
    const d = [5 + (i % 7) * 3, 6 + (i % 5) * 4, 3 + (i % 3) * 2];
    R.setMask({ data: new Uint8Array(d[0] * d[1] * d[2]), dims: d, origin: [1, 2, 3], count: 10 });
    if (i % 4 === 3) R.setMask(null);
  }
  // The whole grid as one selection fits; one past it is refused, never reallocated.
  R.setMask({ data: new Uint8Array(64 * 64 * 40), dims: [64, 64, 40], origin: [0, 0, 0], count: 1 });
  assert.throws(() => R.setMask({ data: new Uint8Array(65 * 64 * 40), dims: [65, 64, 40], origin: [0, 0, 0], count: 1 }), /does not fit/);
  assert.equal(textures(), before, 'no texture made');
  assert.equal(calls.texImage3D || 0, 0);
  assert.equal(calls.texStorage3D || 0, 0);
  assert.equal(calls.texSubImage3D, 51);
  assert.equal(R.tex3dGuard.refused, 0);
});

test('in VR no 3-D texture is allocated: setVolume and prepareVolume refuse, the guard stops a direct call (issue #229)', () => {
  const { gl, calls, reset } = fakeGL();
  const R = new Renderer(gl);
  const seen = [];
  R.onTex3DInXR = (name) => seen.push(name);
  R.setVolume({ data: new Uint8Array(8 * 8 * 8), dims: [8, 8, 8], encoding: 'uint8' });
  const slot = R.slot;
  const bench = R.prepareVolume({ data: new Uint8Array(16 * 16 * 16), dims: [16, 16, 16], encoding: 'uint8' }, 8, { mask: false });
  assert.equal(bench.maskBytes, 0);
  reset();
  R.xrActive = true;
  const err = (() => { try { R.setVolume({ data: new Uint8Array(8), dims: [2, 2, 2], encoding: 'uint8' }); } catch (e) { return e; } return null; })();
  assert.ok(err && err.code === 'tex3d-in-xr', String(err));
  assert.equal(R.slot, slot, 'the cube on show is kept');
  assert.throws(() => R.prepareVolume({ data: new Uint8Array(8), dims: [2, 2, 2], encoding: 'uint8' }), /tex3d-in-xr/);
  // Any other code's allocation is stopped at the context, before the GPU.
  assert.throws(() => gl.texImage3D('TEXTURE_3D', 0, 'R8', 2, 2, 2, 0, 'RED', 'UNSIGNED_BYTE', null), /tex3d-in-xr/);
  assert.throws(() => gl.texStorage3D('TEXTURE_3D', 1, 'R8', 2, 2, 2), /tex3d-in-xr/);
  assert.equal(calls.texImage3D || 0, 0, 'nothing reached the GPU');
  assert.equal(calls.texStorage3D || 0, 0);
  assert.deepEqual(seen, ['texImage3D', 'texStorage3D'], 'each refusal logged');
  // A prepared volume is shown in VR with no allocation (the benchmark), and the mask placeholder bound.
  R.useVolume(bench);
  assert.equal(R.maskTex, R.emptyMaskTex);
  assert.throws(() => R.setMask({ data: new Uint8Array(1), dims: [1, 1, 1], origin: [0, 0, 0], count: 1 }), /no volume with a selection mask/);
  R.useVolume(slot);
  assert.equal(calls.texImage3D || 0, 0);
  assert.equal(calls.texStorage3D || 0, 0);
  // Out of VR: allowed again; the shown volume is never released by releaseVolume.
  R.xrActive = false;
  R.releaseVolume(slot);
  assert.ok(!slot.released);
  R.releaseVolume(bench);
  assert.ok(bench.released);
  assert.throws(() => R.useVolume(bench), /no longer on the GPU/);
  R.setVolume({ data: new Uint8Array(8), dims: [2, 2, 2], encoding: 'uint8' });
  assert.ok(slot.released, 'the previous cube given back');
  assert.equal(R.gpuStats().otherVolumes, 0);
});

test('panels: storage once per canvas size, then redraws write in place; released panels free their texture', () => {
  const { gl, calls, textures, reset } = fakeGL();
  const R = new Renderer(gl);
  const h = R.createPanel();
  const t0 = textures();
  reset();
  for (let i = 0; i < 120; i++) R.updatePanel(h, canvas(1024, 700));     // the menu at 30 Hz for 4 s
  assert.equal(calls.texImage2D, 1);
  assert.equal(calls.texSubImage2D, 119);
  assert.equal(h.version, 120, 'the composition layers copy by this');
  R.updatePanel(h, canvas(512, 256));
  assert.equal(calls.texImage2D, 2, 'a new size is a new storage');
  assert.equal(textures(), t0);
  R.releasePanel(h);
  assert.equal(textures(), t0 - 1);
  assert.equal(R.gpuStats().panels, 0);
});

test('labels and lines keep their storage; restore() makes everything again after a lost context', () => {
  const { gl, calls, reset } = fakeGL();
  globalThis.OffscreenCanvas = class { constructor(w, h) { this.width = w; this.height = h; }
    getContext() { return new Proxy({}, { get: (t, k) => (k === 'measureText' ? () => ({ width: 50 }) : k in t ? t[k] : () => {}), set: (t, k, v) => { t[k] = v; return true; } }); } };
  try {
    const R = new Renderer(gl);
    const labels = Array.from({ length: 12 }, (_, i) => ({ pos: [0, 0, 0], text: `t${i}`, height: 0.03 }));
    reset();
    for (let i = 0; i < 20; i++) R.setLabels(labels);
    assert.equal(calls.texImage2D, 1, 'the atlas allocated once');
    assert.equal(calls.texSubImage2D, 19);
    // The controllers' lines, every frame: the buffer allocated once.
    reset();
    for (let i = 0; i < 100; i++) R.setLines(Array.from({ length: 6 }, () => ({ a: [0, 0, 0], b: [1, 1, 1], color: [1, 1, 1, 1] })), R.tmpLines);
    assert.equal(calls.bufferData, 1);
    assert.equal(calls.bufferSubData, 100);
    // A mesh a frame (controllers and hands): one allocation.
    reset();
    for (let i = 0; i < 100; i++) R.setMesh(new Float32Array(10 * 3 * 500));
    assert.equal(calls.bufferData, 1);
    // A lost context: restore() makes the objects again and uploads what was on show.
    const h = R.createPanel();
    R.updatePanel(h, canvas(64, 64));
    R.setVolume({ data: new Uint8Array(4 * 4 * 4), dims: [4, 4, 4], encoding: 'uint8' });
    const other = R.prepareVolume({ data: new Uint8Array(8), dims: [2, 2, 2], encoding: 'uint8' }, 8, { mask: false });
    R.setMask({ data: new Uint8Array(8), dims: [2, 2, 2], origin: [0, 0, 0], count: 8 });
    reset();
    R.restore();
    assert.ok(calls.createTexture >= 4, 'textures made again');
    assert.ok(calls.texImage2D >= 2, 'the panel and the label atlas uploaded again');
    assert.equal(R.mask.dims.join(), '2,2,2');
    assert.equal(calls.texStorage3D, 4, 'the placeholder, the cube, its grid and its mask');
    assert.ok(other.lost, 'a volume prepared on the lost context is marked lost');
    assert.equal(R.gpuStats().otherVolumes, 0);
  } finally {
    delete globalThis.OffscreenCanvas;
  }
});
