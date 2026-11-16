// Two or three variables of a simulation volume overlaid (issue #357):
// overlay.js (storage, channels, skip grid, apron, LUT atlas, looks,
// uniforms, the hub's word, the readout's values), quality.js (the budget
// and the storage's plan count channels), the hand menu's Overlay page and
// overlay-ui.js's storage and field choices (node --test).
import test from 'node:test';
import assert from 'node:assert/strict';
import * as OV from '../../app/static/vr/js/overlay.js';
import * as Q from '../../app/static/vr/js/quality.js';
import * as HM from '../../app/static/vr/js/hand-menu.js';
import { createOverlay } from '../../app/static/vr/js/overlay-ui.js';
import { prepareCube } from '../../app/static/vr/js/cube-prep.js';
import { buildMacrocells, apronBlocks, dataToNorm, normToData } from '../../app/static/vr/js/volume.js';
import { sampleNorm } from '../../app/static/vr/js/selection.js';
import { colormapTable, COLORMAP_NAMES, OVERLAY_COLORMAPS } from '../../app/static/vr/js/colormaps.js';

const close = (a, b, eps = 1e-9) => assert.ok(Math.abs(a - b) <= eps, `${a} != ${b}`);

/** Interleaved codes of `C` channels over `dims`, channel c of voxel i = f(i, c). */
function interleaved(dims, C, f) {
  const n = dims[0] * dims[1] * dims[2];
  const out = new Uint8Array(n * C);
  for (let i = 0; i < n; i++) for (let c = 0; c < C; c++) out[i * C + c] = f(i, c);
  return out;
}

test('storage channels and bytes: RG8 for two fields, RGB8 (counted as four bytes) for three', () => {
  assert.equal(OV.storageChannels(2), 2);
  assert.equal(OV.storageChannels(3), 3);
  assert.equal(OV.storageChannels(9), 3, 'a PLUTO output with nine variables: three at a time');
  assert.equal(OV.bytesPerVoxel(2), 2);
  assert.equal(OV.bytesPerVoxel(3), 4);
  assert.equal(OV.bytesPerVoxel(3, 'float16'), 8);
  const b = OV.overlayBytes([256, 256, 256], 3);
  assert.equal(b.volumeBytes, 256 ** 3 * 4);
  assert.equal(b.macroBytes, 32 ** 3 * 16, 'the skip grid: RGBA32F, one cell a 8³');
  assert.equal(b.totalBytes, b.volumeBytes + b.macroBytes);
});

test('the backend\'s answer cap: a grid whose fields would pass it is refused with the reason', () => {
  assert.equal(OV.tooLarge([256, 256, 256], 3), '', '48 MiB');
  assert.equal(OV.tooLarge([355, 355, 355], 3), '');
  assert.match(OV.tooLarge([512, 512, 512], 3), /384 MiB, more than the 128 MiB/);
  assert.equal(OV.tooLarge([512, 512, 256], 2), '', 'exactly 128 MiB');
});

test('the fields picked: a press adds or takes out, three at most, in channel order', () => {
  assert.deepEqual(OV.toggleField(['rho'], 'T'), ['rho', 'T']);
  assert.deepEqual(OV.toggleField(['rho', 'T'], 'rho'), ['T'], 'taken out: the others move up a channel');
  assert.deepEqual(OV.toggleField(['rho', 'T', 'vx'], 'prs'), ['rho', 'T', 'vx'], 'no fourth');
  assert.deepEqual(OV.defaultFields(['rho', 'vx1', 'vx2', 'prs'], 'vx2'), ['vx2', 'prs', 'rho'], 'the variable on show first, then the next ones');
  assert.deepEqual(OV.defaultFields(['rho', 'T'], 'zz'), ['rho', 'T']);
  assert.deepEqual(OV.defaultFields(['rho'], 'rho'), [], 'one variable: no overlay');
});

