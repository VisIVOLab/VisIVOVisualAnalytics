// Fixed-capacity 3-D textures (issue #235): cubes change inside a VR session
// with texSubImage3D only. The plan, the apron and the sampling that keeps a
// cube in a corner of a larger storage identical to storage of its own size,
// and the renderer driven through a counting stand-in for WebGL2.
// node --test backend/tests/vr/capacity.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import * as V from '../../app/static/vr/js/volume.js';
import * as Q from '../../app/static/vr/js/quality.js';
import { Renderer } from '../../app/static/vr/js/renderer.js';

const MiB = 1024 * 1024;

/** A WebGL2 stand-in: every call counted, 3-D texture calls logged; objects are tokens. */
function fakeGL() {
  const calls = {};
  const log = [];
  const live = new Set();
  let next = 1;
  const gl = new Proxy({}, {
    get(t, k) {
      if (typeof k !== 'string') return undefined;
      if (Object.prototype.hasOwnProperty.call(t, k)) return t[k];
      if (/^[A-Z0-9_]+$/.test(k)) return k;
      return (...a) => {
        calls[k] = (calls[k] || 0) + 1;
        if (/^tex(Image|SubImage|Storage)3D$/.test(k)) log.push([k, ...a]);
        if (/^create(Texture|Buffer|VertexArray|Program|Shader|Query)$/.test(k)) { const o = { k, id: next++ }; live.add(o); return o; }
        if (/^delete(Texture|Buffer|VertexArray|Query)$/.test(k)) { live.delete(a[0]); return undefined; }
        if (k === 'getShaderParameter' || k === 'getProgramParameter') return a[1] === 'ACTIVE_UNIFORMS' ? 0 : true;
        if (k === 'getParameter') return a[0] === 'MAX_3D_TEXTURE_SIZE' ? 2048 : 0;
        if (k === 'getAttribLocation') return 0;
        if (k === 'getExtension') return null;
        if (k === 'isContextLost') return false;
        return undefined;
      };
    },
  });
  const textures = () => [...live].filter((o) => o.k === 'createTexture').length;
  return { gl, calls, log, textures, reset: () => { for (const k of Object.keys(calls)) delete calls[k]; log.length = 0; } };
}

function rampCube(dims, seed = 1) {
  const n = dims[0] * dims[1] * dims[2];
  const a = new Uint8Array(n);
  for (let i = 0; i < n; i++) a[i] = 1 + ((i * 37 + seed * 101) % 254);
  return a;
}

// ── The plan ──────────────────────────────────────────────────────────────

