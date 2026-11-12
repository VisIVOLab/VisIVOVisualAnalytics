// Unit tests of the VR page's selections (issue #171): ray picking on the
// reduced cube, the reduced -> full mapping, the mask reduction, the box
// statistics and the recipe records (node --test).
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import * as S from '../../app/static/vr/js/selection.js';
import * as Rc from '../../app/static/vr/js/recipe.js';
import * as V from '../../app/static/vr/js/volume.js';
import { createSelection } from '../../app/static/vr/js/select-ui.js';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, '../../..');
const close = (a, b, eps = 1e-9) => assert.ok(Math.abs(a - b) <= eps, `${a} != ${b}`);

/** A uint8 reduced cube with every code 1 (normalised 0) and a value function. */
function cube(dims, f = () => 1) {
  const [w, h, d] = dims;
  const data = new Uint8Array(w * h * d);
  for (let z = 0; z < d; z++) for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    data[(z * h + y) * w + x] = f(x, y, z);
  }
  return { data, dims, encoding: 'uint8', texScale: [1, 1, 1] };
}

test('a sample at a texel centre reads that texel, decoded as the shader decodes it', () => {
  const vol = cube([4, 3, 2], (x, y, z) => 1 + x + 4 * y + 12 * z);
  for (let z = 0; z < 2; z++) for (let y = 0; y < 3; y++) for (let x = 0; x < 4; x++) {
    const p = [(x + 0.5) / 4, (y + 0.5) / 3, (z + 0.5) / 2];
    close(S.sampleNorm(vol, p), V.codeToNorm(1 + x + 4 * y + 12 * z), 1e-12);
  }
  // Halfway between two texels along x: the mean of their codes, then decoded.
  close(S.sampleNorm(vol, [1 / 4, 0.5 / 3, 0.5 / 2]), (1.5 - 1) / 254, 1e-12);
  // Outside the texel centres the edge texel is held (CLAMP_TO_EDGE).
  close(S.sampleNorm(vol, [0.01, 0.5 / 3, 0.5 / 2]), V.codeToNorm(1), 1e-12);
});

test('float16 volumes are filtered on their values', () => {
  const data = new Uint16Array([0x0000, 0x3c00]);      // 0 and 1
  const vol = { data, dims: [2, 1, 1], encoding: 'float16', texScale: [1, 1, 1] };
  close(S.sampleNorm(vol, [0.5, 0.5, 0.5]), 0.5, 1e-12);
});

test('the pick is the first sample drawn, and the peak is the brightest of that object', () => {
  // Two objects on the line of sight x = 5, y = 5: a faint one at z 4-6 with
  // its peak at z = 5, a brighter one behind it at z 12-13.
  const vol = cube([10, 10, 16], (x, y, z) => {
    if (x !== 5 || y !== 5) return 1;
    if (z === 4 || z === 6) return 60;
    if (z === 5) return 120;
    if (z === 12 || z === 13) return 250;
    return 1;
  });
  const o = [5.5 / 10, 5.5 / 10, -0.5], d = [0, 0, 1];
  const thr = 0.2;
  const r = S.pickRay(vol, o, d, { thr, stepVox: 0.05 });
  assert.ok(r);
  // The first sample at or above the threshold lies between voxel 3 (code 1)
  // and voxel 4 (code 60): where the interpolated code reaches 1 + 254 thr.
  const zHit = r.hit.p[2] * 16 - 0.5;
  const exact = 3 + (1 + 254 * thr - 1) / (60 - 1);
  assert.ok(zHit >= exact - 1e-9 && zHit < exact + 0.05 + 1e-9, `${zHit} vs ${exact}`);
  assert.ok(r.hit.value >= thr);
  // The peak is the faint object's centre, not the brighter one behind it.
  close(r.peak.p[2] * 16 - 0.5, 5, 0.051);
  close(r.peak.value, V.codeToNorm(120), 1e-9);
  // Maximum intensity draws the brightest of the whole ray.
  const mip = S.pickRay(vol, o, d, { thr, mode: 1, stepVox: 0.05 });
  assert.ok(Math.abs(mip.peak.p[2] * 16 - 0.5 - 12.5) <= 0.55);
  close(mip.peak.value, V.codeToNorm(250), 1e-9);
  // The clipping box hides the first object: the pick goes through to the second.
  const clipped = S.pickRay(vol, o, d, { thr, boxMin: [0, 0, 8 / 16], boxMax: [1, 1, 1], stepVox: 0.05 });
  assert.ok(clipped.hit.p[2] * 16 - 0.5 > 11);
  // Nothing at or above the threshold: no pick.
  assert.equal(S.pickRay(vol, o, d, { thr: 0.999 }), null);
  // A ray that misses the box.
  assert.equal(S.pickRay(vol, [2, 2, 2], [1, 0, 0], { thr }), null);
});