test('parseFields: each field on its own codes (log10 on a log scale), as one cube is read', () => {
  const header = JSON.stringify([
    { name: 'rho', path: '/d/s.bin::vbt-field=0', scale: 'log', range_min: 1e-12, range_max: 1e-5, log_min: -12, log_max: -5, threshold: 3e-9,
      nonpositive_voxels: 0, bunit: 'g/cm3', source: 'cube', pyramid_level: null, file_revision: 'a' },
    { name: 'vx', path: '/d/s.bin::vbt-field=2', scale: 'linear', range_min: -100, range_max: 100, log_min: null, log_max: null, threshold: null,
      nonpositive_voxels: 5, bunit: '', source: 'pyramid', pyramid_level: 2, file_revision: 'b' },
  ]);
  const f = OV.parseFields(header, 2);
  assert.deepEqual([f[0].lo, f[0].hi, f[0].scale], [-12, -5, 'log']);
  assert.deepEqual([f[1].lo, f[1].hi, f[1].scale], [-100, 100, 'linear']);
  assert.equal(f[0].valueMin, 1e-12);
  assert.equal(f[1].threshold, null);
  assert.equal(f[1].pyramidLevel, 2);
  assert.throws(() => OV.parseFields(header, 3), /2 field descriptions, not 3/);
  assert.throws(() => OV.parseFields('not json', 2), /X-Visivo-Fields/);
  assert.throws(() => OV.parseFields(JSON.stringify([{ name: 'x', scale: 'log', log_min: null, log_max: 1 }]), 1), /no value range/);
});

test('the skip grid keeps each field\'s maximum in its own channel; a channel of no field is -1', () => {
  const dims = [20, 12, 9], C = 3;
  const data = interleaved(dims, C, (i, c) => (c === 0 ? (i % 7 === 0 ? 200 : 1) : c === 1 ? (i % 13) * 9 : 0));
  const p = OV.prepareOverlay(data, dims, 'uint8', { cell: 8, channels: C, fields: 2, aboves: [0, 0.1] });
  const m0 = buildMacrocells(OV.channelOf(data, C, 0), dims, 'uint8', 8);
  const m1 = buildMacrocells(OV.channelOf(data, C, 1), dims, 'uint8', 8);
  assert.deepEqual(p.macro.dims, m0.dims);
  for (let k = 0; k < m0.data.length; k++) {
    close(p.macro.data[4 * k], m0.data[k]);
    close(p.macro.data[4 * k + 1], m1.data[k]);
    assert.equal(p.macro.data[4 * k + 2], -1, 'the third channel holds no field (two fields in RGB8)');
    assert.equal(p.macro.data[4 * k + 3], -1);
  }
  assert.equal(p.quantiles.length, 2);
  // Through cube-prep (the worker's entry point): the same answer.
  const q = prepareCube(data, dims, 'uint8', { cell: 8, channels: C, fields: 2, aboves: [0, 0.1] });
  assert.deepEqual([...q.macro.data], [...p.macro.data]);
});

test('the apron of interleaved fields is each channel\'s apron', () => {
  const dims = [5, 4, 3], store = [8, 8, 8], C = 3;
  const data = interleaved(dims, C, (i, c) => (i * 7 + c * 31) % 251 + 1);
  const a = OV.apronBlocksC(data, dims, store, C);
  for (let c = 0; c < C; c++) {
    const one = apronBlocks(OV.channelOf(data, C, c), dims, store);
    assert.equal(a.length, one.length);
    a.forEach((b, k) => {
      assert.deepEqual(b.offset, one[k].offset);
      assert.deepEqual(b.size, one[k].size);
      assert.deepEqual([...OV.channelOf(b.data, C, c)], [...one[k].data]);
    });
  }
});

test('the LUT atlas: row i is field i\'s colour map; the single-hue maps are not on the cube\'s swatches', () => {
  const atlas = OV.lutAtlas(['red', 'green', 'blue']);
  assert.equal(atlas.length, 256 * 4 * OV.LUT_ROWS);
  assert.deepEqual([...atlas.subarray(256 * 4, 2 * 256 * 4)], [...colormapTable('green')]);
  assert.ok(atlas.subarray(3 * 256 * 4).every((v) => v === 0), 'the fourth row: no field');
  close(OV.lutRow(1), 0.375);
  for (const m of ['red', 'green', 'blue']) {
    assert.ok(OVERLAY_COLORMAPS.includes(m));
    assert.ok(!COLORMAP_NAMES.includes(m));
    const t = colormapTable(m);
    assert.deepEqual([t[0], t[1], t[2]], [0, 0, 0], `${m} starts black (no light where the field is faint)`);
  }
  const r = colormapTable('red');
  assert.ok(r[128 * 4] > 2 * r[128 * 4 + 1], 'red is red half way up');
});

