// The crop in more detail (issue #328, crop-detail.js): which level and box
// are asked for, where the region is drawn, its codes on the whole cube's
// (as cube_reduce.quantise makes them), when it is asked for (node --test).
import test from 'node:test';
import assert from 'node:assert/strict';
import * as CD from '../../app/static/vr/js/crop-detail.js';

const levelsOf = (shape, n = 12) => {
  const out = [shape.slice()];
  while (out.length < n && Math.max(...out[out.length - 1]) > 8) out.push(out[out.length - 1].map((v) => Math.ceil(v / 2)));
  return out;
};

test('a quarter of a 2048³ cube binned 8× comes at level 1 (factor 2) into a 256³ capacity', () => {
  const full = [2048, 2048, 2048];
  const r = CD.planRegion({ clip: { min: [0.25, 0.25, 0.25], max: [0.5, 0.5, 0.5] }, fullShape: full,
    levels: levelsOf(full), capDims: [256, 256, 256], factors: [8, 8, 8] });
  assert.equal(r.level, 1);
  assert.equal(r.step, 2);
  assert.deepEqual(r.dims, [256, 256, 256]);
  assert.deepEqual(r.box, { x0: 256, x1: 511, y0: 256, y1: 511, z0: 256, z1: 511 });
  assert.deepEqual(r.lo, [0.25, 0.25, 0.25]);
  assert.deepEqual(r.hi, [0.5, 0.5, 0.5]);
  // A 384³ capacity holds the same quarter at level 1 too; an eighth of an axis at level 0.
  const e = CD.planRegion({ clip: { min: [0.5, 0.5, 0.5], max: [0.625, 0.625, 0.625] }, fullShape: full,
    levels: levelsOf(full), capDims: [384, 384, 384], factors: [8, 8, 8] });
  assert.equal(e.level, 0);
  assert.deepEqual(e.dims, [256, 256, 256]);
});

test('the crop is rounded out to whole level voxels; a partial last block covers the cube\'s end', () => {
  const full = [100, 60, 30];
  const r = CD.planRegion({ clip: { min: [0.13, 0, 0.5], max: [1, 0.51, 1] }, fullShape: full,
    levels: levelsOf(full), capDims: [40, 40, 40], factors: [8, 8, 8] });
  // Level 0 needs 87 voxels in x: too many; level 1: 44 > 40; level 2: 22.
  assert.equal(r.level, 2);
  assert.deepEqual(r.box, { x0: 3, x1: 24, y0: 0, y1: 7, z0: 3, z1: 7 });
  assert.deepEqual(r.dims, [22, 8, 5]);
  assert.equal(r.lo[0], 12 / 100);
  assert.equal(r.hi[0], 1, 'the last block of 100 at step 4 ends at the cube');
  assert.equal(r.hi[1], 32 / 60);
  // Where it is drawn: the region's own box, its partial block, the opacity per length kept.
  const d = CD.regionDraw(r, full, [4, 4, 4]);
  assert.deepEqual(d.lo, r.lo);
  assert.ok(Math.abs(d.scale[0] - 1 / (1 - 0.12)) < 1e-12);
  assert.ok(Math.abs(d.texScale[0] - (0.88 * 100) / (22 * 4)) < 1e-12);
  assert.equal(d.voxRatio, 1);
  const fine = CD.regionDraw({ ...r, step: 1 }, full, [4, 4, 4]);
  assert.ok(Math.abs(fine.voxRatio - 0.25) < 1e-12);
});

test('no region when it brings nothing: the whole cube, no level finer than the binning, nothing that fits', () => {
  const full = [256, 256, 256];
  const levels = levelsOf(full);
  const quarter = { min: [0.25, 0.25, 0.25], max: [0.5, 0.5, 0.5] };
  assert.equal(CD.planRegion({ clip: { min: [0, 0, 0], max: [1, 1, 1] }, fullShape: full, levels, capDims: [256, 256, 256], factors: [4, 4, 4] }), null);
  assert.equal(CD.planRegion({ clip: quarter, fullShape: full, levels, capDims: [256, 256, 256], factors: [1, 1, 1] }), null, 'not binned');
  assert.equal(CD.planRegion({ clip: quarter, fullShape: full, levels: [full], capDims: [16, 16, 16], factors: [4, 4, 4] }), null, 'nothing fits');
  assert.equal(CD.planRegion({ clip: quarter, fullShape: full, levels: [], capDims: [256, 256, 256], factors: [4, 4, 4] }), null, 'no levels (unsupported)');
  // Anisotropic binning: finer on one axis is a gain.
  assert.equal(CD.planRegion({ clip: quarter, fullShape: full, levels, capDims: [256, 256, 256], factors: [1, 1, 4] }).level, 0);
});

test('regionHolds: the detail stays while the crop is inside its region', () => {
  const r = { lo: [0.25, 0.25, 0.25], hi: [0.5, 0.5, 0.5] };
  assert.ok(CD.regionHolds(r, { min: [0.25, 0.3, 0.25], max: [0.5, 0.4, 0.5] }));
  assert.ok(!CD.regionHolds(r, { min: [0.2, 0.3, 0.25], max: [0.5, 0.4, 0.5] }));
  assert.ok(!CD.regionHolds(r, { min: [0, 0, 0], max: [1, 1, 1] }));
  assert.ok(!CD.regionHolds(null, { min: [0.3, 0.3, 0.3], max: [0.4, 0.4, 0.4] }));
});