test('a partial last block is sampled where the renderer draws it (texScale)', () => {
  // 10 full pixels binned by 3: 4 blocks, the last holding one pixel.
  const n = 10, f = 3, nr = 4;
  const ts = n / (nr * f);
  const vol = { ...cube([nr, 1, 1], (x) => 1 + 50 * x), texScale: [ts, 1, 1] };
  // The centre of full pixel 9 (the partial block's only pixel) reads block 3
  // at x = (9.5 / 10) * ts * 4 - 0.5 = 2.667: two thirds of the way to it.
  const v = S.sampleNorm(vol, [(9 + 0.5) / n, 0.5, 0.5]);
  const x = ((9 + 0.5) / n) * ts * nr - 0.5;
  close(v, (1 + 50 * (2 + (x - 2)) - 1) / 254, 1e-12);
});

test('box coordinates map to the full cube pixel by pixel, and the block holds it', () => {
  const full = [700, 700, 158];
  for (const px of [[0, 0, 0], [699, 699, 157], [460, 299, 75], [1, 698, 3]]) {
    assert.deepEqual(S.boxToFullPixel(S.fullPixelToBox(px, full), full), px);
  }
  // Out of range points clamp to the cube.
  assert.deepEqual(S.boxToFullPixel([-0.1, 1.2, 1], full), [0, 699, 157]);
  assert.deepEqual(S.reducedVoxelOf([460, 299, 75], [3, 3, 1]), [153, 99, 75]);
  assert.deepEqual(S.blockRange(233, 3, 700), [699, 699]);      // partial last block
  assert.deepEqual(S.blockOfReduced([153, 99, 75], [3, 3, 1], full), [[459, 461], [297, 299], [75, 75]]);
  // The renderer's block centres (texToBox) land inside their own block, the
  // partial last one included, for every reduced voxel.
  for (const [nFull, fac] of [[700, 3], [10, 3], [158, 2], [9, 4], [5, 1]]) {
    const nr = Math.ceil(nFull / fac);
    for (let r = 0; r < nr; r++) {
      const u = V.texToBox((r + 0.5) / nr, nFull, fac);
      const p = S.boxToFullPixel([u, 0, 0], [nFull, 1, 1])[0];
      const [a, b] = S.blockRange(r, fac, nFull);
      assert.ok(p >= a && p <= b, `axis ${nFull}/${fac}: reduced ${r} -> pixel ${p} outside ${a}..${b}`);
      assert.equal(S.reducedVoxelOf([p, 0, 0], [fac, 1, 1])[0], r);
    }
  }
});

test('the brightest voxel of a block skips blanks and keeps the first of a tie', () => {
  const v = new Float32Array([1, NaN, 3, 3, 2, -Infinity]);
  assert.deepEqual(S.brightestInBlock(v, [3, 2, 1], [10, 20, 30]), [12, 20, 30]);
  assert.equal(S.brightestInBlock(new Float32Array([NaN, NaN]), [2, 1, 1], [0, 0, 0]), null);
  const z = new Float32Array(8);
  z[7] = 5;
  assert.deepEqual(S.brightestInBlock(z, [2, 2, 2], [4, 4, 4]), [5, 5, 5]);
});