test('looks: the default from the backend\'s threshold; kept as values, put back on new codes', () => {
  const rho = { name: 'rho', path: 'p0', scale: 'log', lo: -12, hi: -5, threshold: 1e-9 };
  const d = OV.defaultLook(rho, 0, { cmaxNorm: 0.95 });
  assert.equal(d.colormap, 'red');
  close(d.thrNorm, dataToNorm(1e-9, -12, -5, 'log'));
  close(d.cmaxNorm, 0.95);
  assert.equal(d.stretch, 'linear', 'linear in log10: the desktop\'s Log');
  assert.equal(OV.defaultLook({ ...rho, scale: 'linear', lo: 0, hi: 1, threshold: 0.2 }, 1).stretch, 'sqrt');
  assert.equal(OV.defaultLook(rho, 2).colormap, 'blue');
  const kept = OV.keptLook({ ...d, thrNorm: 0.5, cmaxNorm: 0.8 }, rho);
  close(kept.threshold, normToData(0.5, -12, -5, 'log'), 1e-20);
  // The same field loaded again on a wider range: the same values on new codes.
  const wider = { ...rho, lo: -13, hi: -4 };
  const n = OV.lookOnField(kept, wider);
  close(normToData(n.thrNorm, -13, -4, 'log'), kept.threshold, 1e-15);
  close(normToData(n.cmaxNorm, -13, -4, 'log'), kept.colourMax, 1e-12);
  assert.equal(OV.lookOnField({ threshold: NaN, colourMax: 1 }, rho), null);
});

test('uniforms: a field switched off, or a channel with no field, never draws nor keeps a cell', () => {
  const fields = [{ scale: 'log', lo: -12, hi: -5 }, { scale: 'linear', lo: 0, hi: 1 }];
  const looks = [{ thrNorm: 0.3, cmaxNorm: 0.9, stretch: 'linear', opacity: 0.4, on: true },
    { thrNorm: 0.2, cmaxNorm: 0.1, stretch: 'sqrt', opacity: 0.7, on: false }];
  const u = OV.overlayUniforms(looks, fields, { stretchCode: (s) => ({ linear: 0, sqrt: 2 })[s], stretchA: () => 0 });
  assert.deepEqual(u.thr, [0.3, 2, 2]);
  assert.deepEqual(u.opacity, [0.4, 0, 0]);
  assert.deepEqual(u.stretch, [0, 2, 0]);
  close(u.cmax[1], 0.2 + 1e-3, 1e-12);
  assert.equal(u.n, 2);
  assert.equal(OV.anyOn(looks, 2), true);
  assert.equal(OV.anyOn([{ on: false }, { on: false }], 2), false);
});

test('the hub: each field with its look as values, and a one-line summary', () => {
  const fields = [{ name: 'rho', path: 'p0', scale: 'log', lo: -12, hi: -5 }, { name: 'vx', path: 'p2', scale: 'linear', lo: -100, hi: 100 }];
  const looks = [{ colormap: 'red', thrNorm: 0.5, cmaxNorm: 1, opacity: 0.4, stretch: 'linear', on: true },
    { colormap: 'blue', thrNorm: 0.6, cmaxNorm: 0.9, opacity: 0.3, stretch: 'sqrt', on: false }];
  const h = OV.hubOverlay(fields, looks, 'mip');
  assert.equal(h.payload.composite, 'mip');
  assert.deepEqual(h.payload.fields.map((f) => [f.name, f.colormap, f.scale, f.on]), [['rho', 'red', 'log', true], ['vx', 'blue', 'linear', false]]);
  close(h.payload.fields[0].threshold, 10 ** -8.5, 1e-20);
  close(h.payload.fields[1].threshold, 20, 1e-9);
  assert.equal(h.summary, 'Overlay (MIP): rho red log, vx (off) blue');
});