test('capacity: the page\'s Max side per axis, two banks and one mask, counted in bytes', () => {
  const b = Q.capacityBytes([256, 256, 256], 'uint8');
  assert.equal(b.volumeBytes, 256 ** 3);
  assert.equal(b.macroBytes, 32 ** 3 * 4);
  assert.equal(b.maskBytes, 256 ** 3);
  assert.equal(b.totalBytes, 2 * (256 ** 3 + 32 ** 3 * 4) + 256 ** 3);
  const f16 = Q.capacityBytes([256, 256, 256], 'float16');
  assert.equal(f16.totalBytes, 2 * (2 * 256 ** 3 + 32 ** 3 * 4) + 256 ** 3);
  // A headset with 8 GB: 256 MiB for the cubes; 256³ 8-bit takes 48.3 MiB.
  const p = Q.planCapacity({ side: 256, encoding: 'uint8', maxTex3D: 2048, limitBytes: Q.textureLimitBytes({ headset: true, deviceMemoryGB: 8 }) });
  assert.deepEqual(p.dims, [256, 256, 256]);
  assert.ok(!p.grown && !p.shrunk);
  // Widened to a larger cube when two banks of it fit...
  const g = Q.planCapacity({ side: 256, encoding: 'uint8', atLeast: [300, 200, 120], maxTex3D: 2048, limitBytes: 256 * MiB });
  assert.deepEqual(g.dims, [300, 256, 256]);
  assert.ok(g.grown);
  // ...not when they do not (512³ 8-bit: 386 MiB): the plain side, and the cube waits in VR.
  const ng = Q.planCapacity({ side: 256, encoding: 'uint8', atLeast: [512, 512, 512], maxTex3D: 2048, limitBytes: 256 * MiB });
  assert.deepEqual(ng.dims, [256, 256, 256]);
  assert.ok(!V.fitsStore([512, 512, 512], ng.dims));
  // Max side 512 on a 4 GB headset (128 MiB): shrunk to the largest multiple of 8 that fits.
  const s = Q.planCapacity({ side: 512, encoding: 'float16', maxTex3D: 2048, limitBytes: 128 * MiB });
  assert.ok(s.shrunk && s.dims[0] % 8 === 0 && s.totalBytes <= 128 * MiB);
  assert.ok(Q.capacityBytes([s.dims[0] + 8, s.dims[0] + 8, s.dims[0] + 8], 'float16').totalBytes > 128 * MiB);
  // The GPU's side limit caps it; nothing below the minimum side.
  assert.deepEqual(Q.planCapacity({ side: 512, maxTex3D: 256, limitBytes: 1024 * MiB }).dims, [256, 256, 256]);
  assert.equal(Q.planCapacity({ side: 256, limitBytes: 1000 }), null);
  assert.ok(Q.sameCapacity(p, { dims: [256, 256, 256], encoding: 'uint8', cell: 8 }));
  assert.ok(!Q.sameCapacity(p, { dims: [256, 256, 256], encoding: 'float16', cell: 8 }));
  assert.ok(Q.sameCapacity(null, null) && !Q.sameCapacity(p, null));
});

test('capacity: the budget says whether a cube goes into the capacity', () => {
  const capacity = { dims: [256, 256, 256], encoding: 'uint8', cell: 8, totalBytes: 50 * MiB };
  const inside = Q.cubeBudget({ dims: [256, 200, 64], encoding: 'uint8' }, { headset: true, capacity });
  assert.ok(inside.ok && inside.inCapacity);
  assert.equal(inside.capacityBytes, 50 * MiB);
  assert.ok(!Q.cubeBudget({ dims: [257, 200, 64], encoding: 'uint8' }, { headset: true, capacity }).inCapacity);
  assert.ok(!Q.cubeBudget({ dims: [64, 64, 64], encoding: 'float16' }, { headset: true, capacity }).inCapacity);
  assert.ok(!Q.cubeBudget({ dims: [64, 64, 64], encoding: 'uint8' }, { headset: true }).inCapacity);
});

// ── The apron and the sampling ───────────────────────────────────────────

test('apron: the plane past each short axis repeats the cube\'s last voxel, edges and corner included', () => {
  const dims = [5, 4, 3], store = [8, 8, 8];
  const data = rampCube(dims);
  const S = new Uint8Array(store[0] * store[1] * store[2]).fill(250);   // a previous cube
  const put = (x, y, z, v) => { S[(z * store[1] + y) * store[0] + x] = v; };
  for (let z = 0; z < 3; z++) for (let y = 0; y < 4; y++) for (let x = 0; x < 5; x++) put(x, y, z, data[(z * 4 + y) * 5 + x]);
  for (const a of V.apronBlocks(data, dims, store)) {
    let i = 0;
    for (let z = 0; z < a.size[2]; z++) for (let y = 0; y < a.size[1]; y++) for (let x = 0; x < a.size[0]; x++) put(a.offset[0] + x, a.offset[1] + y, a.offset[2] + z, a.data[i++]);
    assert.equal(i, a.data.length);
    assert.ok(a.data instanceof Uint8Array);
  }
  const at = (x, y, z) => S[(z * store[1] + y) * store[0] + x];
  for (let z = 0; z <= 3; z++) for (let y = 0; y <= 4; y++) for (let x = 0; x <= 5; x++) {
    const src = data[(Math.min(z, 2) * 4 + Math.min(y, 3)) * 5 + Math.min(x, 4)];
    assert.equal(at(x, y, z), src, `${x},${y},${z}`);
  }
  // An axis as long as the storage has no apron; a cube that fills it, none at all.
  assert.equal(V.apronBlocks(data, dims, [5, 8, 8]).length, 2);
  assert.equal(V.apronBlocks(data, dims, [5, 4, 3]).length, 0);
  assert.ok(V.fitsStore(dims, store) && !V.fitsStore([9, 1, 1], store) && !V.fitsStore(dims, null));
});