test('a mask on the drawn grid marks every block holding a selected voxel, and no other', () => {
  let s = 7;
  const rnd = () => { s = (s * 1664525 + 1013904223) >>> 0; return s / 4294967296; };
  for (const [factors, full] of [[[3, 3, 1], [23, 17, 9]], [[2, 2, 2], [11, 7, 5]], [[4, 4, 4], [9, 10, 11]], [[1, 1, 1], [5, 4, 3]]]) {
    const reducedShape = V.reducedShape(full, factors);
    // A box up to the cube's far corner on x and z (partial blocks there).
    const dims = [Math.min(7, full[0]), Math.min(6, full[1] - 1), Math.min(4, full[2])];
    const origin = [full[0] - dims[0], 1, full[2] - dims[2]];
    const mask = new Float32Array(dims[0] * dims[1] * dims[2]);
    for (let i = 0; i < mask.length; i++) mask[i] = rnd() < 0.08 ? 1 : 0;
    const red = S.reduceMask(mask, dims, origin, factors, reducedShape);
    const want = new Set();
    for (let z = 0; z < dims[2]; z++) for (let y = 0; y < dims[1]; y++) for (let x = 0; x < dims[0]; x++) {
      if (mask[(z * dims[1] + y) * dims[0] + x] > 0.5) {
        want.add(S.reducedVoxelOf([origin[0] + x, origin[1] + y, origin[2] + z], factors).join(','));
      }
    }
    const got = new Set();
    for (let z = 0; z < red.dims[2]; z++) for (let y = 0; y < red.dims[1]; y++) for (let x = 0; x < red.dims[0]; x++) {
      if (red.data[(z * red.dims[1] + y) * red.dims[0] + x]) {
        got.add([red.origin[0] + x, red.origin[1] + y, red.origin[2] + z].join(','));
      }
    }
    assert.deepEqual([...got].sort(), [...want].sort());
    assert.equal(red.count, want.size);
    for (let i = 0; i < 3; i++) assert.ok(red.origin[i] + red.dims[i] <= reducedShape[i]);
  }
  // An empty mask marks nothing.
  const none = S.reduceMask(new Float32Array(8), [2, 2, 2], [0, 0, 0], [2, 2, 2], [1, 1, 1]);
  assert.equal(none.count, 0);
  assert.deepEqual(S.maskSummary(new Float32Array([0, 1, 0, 0, 0, 0, 1, 0]), [2, 2, 2], [0, 0, 5]),
    { voxels: 2, channels: [5, 6] });
});

test('a box from two corners, and the one call that measures it', () => {
  const b = S.boxFromCorners([0.9, 0.1, 0.7], [0.2, 0.35, 0.2], [100, 40, 10]);
  assert.deepEqual(b, [[20, 90], [4, 14], [2, 7]]);
  assert.deepEqual(Rc.boxStatsRequest(b),
    { origin_x: 20, origin_y: 4, origin_z: 2, width: 71, height: 11, depth: 6, box: true });
  const steps = Rc.boxReplay(b);
  assert.equal(steps.length, 1);
  assert.equal(steps[0].route, '/v1/cube/mask_stats_bin');
  // Corners placed by rays: the channels of the velocity slab.
  assert.deepEqual(S.channelsOfClip([0, 1], 10), [0, 9]);
  assert.deepEqual(S.channelsOfClip([0.25, 0.5], 10), [2, 4]);     // centres 2.5/10 .. 4.5/10
  assert.deepEqual(S.channelsOfClip([0.31, 0.32], 10), [3, 3]);    // no centre inside: one channel
  assert.deepEqual(S.boxFromRayCorners([0.9, 0.1, 0.7], [0.2, 0.35, 0.7], [100, 40, 10], [0, 1]),
    [[20, 90], [4, 14], [0, 9]]);
});