test('the readout: each field\'s value, filtered as one cube\'s readout is', () => {
  const dims = [6, 5, 4], C = 3;
  const data = interleaved(dims, C, (i, c) => (c === 2 ? 0 : 1 + ((i * (c + 3)) % 254)));
  const fields = [{ name: 'a', scale: 'linear', lo: 0, hi: 10, bunit: 'K' }, { name: 'b', scale: 'log', lo: -3, hi: 2 }];
  const p = [0.43, 0.61, 0.27];
  const v = OV.sampleFields({ data, dims, channels: C, encoding: 'uint8', fields }, p);
  for (const [c, f] of fields.entries()) {
    const n = sampleNorm({ data: OV.channelOf(data, C, c), dims, encoding: 'uint8' }, p);
    close(v[c].value, normToData(n, f.lo, f.hi, f.scale), 1e-9);
  }
  assert.equal(v[0].unit, 'K');
  const blank = OV.sampleFields({ data: new Uint8Array(6 * 5 * 4 * 3), dims, channels: C, encoding: 'uint8', fields }, p);
  assert.ok(Number.isNaN(blank[0].value), 'code 0 is blank');
});

test('quality: the budget counts channels; the storage is planned in what is left', () => {
  const one = Q.cubeBudget({ dims: [256, 256, 256], encoding: 'uint8' }, { headset: true });
  const three = Q.cubeBudget({ dims: [256, 256, 256], encoding: 'uint8', channels: 3 }, { headset: true });
  assert.equal(three.textureBytes, 4 * one.textureBytes);
  assert.equal(Q.cubeBudget({ dims: [64, 64, 64], encoding: 'uint8', channels: 2 }, { capacity: { dims: [256, 256, 256], encoding: 'uint8', totalBytes: 1 } }).inCapacity, false,
    'interleaved fields never go into the single-channel capacity');
  const MiB = 1024 * 1024;
  const ok = Q.planOverlay({ capDims: [256, 256, 256], channels: 3, limitBytes: 200 * MiB });
  assert.equal(ok.ok, true);
  assert.equal(ok.totalBytes, OV.overlayBytes([256, 256, 256], 3).totalBytes);
  const no = Q.planOverlay({ capDims: [256, 256, 256], channels: 3, limitBytes: 60 * MiB });
  assert.equal(no.ok, false);
  assert.match(no.reason, /needs 65 MiB; 60 MiB is left/);
  // A Quest 3's share (256 MiB) with the 256³ uint8 capacity (49 MiB): three fields fit.
  const capBytes = Q.capacityBytes([256, 256, 256], 'uint8').totalBytes;
  assert.equal(Q.planOverlay({ capDims: [256, 256, 256], channels: 3, limitBytes: Q.textureLimitBytes({ headset: true }) - capBytes }).ok, true);
  assert.equal(Q.planOverlay({ capDims: null, channels: 2, limitBytes: 1e9 }).ok, false);
  assert.equal(Q.sameOverlay(ok, { dims: [256, 256, 256], channels: 3, encoding: 'uint8', cell: 8 }), true);
  assert.equal(Q.sameOverlay(ok, { dims: [256, 256, 256], channels: 2, encoding: 'uint8', cell: 8 }), false);
  assert.equal(Q.sameOverlay(null, null), true);
});

const simCtx = (ov, extra = {}) => ({
  ...HM.defaultContext(), kind: 'cube', simulation: true, vars: { names: ['rho', 'T', 'vx', 'prs'], current: 'rho' },
  ov: { fields: [], edit: 0, composite: 'sum', shown: false, loading: false, room: '', fieldPage: 0, ...ov }, ...extra,
});

