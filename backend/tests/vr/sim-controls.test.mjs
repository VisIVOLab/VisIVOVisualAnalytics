// The headset's simulation controls (issue #259, sim-controls.js and
// colour-bar.js): snapshots, columns, splats sized by a smoothing length, the
// box selection's filter, the colour bar's unit (node --test).
import test from 'node:test';
import assert from 'node:assert/strict';
import * as SC from '../../app/static/vr/js/sim-controls.js';
import * as CB from '../../app/static/vr/js/colour-bar.js';
import { niceTicks } from '../../app/static/vr/js/volume.js';
import { colormapTable } from '../../app/static/vr/js/colormaps.js';

const close = (a, b, eps = 1e-9) => assert.ok(Math.abs(a - b) <= eps, `${a} != ${b}`);

test('snapshots: a step within the series, its label, what the next one keeps', () => {
  const series = { paths: ['/d/snap_000.bin.head', '/d/snap_001.bin.head', '/d/snap_002.bin.head'], index: 1, count: 3 };
  assert.deepEqual(SC.seriesStep(series, 1), { index: 2, path: '/d/snap_002.bin.head' });
  assert.deepEqual(SC.seriesStep(series, -1), { index: 0, path: '/d/snap_000.bin.head' });
  assert.equal(SC.seriesStep({ ...series, index: 2 }, 1), null);
  assert.equal(SC.seriesStep(null, 1), null);
  assert.equal(SC.seriesLabel(series), 'snap_001.bin (2 of 3)');
  assert.equal(SC.seriesLabel({ paths: ['/a'], index: 0, count: 1 }), '');
  const prev = { axes: ['x', 'y', 'z'], colourField: 'rho', sizeField: 'hsml' };
  assert.deepEqual(SC.carryOver(prev, ['x', 'y', 'z', 'rho', 'hsml'], { axes: ['X', 'Y', 'Z'], colourField: null }),
    { axes: ['x', 'y', 'z'], colourField: 'rho', sizeField: 'hsml' });
  // One colour (none) was chosen: it stays none (#259 review), not the default column.
  assert.equal(SC.carryOver({ ...prev, colourField: null }, ['x', 'y', 'z', 'rho', 'hsml'], { axes: ['X', 'Y', 'Z'], colourField: 'rho' }).colourField, null);
  // A snapshot without those columns takes its own.
  assert.deepEqual(SC.carryOver(prev, ['X', 'Y', 'Z', 'T'], { axes: ['X', 'Y', 'Z'], colourField: 'T' }),
    { axes: ['X', 'Y', 'Z'], colourField: 'T', sizeField: null });
});

test('columns: cycling, and a smoothing length found by its usual names', () => {
  const f = ['x', 'y', 'z', 'vx', 'hsml'];
  assert.equal(SC.cycleField(f, 'x', 1), 'y');
  assert.equal(SC.cycleField(f, 'x', -1), 'hsml');
  assert.equal(SC.cycleField(f, 'nope', 1), 'x');
  assert.equal(SC.cycleField([], 'x', 1), null);
  // Columns found unusable (no positive size) are passed over.
  const bad = new Set(['vx', 'hsml']);
  assert.equal(SC.cycleField(f, 'z', 1, bad), 'x');
  assert.equal(SC.cycleField(f, 'x', -1, bad), 'z');
  assert.equal(SC.cycleField(['vx'], 'vx', 1, new Set(['vx'])), null);
  for (const name of ['h', 'HSML', 'SmoothingLength', 'smoothing_length', 'sml']) assert.equal(SC.smoothingField(['x', name]), name, name);
  assert.equal(SC.smoothingField(['x', 'y', 'z', 'Mass', 'height']), null);
});

test('sizes: the header, the codes after the colours, each splat h/√2 and its light kept', () => {
  const hdr = { 'X-Visivo-Size-Field': 'hsml', 'X-Visivo-Size-Range': '0.01,10' };
  const info = SC.sizeInfo((k) => hdr[k] || null);
  assert.deepEqual(info, { field: 'hsml', range: [0.01, 10] });
  assert.equal(SC.sizeInfo(() => null), null);
  assert.equal(SC.sizeInfo((k) => ({ 'X-Visivo-Size-Field': 'h', 'X-Visivo-Size-Range': '0,1' })[k]), null);
  const n = 4;
  const buf = new ArrayBuffer(10 * n);
  new Uint16Array(buf, 8 * n, n).set([0, 1, 2, 65535]);
  assert.deepEqual(Array.from(SC.sizeArray(buf, n, true)), [0, 1, 2, 65535]);
  assert.equal(SC.sizeArray(new ArrayBuffer(8 * n), n, true), null);
  close(SC.codeSize(0, [0.01, 10]), 0.01);
  close(SC.codeSize(65535, [0.01, 10]), 10, 1e-9);
  close(SC.codeSize(65535 / 3, [0.01, 10]), 0.1, 1e-9);
  // The shader's factor: f = h · mul, so f · radius = h / √2 · size · grow.
  const s = SC.sizedSplat([0.01, 10], { splatSize: 2, grow: 1.5, radius: 0.3 });
  close(s.log[0], -2); close(s.log[1], 3);
  close(0.3 * (1.0 * s.mul), (1.0 / Math.SQRT2) * 2 * 1.5);
  // The light kept: the uniform splat's over this one's, as drawn.
  close(SC.lightFactor(10, 20, 64), 0.25);                 // twice as wide: a quarter as bright
  close(SC.lightFactor(10, 80, 64), (10 / 64) ** 2);       // h ≥ the spacing: the sprite clamped at 64 px
  close(SC.lightFactor(10, 640, 64), (10 / 64) ** 2);      // no floor at 1/16: the clamped size counts
  assert.ok(SC.lightFactor(10, 640, 64) < 1 / 16);
  close(SC.lightFactor(10, 2, 64), 25);                    // smaller: brighter, no cap
  close(SC.lightFactor(10, 0.5, 64), 100 / (1 * 0.5));     // sub-pixel: one pixel at half its light
  close(SC.lightFactor(0.5, 0.25, 64), 0.5 / 0.25);        // both sub-pixel: their light factors
  close(SC.lightFactor(4, 4, 64), 1);
});