test('a large block snaps to its brightest 3x3 mean, not to an edge spike', () => {
  // A 10 x 10 x 1 read of an 8 x 8 block (x, y 1..8) widened by one pixel: a
  // spike of 50 at the block's edge (1, 1), a broad source of 10 around (5, 5).
  const w = 10, h = 10;
  const v = new Float32Array(w * h);
  for (let y = 4; y <= 6; y++) for (let x = 4; x <= 6; x++) v[y * w + x] = 10;
  v[1 * w + 1] = 50;
  v[0] = NaN;
  const block = [[1, 8], [1, 8], [0, 0]];
  assert.deepEqual(S.brightestSmoothed(v, [w, h, 1], [0, 0, 0], block), [5, 5, 0]);
  // The plain rule would have taken the spike.
  assert.deepEqual(S.brightestInBlock(v, [w, h, 1], [0, 0, 0]), [1, 1, 0]);
  assert.ok(64 > S.SMOOTH_SNAP_ABOVE && 9 <= S.SMOOTH_SNAP_ABOVE, 'HCG 44 (3x3x1) keeps the plain rule');
  assert.equal(S.brightestSmoothed(new Float32Array(4).fill(NaN), [2, 2, 1], [0, 0, 0], [[0, 1], [0, 1], [0, 0]]), null);
});

test('numbers are written as the desktop writes them (QString::arg with g)', () => {
  assert.equal(Rc.formatG(0.0012345678901234567, 17), '0.0012345678901234567');
  assert.equal(Rc.formatG(8, 4), '8');
  assert.equal(Rc.formatG(64, 4), '64');
  assert.equal(Rc.formatG(2.5e-5, 17), '2.5000000000000001e-05');
  assert.equal(Rc.formatG(12345.678, 4), '1.235e+04');
  assert.equal(Rc.formatG(0.1, 17), '0.10000000000000001');
  const params = Rc.lassoDisplayParams(Rc.lassoGrowRequest([1, 2, 3], { sigma: 0.0012345678901234567 }), 'noise box (1, 2)');
  assert.equal(params['Noise σ'], '0.0012345678901234567 (noise estimate, noise box (1, 2))');
});

/** A selection controller over a stub page and backend whose answers the test releases. */
const harnessState = { state: null };
function harness(extra = {}) {
  const pending = [];
  const later = (value) => new Promise((resolve) => pending.push(() => resolve(value)));
  const calls = [];
  const masks = [];
  const n = [8, 8, 8];
  const dims = [8, 8, 8];
  const data = new Uint8Array(512).fill(200);
  const state = {
    info: { full_shape: n, factors: [1, 1, 1], reduced_shape: dims, bunit: 'Jy/beam',
      axes: { x: { name: 'RA', unit: 'deg', world: Array.from({ length: 8 }, (_, i) => 10 - i * 0.01) },
        y: { name: 'DEC', unit: 'deg', world: Array.from({ length: 8 }, (_, i) => 20 + i * 0.01) },
        z: { name: 'VELO', unit: 'm/s', world: [], velocity_kms: null, full_velocity_kms: null } } },
    cube: { data, dims, encoding: 'uint8' }, open: { dataset_id: 'ds_1' },
    clip: { min: [0, 0, 0], max: [1, 1, 1] }, extent: [1, 1, 1], texScale: [1, 1, 1], thrNorm: 0.1, mode: 0,
  };
  harnessState.state = state;
  const backend = {
    session: 's',
    subvolume: async () => ({ values: new Float32Array([1]), dims: [1, 1, 1], origin: [4, 4, 4] }),
    pixelToSky: async () => ({ valid: false }),
    kinematicSelect: (ds, req) => { calls.push('select'); return later({ mask: new Float32Array([1]), bytes: new ArrayBuffer(4), dims: [1, 1, 1], origin: [4, 4, 4], count: 1, trimBox: 0, ms: 1 }); },
    maskStats: () => { calls.push('stats'); return later({ bbox: [4, 4, 4, 4, 4, 4], flux_jy_kms: 1 }); },
    boxStats: (ds, req) => { calls.push('box'); return later({ selected_voxels: 8, valid_voxels: 8, blank_voxels: 0 }); },
    spectralProbe: () => { calls.push('probe'); return later({ values: [1, 2], spectral_axis: [0, 1] }); },
  };
  const sel = createSelection({ state, renderer: { setMask: (m) => masks.push(m), createPanel: () => ({}), updatePanel: () => {} },
    backend: () => backend, path: () => '/data/cube.fits', onChange: () => {}, ...extra });
  const flush = async () => { for (let i = 0; i < 20; i++) await Promise.resolve(); };
  const release = async () => { await flush(); while (pending.length) { pending.shift()(); await flush(); } };
  const ray = { o: [0.5, 0.5, 1.5], d: [0, 0, -1] };
  return { sel, calls, masks, release, flush, ray, pending };
}