/** GL's linear filtering of a 3-D storage, CLAMP_TO_EDGE, at a normalised
 *  coordinate, with `bits` of sub-texel weight precision (8 on most GPUs). */
function sample(S, store, c, bits = 8) {
  const q = 1 << bits;
  let v = 0;
  const i0 = [], w = [];
  for (let k = 0; k < 3; k++) {
    const u = c[k] * store[k] - 0.5;
    const f = Math.floor(u);
    i0.push(f);
    w.push(Math.round((u - f) * q) / q);
  }
  for (let dz = 0; dz < 2; dz++) for (let dy = 0; dy < 2; dy++) for (let dx = 0; dx < 2; dx++) {
    const wt = (dx ? w[0] : 1 - w[0]) * (dy ? w[1] : 1 - w[1]) * (dz ? w[2] : 1 - w[2]);
    const x = Math.min(store[0] - 1, Math.max(0, i0[0] + dx)), y = Math.min(store[1] - 1, Math.max(0, i0[1] + dy)), z = Math.min(store[2] - 1, Math.max(0, i0[2] + dz));
    const s = S[(z * store[1] + y) * store[0] + x];
    v += wt * s;                   // as the GPU: a NaN neighbour poisons even at weight 0
  }
  return v;
}

test('a cube in a corner of the capacity samples as storage of its own size (the shader\'s scale and clamp)', () => {
  const dims = [13, 9, 6], store = [32, 16, 16];
  const data = new Float32Array(rampCube(dims, 3));
  const own = data;
  const S = new Float32Array(store[0] * store[1] * store[2]).fill(NaN);   // a previous cube: anything, NaN included
  for (let z = 0; z < 6; z++) for (let y = 0; y < 9; y++) for (let x = 0; x < 13; x++) S[(z * 16 + y) * 32 + x] = data[(z * 9 + y) * 13 + x];
  for (const a of V.apronBlocks(data, dims, store)) {
    let i = 0;
    for (let z = 0; z < a.size[2]; z++) for (let y = 0; y < a.size[1]; y++) for (let x = 0; x < a.size[0]; x++) {
      S[((a.offset[2] + z) * 16 + a.offset[1] + y) * 32 + a.offset[0] + x] = a.data[i++];
    }
  }
  const texScale = [0.97, 1, 0.9];   // partial last blocks: the box covers a little less than the texture
  let rnd = 7;
  const r = () => { rnd = (rnd * 16807) % 2147483647; return rnd / 2147483647; };
  let worst = 0;
  for (let n = 0; n < 4000; n++) {
    // Box coordinates over the whole box, faces and corners included.
    const p = [r(), r(), r()].map((v) => (n % 5 === 0 ? Math.round(v) : v));
    const tc = p.map((v, i) => v * texScale[i]);
    // Own storage: the texture coordinate as is (CLAMP_TO_EDGE).
    const a = sample(own, dims, tc);
    // Capacity: scaled to the storage and clamped half a voxel inside the cube,
    // with a float32 rounding nudge outward (the worst case of the GPU's arithmetic).
    const cc = tc.map((v, i) => Math.min((dims[i] - 0.5) / store[i], Math.max(0.5 / store[i], v * dims[i] / store[i])) * (1 + 1e-6));
    const b = sample(S, store, cc);
    // Finite although the storage beyond is NaN: the apron, not the clamp alone, keeps it out.
    assert.ok(Number.isFinite(b), `NaN from a previous cube at ${p}`);
    worst = Math.max(worst, Math.abs(a - b));
  }
  // Same values up to the GPU's sub-texel weight quantisation (1/256 of a step between neighbours).
  assert.ok(worst <= 254 / 256 + 1e-9, `worst ${worst}`);
});