test('the box selection as the backend filter, in data units on the axis columns', () => {
  const bounds = [0, 10, -5, 5, 100, 200];
  const box = { min: [0.1, 0.5, 0], max: [0.6, 1, 0.25] };
  assert.deepEqual(SC.boxFilters(['x', 'y', 'z'], bounds, box, 'inside'),
    [{ box: { fields: ['x', 'y', 'z'], min: [1, 0, 100], max: [6, 5, 125] }, op: 'inside' }]);
  assert.equal(SC.boxFilters(['x', 'y', 'z'], bounds, box, 'outside')[0].op, 'outside');
  assert.deepEqual(SC.boxFilters(['x', 'y', 'z'], bounds, box, 'all'), []);
  assert.deepEqual(SC.boxFilters(['x', 'y', 'z'], bounds, null, 'inside'), []);
  assert.equal(SC.boxText(['x', 'y', 'z'], bounds, box), 'x 1 to 6, y 0 to 5, z 100 to 125');
});

test('the crop and the box keep their place in table units from one snapshot to the next', () => {
  const before = [0, 10, -5, 5, 100, 200];
  const box = { min: [0.2, 0.5, 0], max: [0.6, 1, 0.5] };
  const data = SC.toData(box, before);
  assert.deepEqual(data, { min: [2, 0, 100], max: [6, 5, 150] });
  // The next snapshot's box is larger: the same region is a smaller fraction of it.
  const after = [-10, 10, -5, 15, 100, 300];
  const f = SC.toFrac(data, after);
  for (let i = 0; i < 3; i++) {
    close(after[2 * i] + f.min[i] * (after[2 * i + 1] - after[2 * i]), data.min[i]);
    close(after[2 * i] + f.max[i] * (after[2 * i + 1] - after[2 * i]), data.max[i]);
  }
  // Partly outside: clamped to the new box; wholly outside an axis: null.
  assert.deepEqual(SC.toFrac({ min: [-20, -5, 100], max: [0, 5, 200] }, [-10, 10, -5, 5, 100, 200]).min, [0, 0, 0]);
  assert.equal(SC.toFrac({ min: [20, 0, 100], max: [30, 1, 200] }, [-10, 10, -5, 5, 100, 200]), null);
  // A flat axis (a planar table) is all of it.
  assert.deepEqual(SC.toFrac({ min: [0, 0, 3], max: [1, 1, 3] }, [0, 1, 0, 1, 3, 3]), { min: [0, 0, 0], max: [1, 1, 1] });
});

test('the colour bar: the unit from the table or the name, its title, ticks, and a drawing', () => {
  assert.equal(SC.unitOf({ mass: 'Msun' }, 'mass'), 'Msun');
  assert.equal(SC.unitOf({}, 'T (K)'), 'K');
  assert.equal(SC.unitOf({}, 'mass [1e10 Msun/h]'), '1e10 Msun/h');
  assert.equal(SC.unitOf({}, 'rho'), '');
  assert.equal(SC.colourBarTitle('mass', 'Msun', true), 'log mass [Msun]');
  assert.equal(SC.colourBarTitle('rho', ''), 'rho (no unit in the file)');
  assert.deepEqual(SC.colourBarTicks(0.5, 2000, true, niceTicks), [1, 10, 100, 1000]);
  assert.ok(SC.colourBarTicks(0, 1, false, niceTicks).length >= 3);
  close(CB.tickAt(10, 1, 100, true), 0.5);
  close(CB.tickAt(5, 0, 10, false), 0.5);
  const texts = [];
  const target = { measureText: (t) => ({ width: String(t).length * 11 }), fillText: (t) => texts.push(t),
    createLinearGradient: () => ({ addColorStop() {} }) };
  const c = new Proxy(target, { get: (t, k) => (k in t ? t[k] : () => {}), set: (t, k, v) => { t[k] = v; return true; } });
  CB.drawColourBar(c, { lut: colormapTable('viridis'), lo: 1, hi: 1000, log: true, title: 'log mass [Msun]', ticks: [1, 10, 100, 1000] });
  assert.ok(texts.includes('log mass [Msun]'));
  assert.ok(texts.includes('1') && texts.includes('1e3'));
  assert.equal(CB.tickLabel(1e11), '1e11');
  assert.equal(CB.tickLabel(250), '250');
  assert.equal(CB.tickLabel(-1234.5), '-1230');
  assert.notEqual(CB.barKey({ lo: 1, hi: 2, log: false, title: 'a', ticks: [] }, 'viridis'),
    CB.barKey({ lo: 1, hi: 2, log: false, title: 'a', ticks: [] }, 'magma'));
});