test('a lasso superseded while in flight neither draws nor records', async () => {
  const h = harness();
  h.sel.setTool('lasso');
  const p = h.sel.act(h.ray);
  await h.flush();
  assert.deepEqual(h.calls, ['select']);
  h.sel.setTool('probe');               // the user switches tool mid-grow
  await h.release();
  await p;
  assert.deepEqual(h.calls, ['select'], 'no statistics asked for a dropped grow');
  assert.equal(h.masks.filter(Boolean).length, 0, 'no overlay drawn');
  assert.equal(h.sel.sel.recipes.length, 0);
  assert.equal(h.sel.sel.lasso, null);
  // The same grow, not superseded: drawn and recorded once, after its statistics.
  h.sel.setTool('lasso');
  const q = h.sel.act(h.ray);
  await h.release();
  await q;
  assert.deepEqual(h.calls, ['select', 'select', 'stats']);
  assert.equal(h.masks.filter(Boolean).length, 1);
  assert.equal(h.sel.sel.recipes.length, 1);
  assert.equal(h.sel.sel.recipes[0].product.kind, 'lasso');
  // A local grow keeps its full-resolution mask in the same shape.
  assert.ok(h.sel.sel.lasso.bytes instanceof ArrayBuffer);
  assert.deepEqual(h.sel.sel.lasso.origin, [4, 4, 4]);
  assert.deepEqual(h.sel.sel.lasso.dims, [1, 1, 1]);
});

test('a box is one request, and Clear drops it', async () => {
  const h = harness();
  h.sel.setTool('box');
  await h.sel.act(h.ray);               // first corner: no request
  assert.deepEqual(h.calls, []);
  const p = h.sel.act(h.ray);
  await h.flush();
  assert.deepEqual(h.calls, ['box']);
  h.sel.clear();
  await h.release();
  await p;
  assert.equal(h.sel.sel.box, null);
  assert.equal(h.sel.sel.recipes.length, 0);
  // Measured to the end: one call, one record with one replay step.
  await h.sel.act(h.ray);
  const q = h.sel.act(h.ray);
  await h.release();
  await q;
  assert.deepEqual(h.calls, ['box', 'box']);
  assert.equal(h.sel.sel.recipes.length, 1);
  assert.equal(h.sel.sel.recipes[0].product.replay.length, 1);
  assert.equal(h.sel.sel.recipes[0].product.replay[0].request.box, true);
});

test('a lasso or a box made here is offered to the desktop once, with its grow (#188)', async () => {
  const shared = [];
  const h = harness({ onSelection: (x) => shared.push(x) });
  h.sel.setTool('lasso');
  const p = h.sel.act(h.ray);
  await h.release();
  await p;
  assert.equal(shared.length, 1);
  assert.equal(shared[0].kind, 'lasso');
  assert.deepEqual(shared[0].request, h.sel.sel.recipes[0].product.replay[0].request);
  assert.deepEqual(shared[0].origin, [4, 4, 4]);
  h.sel.setTool('box');
  await h.sel.act(h.ray);
  const q = h.sel.act(h.ray);
  await h.release();
  await q;
  assert.equal(shared.length, 2);
  assert.equal(shared[1].kind, 'box');
  assert.equal(shared[1].range.length, 3);
});