// ── The renderer ─────────────────────────────────────────────────────────

test('renderer: the capacity is allocated outside VR; in VR ten cubes switch with texSubImage3D only, in two banks', () => {
  const { gl, calls, log, textures, reset } = fakeGL();
  const R = new Renderer(gl);
  reset();
  const cap = R.allocCapacity([64, 64, 64], 'uint8');
  assert.equal(calls.texStorage3D, 5, 'two banks (cube and skip grid each) and one mask');
  assert.equal(calls.texImage3D || 0, 0);
  // Every storage zeroed (nothing left for the browser to clear in VR).
  const zeroed = log.filter((c) => c[0] === 'texSubImage3D').reduce((a, c) => a + c[6] * c[7] * c[8], 0);
  assert.equal(zeroed, 3 * 64 ** 3 + 2 * 8 ** 3);
  assert.equal(R.gpuStats().capacityBytes, cap.totalBytes);
  assert.equal(cap.totalBytes, Q.capacityBytes([64, 64, 64], 'uint8').totalBytes);
  const first = R.showVolume({ data: rampCube([64, 40, 20]), dims: [64, 40, 20], encoding: 'uint8' });
  assert.ok(first.capacity);
  R.setMask({ data: new Uint8Array(8), dims: [2, 2, 2], origin: [1, 1, 1], count: 8 });
  const t0 = textures();
  reset();
  R.xrActive = true;
  const banks = [];
  const shown = [R.slot];
  for (let i = 0; i < 10; i++) {
    const d = [10 + i * 5, 64 - i * 3, 8 + i];
    const before = R.slot;
    R.showVolume({ data: rampCube(d, i), dims: d, encoding: 'uint8' });
    assert.notEqual(R.slot, before);
    assert.deepEqual(R.slot.dims, d);
    assert.deepEqual(R.store, [64, 64, 64]);
    assert.equal(R.maskTex, cap.maskTex);
    assert.equal(R.mask, null, 'the previous cube\'s selection is cleared');
    banks.push(R.slot.bank);
    shown.push(R.slot);
    assert.ok(!before.released, 'the cube shown before stays until its bank is written again');
    if (shown.length > 2) assert.ok(shown[shown.length - 3].released, 'the cube written two before is given back');
  }
  assert.deepEqual(banks, [1, 0, 1, 0, 1, 0, 1, 0, 1, 0]);
  assert.equal(calls.texImage3D || 0, 0);
  assert.equal(calls.texStorage3D || 0, 0);
  assert.equal(R.tex3dGuard.refused, 0);
  assert.equal(textures(), t0, 'no texture made or deleted');
  // A selection on the new cube: a sub-image into the shared mask.
  R.setMask({ data: new Uint8Array(27), dims: [3, 3, 3], origin: [0, 0, 0], count: 3 });
  assert.equal(R.mask.dims.join(), '3,3,3');
  // A cube larger than the capacity, or of another texture, is refused in VR before anything changes.
  const on = R.slot;
  const err = (() => { try { R.showVolume({ data: rampCube([65, 8, 8]), dims: [65, 8, 8], encoding: 'uint8' }); } catch (e) { return e; } return null; })();
  assert.equal(err && err.code, 'tex3d-in-xr');
  assert.throws(() => R.showVolume({ data: new Uint16Array(8), dims: [2, 2, 2], encoding: 'float16' }), /tex3d-in-xr/);
  assert.equal(R.slot, on);
  assert.ok(!R.fitsCapacity({ dims: [65, 8, 8], encoding: 'uint8' }));
  assert.throws(() => R.allocCapacity([8, 8, 8], 'uint8'), /tex3d-in-xr/);
  assert.equal(calls.texStorage3D || 0, 0);
});