test('the hand menu: Display leads to the Overlay page only for a simulation with variables', () => {
  const ids = (ctx) => HM.pageSpec('display', ctx).widgets.map((w) => w.id);
  assert.ok(ids(simCtx({})).includes('overlay-page'));
  assert.ok(!ids({ ...simCtx({}), ov: null }).includes('overlay-page'));
  assert.ok(!ids({ ...HM.defaultContext(), kind: 'cube' }).includes('overlay-page'), 'a radio cube');
  // While the overlay is drawn, the single variable's look is greyed (the picker stays).
  const on = HM.pageSpec('display', simCtx({ shown: true, fields: [{ name: 'rho' }, { name: 'T' }] })).widgets;
  assert.equal(on.find((w) => w.id === 'thr').disabled, HM.WHY.ovOn);
  assert.equal(on.find((w) => w.id === 'overlay-page').disabled, '');
  assert.equal(on.find((w) => w.id === 'var:T').disabled, '');
  // On with every field hidden: the single cube is drawn, its look not greyed.
  const hidden = HM.pageSpec('display', simCtx({ shown: true, drawing: false, fields: [{ name: 'rho' }, { name: 'T' }] })).widgets;
  assert.equal(hidden.find((w) => w.id === 'thr').disabled, '');
  assert.equal(HM.SUB_PAGES.overlay, 'display');
  assert.equal(HM.tabOf('overlay'), 'display');
});

test('the Overlay page: fields numbered by channel, per-field rows, greyed with the reason', () => {
  const fields = [{ name: 'T', colormap: 'red', log: true, on: true }, { name: 'rho', colormap: 'green', log: true, on: false }];
  const s = HM.pageSpec('overlay', simCtx({ fields, edit: 1, shown: true }));
  const w = (id) => s.widgets.find((x) => x.id === id);
  assert.equal(w('ov-field:T').label, '1 T');
  assert.equal(w('ov-field:rho').label, '2 rho');
  assert.equal(w('ov-field:vx').label, 'vx');
  assert.equal(w('ov-edit:1').on, true);
  assert.equal(w('ov-show-field').label, 'Hidden', 'the edited field (rho) is hidden');
  assert.equal(w('ov-cmap:green').on, true);
  assert.equal(w('ov-log').on, true);
  assert.equal(w('ov-on').label, 'On');
  assert.ok(s.widgets.every((x) => x.row <= 5));
  // One field: nothing to overlay yet.
  const one = HM.pageSpec('overlay', simCtx({ fields: [{ name: 'T', colormap: 'red', log: false, on: true }] }));
  assert.equal(one.widgets.find((x) => x.id === 'ov-on').disabled, HM.WHY.ovFields);
  // No room on this device: every control says why.
  const room = HM.pageSpec('overlay', simCtx({ fields, room: 'An overlay of 3 fields at 256×256×256 needs 65 MiB; 10 MiB is left.' }));
  assert.ok(room.widgets.filter((x) => x.id.startsWith('ov-')).every((x) => /needs 65 MiB/.test(x.disabled)));
  // Loading: the fields wait.
  assert.equal(HM.pageSpec('overlay', simCtx({ fields, loading: true })).widgets.find((x) => x.id === 'ov-field:vx').disabled, HM.WHY.ovLoading);
  // Nine PLUTO variables: four a page and "more".
  const many = { ...simCtx({ fields }), vars: { names: ['rho', 'vx1', 'vx2', 'vx3', 'prs', 'tr1', 'tr2', 'tr3', 'tr4'], current: 'rho' } };
  const p0 = HM.pageSpec('overlay', many).widgets.filter((x) => x.row === 0).map((x) => x.id);
  assert.deepEqual(p0, ['ov-field:rho', 'ov-field:vx1', 'ov-field:vx2', 'ov-field:vx3', 'ov-fields-more']);
  assert.deepEqual(HM.overlayFieldChips(many.vars.names, 2).chips, ['tr4']);
});

/** A renderer stand-in for overlay-ui.js: the storage calls only. */
function fakeRenderer({ cap = [256, 256, 256], held = 0 } = {}) {
  const R = {
    xrActive: false, maxTex3D: 2048, cap: cap ? { dims: cap } : null, ov: null, calls: [],
    gpuStats() { return { cubeBytes: held + (R.ov ? R.ov.totalBytes : 0), overlayBytes: R.ov ? R.ov.totalBytes : 0 }; },
    allocOverlay(dims, channels, encoding, cell) {
      R.calls.push('alloc');
      R.ov = { dims, channels, encoding, cell, totalBytes: OV.overlayBytes(dims, channels).totalBytes, allocMs: 1, shown: null };
      return R.ov;
    },
    releaseOverlay() { R.calls.push('release'); R.ov = null; },
    hideOverlay() { if (R.ov) R.ov.shown = null; },
    setOverlayColormaps() {},
  };
  return R;
}