test('a desktop selection before the cube is drawn is refused, leaving a grow in flight alone (#188)', async () => {
  const h = harness();
  h.sel.setTool('lasso');
  const p = h.sel.act(h.ray);
  await h.flush();
  const gen = h.sel.sel.generation;
  const cube = h.sel.sel;   // keep a handle; empty the page's cube
  const info = { seq: 3, kind: 'lasso', source: 'desktop', count: 1, meta: {}, stats: { valid: true } };
  const saved = harnessState.state.cube;
  harnessState.state.cube = null;
  assert.equal(h.sel.showShared(info, { mask: new Float32Array([1]), dims: [1, 1, 1], origin: [1, 1, 1] }), false);
  assert.equal(cube.generation, gen, 'the generation is untouched');
  harnessState.state.cube = saved;
  await h.release();
  await p;
  assert.equal(h.sel.sel.recipes.length, 1, 'the grow in flight still lands');
});

test('a desktop selection is drawn, wins over a grow in flight and is not recorded (#188)', async () => {
  const h = harness();
  h.sel.setTool('lasso');
  const p = h.sel.act(h.ray);
  await h.flush();
  const info = { seq: 3, kind: 'lasso', source: 'desktop', count: 2, meta: { grow: { seed_z: 1 } },
    stats: { valid: true, bbox: [1, 2, 1, 1, 1, 1], flux_jy_kms: 6.061 } };
  const recv = new Float32Array([1, 1]);
  assert.equal(h.sel.showShared(info, { mask: recv, bytes: recv.buffer, dims: [2, 1, 1], origin: [1, 1, 1] }), true);
  await h.release();
  await p;
  assert.equal(h.masks.filter(Boolean).length, 1, 'only the shared mask is drawn');
  assert.equal(h.masks.at(-1).count, 2);
  assert.equal(h.sel.sel.recipes.length, 0, 'the receiving side records nothing');
  assert.equal(h.sel.sel.lasso.count, 2);
  assert.equal(h.sel.sel.lasso.shared.source, 'desktop');
  assert.deepEqual(h.sel.sel.lasso.request, { seed_z: 1 });
  assert.equal(h.sel.sel.lasso.stats.flux_jy_kms, 6.061);
  // The received mask itself is kept, as a local grow keeps its own: the
  // tools apply it, never a regrow.
  assert.ok(h.sel.sel.lasso.bytes instanceof ArrayBuffer);
  assert.deepEqual([...new Float32Array(h.sel.sel.lasso.bytes)], [1, 1]);
  assert.deepEqual(h.sel.sel.lasso.origin, [1, 1, 1]);
  assert.deepEqual(h.sel.sel.lasso.dims, [2, 1, 1]);
  assert.deepEqual(h.sel.sel.lasso.fullShape, [8, 8, 8]);
  assert.equal(h.sel.sel.recipes.length, 0);
});