test('renderer: the apron goes in beside a short cube; a cube of its own storage gives way to the capacity and back', () => {
  const { gl, log, reset } = fakeGL();
  const R = new Renderer(gl);
  R.allocCapacity([32, 32, 32], 'uint8');
  reset();
  R.showVolume({ data: rampCube([20, 32, 10]), dims: [20, 32, 10], encoding: 'uint8' });
  const subs = log.filter((c) => c[0] === 'texSubImage3D');
  // Cube, apron on x and on z (y fills the storage), the skip grid.
  assert.equal(subs.length, 4);
  assert.deepEqual(subs[1].slice(3, 9), [20, 0, 0, 1, 32, 10]);
  assert.deepEqual(subs[2].slice(3, 9), [0, 0, 10, 21, 32, 1]);
  // Outside VR an oversized cube goes into storage of its own; the capacity stays.
  const own = R.showVolume({ data: rampCube([40, 8, 8]), dims: [40, 8, 8], encoding: 'uint8' });
  assert.ok(!own.capacity);
  assert.equal(R.slot.cap, null);
  const g = R.gpuStats();
  assert.ok(g.capacity && !g.capacity.onShow && g.capacity.spareBytes === g.capacityBytes);
  assert.equal(g.cubeBytes, g.capacityBytes + 40 * 8 * 8 + 5 * 1 * 1 * 4 + 40 * 8 * 8);
  const ownSlot = R.slot;
  R.xrActive = true;
  R.showVolume({ data: rampCube([8, 8, 8]), dims: [8, 8, 8], encoding: 'uint8' });
  assert.ok(ownSlot.released, 'the storage of its own is given back when a cube goes into the capacity');
  assert.equal(R.gpuStats().otherVolumes, 0);
});

test('renderer: a new capacity outside VR keeps the cube on show and its selection; restore() makes it again', () => {
  const { gl, calls, reset } = fakeGL();
  const R = new Renderer(gl);
  R.allocCapacity([32, 32, 32], 'uint8');
  R.showVolume({ data: rampCube([16, 16, 16]), dims: [16, 16, 16], encoding: 'uint8' });
  R.setMask({ data: new Uint8Array(8), dims: [2, 2, 2], origin: [0, 0, 0], count: 8 });
  R.reallocCapacity({ dims: [64, 64, 64], encoding: 'uint8', cell: 8 });
  assert.deepEqual(R.cap.dims, [64, 64, 64]);
  assert.equal(R.slot.cap, R.cap);
  assert.deepEqual(R.slot.dims, [16, 16, 16]);
  assert.equal(R.mask.dims.join(), '2,2,2');
  // Smaller than the cube: back to storage of its own.
  R.reallocCapacity({ dims: [8, 8, 8], encoding: 'uint8', cell: 8 });
  assert.equal(R.slot.cap, null);
  assert.equal(R.mask.dims.join(), '2,2,2');
  R.reallocCapacity({ dims: [32, 32, 32], encoding: 'uint8', cell: 8 });
  R.showVolume({ data: rampCube([16, 16, 16]), dims: [16, 16, 16], encoding: 'uint8' });
  reset();
  R.restore();
  assert.equal(calls.texStorage3D, 1 + 5, 'the placeholder and the capacity');
  assert.deepEqual(R.cap.dims, [32, 32, 32]);
  assert.equal(R.slot.cap, R.cap);
  // None: the cube on show is in storage of its own.
  R.reallocCapacity(null);
  assert.equal(R.cap, null);
  assert.equal(R.slot.cap, null);
  assert.equal(R.gpuStats().capacityBytes, 0);
});