test('quantise: the reduced cube\'s codes, linear and log, 8-bit and half floats', () => {
  const v = new Float32Array([NaN, -1, 0, 0.5, 1, 2, 1e-3, 100]);
  const u8 = CD.quantise(v, 0, 1, 'linear', 'uint8', new Uint8Array(v.length));
  assert.deepEqual([...u8], [0, 1, 1, 128, 255, 255, 1, 255]);
  // log: lo/hi are log10; values <= 0 blank; 1e-3 at code 1, 100 at 255.
  const lg = CD.quantise(v, -3, 2, 'log', 'uint8', new Uint8Array(v.length));
  assert.deepEqual([...lg], [0, 0, 0, 1 + Math.round((Math.log10(0.5) + 3) / 5 * 254), 1 + Math.round(3 / 5 * 254), 1 + Math.round((Math.log10(2) + 3) / 5 * 254), 1, 255]);
  const h = CD.quantise(new Float32Array([NaN, 0, 0.5, 1, -0.25]), 0, 1, 'linear', 'float16', new Uint16Array(5));
  assert.deepEqual([...h], [0xbc00, 0x0000, 0x3800, 0x3c00, 0xb400], 'blank -1, then 0, 0.5, 1, and an unclamped -0.25');
  // Slices: the same codes whatever the slicing.
  const big = new Float32Array(1000).map((_, i) => i / 999);
  const whole = CD.quantise(big, 0, 1, 'linear', 'uint8', new Uint8Array(1000));
  const parts = new Uint8Array(1000);
  for (let i = 0; i < 1000; i += 333) CD.quantise(big, 0, 1, 'linear', 'uint8', parts, i, Math.min(1000, i + 333));
  assert.deepEqual(parts, whole);
});

test('toHalf: exact for representable values, rounded to nearest even, subnormals and infinities', () => {
  assert.equal(CD.toHalf(1), 0x3c00);
  assert.equal(CD.toHalf(-2), 0xc000);
  assert.equal(CD.toHalf(65504), 0x7bff);
  assert.equal(CD.toHalf(1e6), 0x7c00);
  assert.equal(CD.toHalf(2 ** -24), 0x0001);
  assert.equal(CD.toHalf(2 ** -26), 0x0000);
  assert.equal(CD.toHalf(1 + 2 ** -11), 0x3c00, 'a tie to even');
  assert.equal(CD.toHalf(1 + 3 * 2 ** -11), 0x3c02);
});

test('DetailGate: a crop asked for once it stayed still, once per crop', () => {
  const g = new CD.DetailGate(500);
  g.note('a', 0);
  assert.equal(g.due(400), '');
  assert.equal(g.due(500), 'a');
  assert.equal(g.due(900), '', 'once');
  g.note('b', 1000);
  g.note('c', 1300);
  assert.equal(g.due(1700), '', 'a crop still moving restarts the wait');
  assert.equal(g.due(1800), 'c');
  g.reset();
  assert.equal(g.due(1900), 'c', 'asked again after a failure');
  g.note('', 2000);
  assert.equal(g.due(9000), '', 'no crop: nothing');
});

test('levelShapesXYZ: pyramid_status [z, y, x] to [x, y, z]', () => {
  assert.deepEqual(CD.levelShapesXYZ([[64, 128, 256], [32, 64, 128]]), [[256, 128, 64], [128, 64, 32]]);
  assert.deepEqual(CD.levelShapesXYZ(null), []);
});

test('quantise gives the backend\'s codes (cube_reduce.quantise: float32, half to even)', () => {
  // Expected values printed by app.cube_reduce.quantise for the same float32 inputs.
  const f = (a) => new Float32Array(a);
  const lin = CD.quantise(f([0.5 / 254, 1.5 / 254, 2.5 / 254, 0.123456, 0.987654, 3e-7, 0.5]), 0, 1, 'linear', 'uint8', new Uint8Array(7));
  assert.deepEqual([...lin], [1, 3, 3, 32, 252, 1, 128]);
  const w = f([1e-9, 3.3e-7, 2e-5, 7.7e-3, 0.99]);
  assert.deepEqual([...CD.quantise(w, -9, 0, 'log', 'uint8', new Uint8Array(5))], [1, 72, 122, 195, 255]);
  assert.deepEqual([...CD.quantise(w, -9, 0, 'log', 'float16', new Uint16Array(5))], [0, 13434, 14245, 14879, 15359]);
  assert.equal(CD.rint(0.5), 0);
  assert.equal(CD.rint(1.5), 2);
  assert.equal(CD.rint(2.5), 2);
  assert.equal(CD.rint(-0.5), -0);
  assert.equal(CD.rint(2.4), 2);
});

test('no region coarser than the cube on any axis (a spectral axis left unbinned)', () => {
  const full = [256, 256, 256];
  const levels = levelsOf(full);
  const quarter = { min: [0.25, 0.25, 0.25], max: [0.5, 0.5, 0.5] };
  // Level 0 fits a 64³ capacity: finer in x, y, the same in z.
  assert.equal(CD.planRegion({ clip: quarter, fullShape: full, levels, capDims: [64, 64, 64], factors: [4, 4, 1] }).level, 0);
  // Only level 1 fits a 32³ capacity: coarser in z than the cube: none.
  assert.equal(CD.planRegion({ clip: quarter, fullShape: full, levels, capDims: [32, 32, 32], factors: [4, 4, 1] }), null);
});