test('a received selection has exactly the contract the tools read (#188, #195)', () => {
  const h = harness();
  const one = new Float32Array([1, 1, 1, 1]);
  const m = { mask: one, bytes: one.buffer, dims: [2, 2, 1], origin: [1, 2, 3] };
  const base = { kind: 'lasso', source: 'desktop', count: 4, full_shape: [8, 8, 8], stats: { valid: true } };
  const params = { Reach: '8', 'Noise σ': 'robust, whole cube' };
  const grow = { seed_z: 3, seed_y: 2, seed_x: 1, reach: 8 };
  const pick = (L) => ({ request: L.request, bytes: L.bytes, origin: L.origin, dims: L.dims, fullShape: L.fullShape,
    received: L.received, kind: L.kind, source: L.source, handEdited: L.handEdited, box: L.box, parameters: L.parameters });
  // A lasso with its grow, from the desktop.
  h.sel.showShared({ ...base, seq: 1, meta: { grow, parameters: params, hand_edited: false } }, m);
  assert.deepEqual(pick(h.sel.sel.lasso), { request: grow, bytes: one.buffer, origin: [1, 2, 3], dims: [2, 2, 1],
    fullShape: [8, 8, 8], received: true, kind: 'lasso', source: 'desktop', handEdited: false, box: null, parameters: params });
  assert.ok(h.sel.sel.lasso.shared, 'the bookkeeping stays');
  // A mask-only selection (no grow), hand-edited, from the desktop.
  h.sel.showShared({ ...base, seq: 2, meta: { hand_edited: true } }, m);
  assert.deepEqual(pick(h.sel.sel.lasso), { request: null, bytes: one.buffer, origin: [1, 2, 3], dims: [2, 2, 1],
    fullShape: [8, 8, 8], received: true, kind: 'lasso', source: 'desktop', handEdited: true, box: null, parameters: null });
  // A box, from a headset.
  h.sel.showShared({ ...base, seq: 3, kind: 'box', source: 'headset', meta: { box: [[1, 2], [2, 3], [3, 3]], parameters: params } }, m);
  assert.deepEqual(pick(h.sel.sel.lasso), { request: null, bytes: one.buffer, origin: [1, 2, 3], dims: [2, 2, 1],
    fullShape: [8, 8, 8], received: true, kind: 'box', source: 'headset', handEdited: false, box: [1, 2, 2, 3, 3, 3], parameters: params });
  // This page's own selection drawn back from the hub.
  h.sel.showShared({ ...base, seq: 4, source: 'headset', meta: { grow } }, m, true);
  assert.equal(h.sel.sel.lasso.source, 'own');
  assert.equal(h.sel.sel.recipes.length, 0);
});

test('world coordinates of a pixel: the channel velocity from the full axis', () => {
  const info = {
    full_shape: [6, 4, 5], factors: [3, 2, 1], reduced_shape: [2, 2, 5],
    axes: {
      x: { name: 'RA', unit: 'deg', world: [10.0, 9.0] },     // block centres x = 1, 4
      y: { name: 'DEC', unit: 'deg', world: [20.0, 21.0] },   // block centres y = 0.5, 2.5
      z: { name: 'VELO', unit: 'm/s', world: [1, 2, 3, 4, 5], velocity_kms: [100, 110, 120, 130, 140],
        full_velocity_kms: [100, 110, 120, 130, 140] },
    },
  };
  const w = S.worldOfPixel(info, [1, 0, 3]);
  close(w.x, 10.0, 1e-12);            // pixel 1 is block 0's centre
  close(w.y, 20.0, 1e-12);            // before the first block centre: clamped to it, as the labels are
  close(w.velocity, 130, 0);
  assert.equal(S.channelVelocity(info, 4), 140);
  assert.equal(S.channelVelocity(info, 9), null);
});

test('sexagesimal coordinates', () => {
  assert.equal(S.formatRA(154.452375), '10:17:48.57');
  assert.equal(S.formatRA(-15), '23:00:00.00');
  assert.equal(S.formatDec(21.869333), '+21:52:09.6');
  assert.equal(S.formatDec(-0.5), '-00:30:00.0');
  assert.equal(S.formatDec(10.99999999), '+11:00:00.0');
});

/** The field names of `class CubeKinematicSelectRequest(BaseModel)` in the
 *  router, read as the desktop's TestLassoReplay reads them. */