test('renderer: a cube in storage of its own moves into a capacity that now holds it; a load gives it back before the new capacity', () => {
  const { gl } = fakeGL();
  const R = new Renderer(gl);
  R.setVolume({ data: rampCube([40, 40, 40]), dims: [40, 40, 40], encoding: 'uint8' });
  R.setMask({ data: new Uint8Array(8), dims: [2, 2, 2], origin: [0, 0, 0], count: 8 });
  const own = R.slot;
  R.reallocCapacity({ dims: [64, 64, 64], encoding: 'uint8', cell: 8 });
  assert.equal(R.slot.cap, R.cap, 'moved into the capacity');
  assert.ok(own.released);
  assert.equal(R.mask.dims.join(), '2,2,2');
  assert.equal(R.gpuStats().cubeBytes, R.cap.totalBytes, 'nothing of its own left');
  // Without reshow (a load about to show another cube): the cube of its own is given back first.
  R.reallocCapacity(null);
  assert.equal(R.slot.cap, null);
  const own2 = R.slot;
  R.reallocCapacity({ dims: [32, 32, 32], encoding: 'uint8', cell: 8 }, { reshow: false });
  assert.ok(own2.released);
  assert.equal(R.slot, null);
  assert.equal(R.dims, null);
  assert.equal(R.gpuStats().cubeBytes, R.cap.totalBytes);
  // The benchmark's volumes (not shown from voxels) are never given back by it.
  const bench = R.prepareVolume({ data: rampCube([16, 16, 16]), dims: [16, 16, 16], encoding: 'uint8' }, 8, { mask: false });
  R.useVolume(bench);
  R.reallocCapacity({ dims: [48, 48, 48], encoding: 'uint8', cell: 8 }, { reshow: false });
  assert.equal(R.slot, bench);
  assert.ok(!bench.released);
});

test('renderer: a capacity the GL could not allocate is given back whole and not used; a migration never holds both', () => {
  const { gl, textures } = fakeGL();
  const R = new Renderer(gl);
  R.setVolume({ data: rampCube([40, 40, 40]), dims: [40, 40, 40], encoding: 'uint8' });
  const base = textures();
  // Out of memory, as WebGL reports it: a GL error after the calls, no exception.
  let calls = 0;
  gl.getError = () => (++calls > 1 ? 'OUT_OF_MEMORY' : 0);
  assert.throws(() => R.allocCapacity([64, 64, 64], 'uint8'), /could not be allocated/);
  assert.equal(textures(), base, 'every texture made for it deleted');
  assert.equal(R.cap, null);
  assert.ok(!R.fitsCapacity({ dims: [8, 8, 8], encoding: 'uint8' }));
  delete gl.getError;
  // Moving the cube of its own into a new capacity: its storage is given back before the capacity is made.
  let peak = 0;
  const create = gl.createTexture;
  gl.createTexture = (...a) => { const t = create(...a); peak = Math.max(peak, textures()); return t; };
  const before = textures();      // the cube's three among them
  R.reallocCapacity({ dims: [64, 64, 64], encoding: 'uint8', cell: 8 });
  assert.equal(R.slot.cap, R.cap);
  assert.equal(peak, before - 3 + 5, 'the capacity\'s five textures made after the cube\'s three were deleted');
});

test('renderer: restore() goes on without the capacity when the restored context cannot hold it', () => {
  const { gl } = fakeGL();
  const R = new Renderer(gl);
  R.allocCapacity([32, 32, 32], 'uint8');
  R.showVolume({ data: rampCube([16, 16, 16]), dims: [16, 16, 16], encoding: 'uint8' });
  R.setMask({ data: new Uint8Array(8), dims: [2, 2, 2], origin: [0, 0, 0], count: 8 });
  let pending = 0, n = 0;
  const storage = gl.texStorage3D;
  gl.texStorage3D = (...a) => { storage(...a); if (++n === 3) pending = 'OUT_OF_MEMORY'; };
  gl.getError = () => { const e = pending; pending = 0; return e; };
  R.restore();
  assert.equal(R.cap, null);
  assert.match(R.capacityError, /could not be allocated/);
  assert.ok(R.slot && R.slot.cap === null, 'the cube in storage of its own');
  assert.equal(R.mask.dims.join(), '2,2,2', 'with its selection');
});