function overlayWith(R, state, limit = 256 * 1024 * 1024) {
  return createOverlay({ R, state, backend: () => null, cubePrep: null, nextTask: () => Promise.resolve(), writeStepMs: 4,
    limitBytes: () => limit, isSimulation: () => !!(state.info && state.info.simulation), log() {}, event() {}, toast() {}, changed() {},
    dropDetail() {}, sliderToNorm: (v) => v / 1000, normToSlider: (v) => v * 1000 });
}

const simState = (names = ['rho', 'T', 'vx']) => ({
  cube: { mode: 'mean', dims: [64, 64, 64], factors: [1, 1, 1] }, open: { dataset_id: 'd' },
  info: { simulation: true, field_name: names[0], fields: names.map((n, i) => ({ name: n, path: `/d/s.bin::vbt-field=${i}` })) },
});

test('overlay-ui: the storage follows the cube outside VR, within what is left of the share', () => {
  const state = simState();
  const R = fakeRenderer({ held: 51 * 1024 * 1024 });
  const o = overlayWith(R, state);
  o.ensureStorage();
  assert.deepEqual([R.ov.dims, R.ov.channels], [[256, 256, 256], 3]);
  assert.equal(o.ov.room, '');
  o.ensureStorage();
  assert.deepEqual(R.calls, ['alloc'], 'the same plan: nothing allocated again');
  // In VR nothing is allocated or given back.
  R.xrActive = true;
  state.info = { simulation: false };
  o.ensureStorage();
  assert.ok(R.ov);
  R.xrActive = false;
  // A radio cube outside VR: given back.
  o.ensureStorage();
  assert.equal(R.ov, null);
  assert.match(o.ov.room, /two variables or more/);
  // Two fields: RG8; too little room: none, and why.
  const two = simState(['rho', 'vx']);
  const R2 = fakeRenderer({ held: 230 * 1024 * 1024 });
  const o2 = overlayWith(R2, two);
  o2.ensureStorage();
  assert.equal(R2.ov, null);
  assert.match(o2.ov.room, /needs 33 MiB/);
  const R3 = fakeRenderer({ held: 0 });
  const o3 = overlayWith(R3, two);
  o3.ensureStorage();
  assert.equal(R3.ov.channels, 2);
});

test('overlay-ui: the fields picked follow the source; the menu\'s context', () => {
  const state = simState(['rho', 'T', 'vx', 'prs']);
  const R = fakeRenderer();
  const o = overlayWith(R, state);
  o.onCube(state.info, '/var/d/s.bin::vbt-field=0');
  assert.deepEqual(o.ov.fields, ['rho', 'T', 'vx']);
  assert.ok(o.press('ov-field:T', { group: 'ov-field', value: 'T' }));
  assert.deepEqual(o.ov.fields, ['rho', 'vx']);
  // Another variable of the same source (spelled /private/var by the backend): the choice stays.
  o.onCube({ ...state.info, field_name: 'vx' }, '/private/var/d/s.bin::vbt-field=2');
  assert.deepEqual(o.ov.fields, ['rho', 'vx']);
  const ctx = o.menuContext();
  assert.deepEqual(ctx.fields.map((f) => [f.name, f.colormap]), [['rho', 'red'], ['vx', 'green']]);
  assert.equal(ctx.shown, false);
  // Another source: its own default.
  o.onCube({ ...state.info, fields: [{ name: 'a', path: '/e/o.bin::vbt-field=0' }, { name: 'b', path: '/e/o.bin::vbt-field=1' }], field_name: 'b' }, '/e/o.bin::vbt-field=1');
  assert.deepEqual(o.ov.fields, ['b', 'a']);
  assert.equal(o.drawn(), false);
});