function serviceFields() {
  const src = readFileSync(path.join(REPO, 'backend/app/routers/cube.py'), 'utf8');
  const start = src.indexOf('class CubeKinematicSelectRequest(BaseModel):');
  assert.ok(start >= 0);
  const end = src.indexOf('@model_validator', start);
  const body = src.slice(start, end);
  return new Set([...body.matchAll(/^ {4}(\w+): /gm)].map((m) => m[1]));
}

test('the lasso request uses the service request model\'s field names', () => {
  const full = Rc.lassoGrowRequest([69, 83, 80], { smooth_v: 2, smooth_xy: 6, sigma: 0.00123 },
    [[1, 2, 3]], [[4, 5, 6]]);
  const recorded = new Set([...Object.keys(full), 'dataset_id']);
  assert.deepEqual([...recorded].sort(), [...serviceFields()].sort());
  const bare = Rc.lassoGrowRequest([52, 85, 84]);
  for (const k of ['add_seeds', 'sub_seeds', 'smooth_v', 'smooth_xy']) assert.ok(!(k in bare));
});

test('the default lasso request is the one the desktop records', () => {
  // A desktop recipe exported from the real GUI (WALLABY, 2026-10-22).
  const desk = JSON.parse(readFileSync(path.join(REPO, 'validation/out/recipe_ngc3137_lasso.json'), 'utf8'));
  const grow = desk.product.replay[0];
  assert.equal(grow.route, '/v1/cube/kinematic_select_bin');
  const vr = Rc.lassoGrowRequest([69, 83, 80]);
  assert.deepEqual(vr, grow.request);
  // The same order of keys as the desktop's map (QVariantMap sorts them; the
  // values are what matters, but keep the record readable side by side).
  assert.deepEqual(Object.keys(vr).sort(), Object.keys(grow.request).sort());
  // The Provenance labels match the desktop's for the same request.
  const params = Rc.lassoDisplayParams(vr);
  for (const k of ['Seed (z, y, x)', 'Reach', 'Level-field budget (reach_max)', 'Velocity coupling',
    'Threshold', 'Noise σ', 'Smoothing (v, y, x)', 'Hand-edited']) {
    assert.equal(params[k], desk.parameters[k], k);
  }
});

test('a VR recipe has the desktop record\'s shape', () => {
  const desk = JSON.parse(readFileSync(path.join(REPO, 'validation/out/recipe_ngc3137_lasso.json'), 'utf8'));
  const req = Rc.lassoGrowRequest([69, 83, 80]);
  const r = Rc.makeRecipe({
    application: 'VisIVO Visual Analytics VR page', exported: '2026-11-10T12:00:00Z',
    product: { id: 'vr-lasso-1', kind: 'lasso', label: 'Lasso: selection', replay: Rc.lassoReplay(req) },
    dataset: { label: 'cube.fits', path: '/data/cube.fits', backend_dataset_id: 'ds_1', backend_session_id: 's' },
    parameters: Rc.lassoDisplayParams(req), origin: { kind: 'probe', voxel: [80, 83, 69] },
  });
  assert.equal(r.schema, desk.schema);
  for (const k of Object.keys(desk)) assert.ok(k in r, k);
  for (const k of Object.keys(desk.dataset)) assert.ok(k in r.dataset, k);
  assert.deepEqual(Object.keys(r.product).sort(), Object.keys(desk.product).sort());
  assert.deepEqual(r.product.replay[0], { route: '/v1/cube/kinematic_select_bin', request: req });
  assert.equal(r.product.replay[1].route, '/v1/cube/mask_stats_bin');
  assert.match(r.product.replay[1].body, /X-Visivo-Origin/);
  // The probe and the box records.
  assert.deepEqual(Rc.probeRequest(3, 4), { x: 3, y: 4 });
  assert.deepEqual(Rc.probeOrigin([3, 4, 5]), { kind: 'probe', voxel: [3, 4, 5] });
  assert.deepEqual(Rc.boxOrigin([1, 3], [2, 2], [7, 9]),
    { kind: 'region', shape: 'box', anchor: [1, 2], current: [3, 2], channels: [7, 9] });
});
