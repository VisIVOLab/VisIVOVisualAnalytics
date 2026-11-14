// The moment of the selection in the headset and the product planes
// (issue #191): the desktop's request, labels and colours, decoding,
// orientation, units (node --test).
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { deflateSync } from 'node:zlib';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import * as TM from '../../app/static/vr/js/tools-moment.js';
import * as PC from '../../app/static/vr/js/product-canvas.js';
import * as Rc from '../../app/static/vr/js/recipe.js';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, '../../..');

function schemaFields(cls) {
  const src = readFileSync(path.join(REPO, 'backend/app/schemas.py'), 'utf8');
  const start = src.indexOf(`class ${cls}(BaseModel):`);
  assert.ok(start >= 0, cls);
  const end = src.indexOf('\nclass ', start + 10);
  return new Set([...src.slice(start, end).matchAll(/^ {4}(\w+): /gm)].map((m) => m[1]));
}

test('the moment request is the desktop\'s, under the route\'s field names', () => {
  const req = TM.momentRequest(1, 158);
  assert.deepEqual(req, { moment_order: 1, channel_start: 0, channel_end: 157, mask_enabled: false, threshold_value: 0, threshold_auto: false });
  const fields = schemaFields('MomentProductRequest');
  for (const k of Object.keys(req)) assert.ok(fields.has(k), k);
  // What the desktop sends (BackendClient::requestMoment): these seven keys, not moment2_variance.
  assert.deepEqual([...fields].filter((k) => !(k in req)).sort(), ['dataset_id', 'moment2_variance']);
});

test('the Provenance uses the desktop\'s labels; the replay isolates, then measures the new cube', () => {
  const req = TM.momentRequest(2, 158);
  const p = TM.momentParams(req, 'm/s', 'hcg44_lasso-outside.fits');
  assert.equal(p.Order, 'Moment 2 (Spectral dispersion σ)');
  assert.equal(p.Channels, '1–158');
  assert.equal(p.Mask, 'none');
  assert.equal(p.Scope, 'Remote full dataset');
  assert.equal(p.Unit, 'm/s');
  assert.match(p['Measured on'], /isolated cube hcg44_lasso-outside\.fits/);
  assert.equal(TM.momentParams(req, '', '').Unit, '-');
  const g = Rc.lassoGrowRequest([1, 2, 3]);
  const steps = TM.momentReplay(g, req);
  assert.deepEqual(steps.map((s) => s.route), ['/v1/cube/kinematic_select_bin', '/v1/cube/mask_apply_bin', '/v1/products/moment']);
  assert.deepEqual(steps[1].request, { mode: 'blank_outside' });
  // The chaining is in the request: the isolate's answer field, as a placeholder.
  assert.deepEqual(steps[2].request, { dataset_id: '{new_dataset_id}', ...req });
  assert.match(steps[2].body, /new_dataset_id/);
  assert.match(TM.MOMENT_CAVEAT, /whole cube/);
  assert.match(TM.MOMENT_CAVEAT, /desktop records no replay steps/);
  // A hand-edited mask: the apply step says so.
  assert.match(TM.momentReplay(g, req, true)[1].body, /^the hand-edited selection/);
});

test('the crop is the selection\'s box grown by a margin, within the map', () => {
  assert.deepEqual(TM.cropBox([10, 19, 30, 34], 100, 100), [7, 22, 27, 37]);       // 3 px at least
  assert.deepEqual(TM.cropBox([0, 59, 90, 99], 100, 100), [0, 65, 84, 99]);        // a tenth of 60
  assert.deepEqual(TM.cropBox([5, 5, 5, 5], 8, 8, 10), [0, 7, 0, 7]);
  const v = new Float32Array([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]);     // 4 × 3
  assert.deepEqual([...PC.crop(v, 4, 1, 2, 1, 2)], [5, 6, 9, 10]);
});

test('colours by kind: velocity on the desktop\'s robust symmetric range, the rest min..max', () => {
  const v = [1, 2, 3, 4, 5, 6, 7, 8, 9, NaN, 1000];
  const r = TM.colourRange(1, v);
  // median 5.5, MAD 2.5 -> ±4·1.4826·2.5
  assert.ok(Math.abs(r[0] - (5.5 - 14.826)) < 1e-9 && Math.abs(r[1] - (5.5 + 14.826)) < 1e-9, String(r));
  assert.deepEqual(TM.colourRange(0, v), [1, 1000]);
  assert.equal(PC.robustSymmetricRange([1, 2, 3]), null);
  assert.equal(TM.ORDER_LUT[0], 'inferno');
  assert.equal(TM.ORDER_LUT[1], 'coolwarm');
  assert.equal(TM.ORDER_LUT[2], 'viridis');
  for (const n of ['inferno', 'coolwarm', 'viridis']) assert.equal(PC.lut(n).length, 768);
});

test('units: a velocity in m/s reads in km/s with the numbers scaled, the rest as given', () => {
  assert.deepEqual(PC.displayUnit('JY/BEAM m/s'), { unit: 'JY/BEAM km/s', scale: 1e-3 });
  assert.deepEqual(PC.displayUnit('m/s'), { unit: 'km/s', scale: 1e-3 });
  assert.deepEqual(PC.displayUnit('km/s'), { unit: 'km/s', scale: 1 });
  assert.deepEqual(PC.displayUnit('Jy/beam km/s'), { unit: 'Jy/beam km/s', scale: 1 });
  assert.deepEqual(PC.displayUnit('(m/s)^2'), { unit: '(km/s)^2', scale: 1e-6 });
  assert.deepEqual(PC.displayUnit('Hz'), { unit: 'Hz', scale: 1 });
  assert.deepEqual(PC.displayUnit(''), { unit: '', scale: 1 });
});

test('a map is drawn with its row 0 at the bottom (FITS y up), NaN transparent', () => {
  const lut = PC.lut('viridis');
  // 2 × 2: row 0 = [0, 1], row 1 = [NaN, 1].
  const rgba = PC.colourise(new Float32Array([0, 1, NaN, 1]), 2, 2, [0, 1], lut);
  // Canvas row 0 (top) is the image's row 1.
  assert.equal(rgba[3], 0, 'NaN is transparent');
  assert.deepEqual([...rgba.slice(4, 8)], [lut[765], lut[766], lut[767], 255]);
  // Canvas row 1 (bottom) is the image's row 0: value 0 = the table's first entry.
  assert.deepEqual([...rgba.slice(8, 12)], [lut[0], lut[1], lut[2], 255]);
  // A row order (a PV diagram): rows from the bottom up.
  const pv = PC.colourise(new Float32Array([1, 0]), 1, 2, [0, 1], lut, [1, 0]);
  assert.deepEqual([...pv.slice(4, 7)], [lut[0], lut[1], lut[2]], 'bottom row = channel 1 (value 0)');
});

test('base64 and qt-zlib payloads decode as the backend encodes them', async () => {
  const f = new Float32Array([1.5, -2, NaN, 3e-8]);
  const b64 = Buffer.from(f.buffer).toString('base64');
  const back = PC.f32FromBase64(b64);
  assert.equal(back[0], 1.5); assert.equal(back[1], -2); assert.ok(Number.isNaN(back[2])); assert.equal(back[3], f[3]);
  const raw = Buffer.from(f.buffer);
  const z = deflateSync(raw);
  const q = Buffer.concat([Buffer.from([0, 0, 0, raw.length]), z]);       // qCompress: big-endian length + zlib
  const inflated = await PC.qtUncompress(new Uint8Array(q));
  assert.deepEqual([...inflated], [...raw]);
  await assert.rejects(PC.qtUncompress(new Uint8Array(Buffer.concat([Buffer.from([0, 0, 0, 9]), z]))), /expected 9/);
  const d = new Float64Array([880333.028, 888576.993]);
  assert.deepEqual([...PC.f64FromBase64(Buffer.from(d.buffer).toString('base64'))], [...d]);
});

test('ticks lie inside their span, at their fraction', () => {
  const t = PC.ticks(0.5, 10.5, 4);
  assert.ok(t.length >= 3);
  for (const k of t) {
    assert.ok(k.f >= 0 && k.f <= 1);
    assert.ok(Math.abs(0.5 + k.f * 10 - Number(k.text)) < 1e-9);
  }
  assert.deepEqual(PC.ticks(1, 1), []);
  const down = PC.ticks(10, 0, 2);
  assert.ok(down.every((k) => Math.abs(10 - 10 * k.f - Number(k.text)) < 1e-9), 'a descending span');
});

test('units: the FITS spellings of m/s read in km/s too', () => {
  for (const u of ['m s-1', 'm.s-1', 'm s^-1', 'm.s**-1', 'm / s']) {
    assert.deepEqual(PC.displayUnit(u), { unit: 'km/s', scale: 1e-3 }, u);
  }
  assert.deepEqual(PC.displayUnit('JY/BEAM m s-1'), { unit: 'JY/BEAM km/s', scale: 1e-3 });
  assert.deepEqual(PC.displayUnit('m2 s-2'), { unit: '(km/s)^2', scale: 1e-6 });
  assert.deepEqual(PC.displayUnit('km s-1'), { unit: 'km s-1', scale: 1 });
});

test('a velocity field: values outside its range transparent, as the desktop draws them', () => {
  const lut = PC.lut('coolwarm');
  const rgba = PC.colourise(new Float32Array([-5, 0.5, 9]), 3, 1, [0, 1], lut, null, true);
  assert.equal(rgba[3], 0);
  assert.equal(rgba[7], 255);
  assert.equal(rgba[11], 0);
  const clamped = PC.colourise(new Float32Array([-5]), 1, 1, [0, 1], lut);
  assert.equal(clamped[3], 255, 'other orders clamp');
});

// ── The tools together, over a stub backend (issues of the #191 review) ────

const { createSelection } = await import('../../app/static/vr/js/select-ui.js');
const TL = await import('../../app/static/vr/js/tools-lasso.js');

/** createSelection + lasso tools + moment tool over a stub backend that
 *  counts every call, with answers released by the test when `hold` is set. */
function harness() {
  const log = [];
  const held = [];
  let hold = false;
  let n = 0;
  const later = (v) => (hold ? new Promise((r) => held.push(() => r(v))) : Promise.resolve(v));
  const state = {
    info: { full_shape: [3, 3, 4], factors: [1, 1, 1], reduced_shape: [3, 3, 4], bunit: 'Jy/beam',
      axes: { x: { name: 'RA', unit: 'deg', world: [0, 0, 0] }, y: { name: 'DEC', unit: 'deg', world: [0, 0, 0] },
        z: { name: 'VELO', unit: 'm/s', world: [], full_velocity_kms: null } } },
    cube: { data: new Uint8Array(36).fill(200), dims: [3, 3, 4], encoding: 'uint8' }, open: { dataset_id: 'ds_1' },
    clip: { min: [0, 0, 0], max: [1, 1, 1] }, extent: [1, 1, 1], texScale: [1, 1, 1], thrNorm: 0.1, mode: 0,
  };
  const map = new Float32Array([NaN, 1, 2, NaN, 3, 4, NaN, NaN, NaN]);
  const backend = {
    session: 's',
    subvolume: async (ds, x0, x1, y0, y1, z0) => ({ values: new Float32Array([1]), dims: [1, 1, 1], origin: [x0, y0, z0] }),
    pixelToSky: async () => ({ valid: false }),
    kinematicSelect: async (ds, req) => { log.push(['grow']); return { mask: new Float32Array([1]), bytes: new ArrayBuffer(4), dims: [1, 1, 1], origin: [1, 1, 1], count: 7, trimBox: 0, ms: 1 }; },
    maskStats: async () => ({ bbox: [1, 1, 1, 1, 1, 1], flux_jy_kms: 1 }),
    _binary: async (url) => {
      const q = new URLSearchParams(url.split('?')[1]);
      const mode = q.get('mode');
      n++;
      log.push(['apply', mode, q.get('output_basename') || '']);
      const file = q.get('output_basename') || `c_${mode}_${n}.fits`;     // the backend's name, or the one asked for
      const body = { valid: true, new_dataset_id: `ds_${mode}_${n}`, workspace_filename: file, output_path: `/x/${file}`, width: 3, height: 3, depth: 4 };
      return later({ buf: new TextEncoder().encode(JSON.stringify(body)).buffer });
    },
    _json: async (method, url, body) => {
      log.push([method, url, body]);
      if (method === 'DELETE') return { valid: true, closed: true };
      return later({ valid: true, width: 3, height: 3, moment_unit: 'JY/BEAM m/s', bunit: 'JY/BEAM', data_base64: Buffer.from(map.buffer).toString('base64') });
    },
  };
  const sel = createSelection({ state, renderer: { setMask: () => {}, createPanel: () => ({}), updatePanel: () => {} },
    backend: () => backend, path: () => '/data/cube.fits', onChange: () => {} });
  const base = { selection: sel, backend: () => backend, datasetId: () => 'ds_1', info: () => state.info, onChange: () => {} };
  const lasso = TL.createLassoTools({ ...base, path: () => '/data/hcg44.fits' });
  const moment = TM.createMomentTool({ ...base, lassoTools: lasso, clipZ: () => [state.clip.min[2], state.clip.max[2]] });
  const flush = async () => { for (let i = 0; i < 40; i++) await Promise.resolve(); };
  const release = async () => { await flush(); while (held.length) { held.shift()(); await flush(); } };
  const grow = async () => { sel.setTool('lasso'); await sel.act({ o: [0.5, 0.5, 1.5], d: [0, 0, -1] }); };
  const applies = () => log.filter((e) => e[0] === 'apply');
  const kinds = () => sel.sel.recipes.map((r) => r.product.label);
  return { sel, lasso, moment, log, flush, release, grow, applies, kinds, state, setHold: (h) => { hold = h; } };
}

test('the moment isolates the selection into a temporary cube, not recorded, then measures it', async () => {
  const h = harness();
  await h.grow();
  await h.moment.compute(0);
  assert.equal(h.moment.state.error, '');
  const ap = h.applies();
  assert.equal(ap.length, 1);
  assert.equal(ap[0][1], 'blank_outside');
  assert.match(ap[0][2], /^hcg44_vr-moment-tmp-[0-9a-z]+\.fits$/, 'a distinctive temporary name');
  const post = h.log.find((e) => e[1] === '/v1/products/moment');
  assert.deepEqual(post[2], { dataset_id: 'ds_blank_outside_1', ...TM.momentRequest(0, 4) });
  assert.deepEqual(h.moment.state.result.range, [1, 4]);
  assert.equal(h.moment.state.result.summary.pixels, 4);
  assert.deepEqual(h.kinds(), ['Lasso: selection', 'Moment 0'], 'no isolated-cube product for a moment');
  const r = h.sel.sel.recipes.at(-1);
  assert.equal(r.product.replay_caveat, TM.MOMENT_CAVEAT);
  assert.equal(r.product.replay[2].request.dataset_id, '{new_dataset_id}');
  assert.match(r.parameters['Measured on'], /temporary isolated cube hcg44_vr-moment-tmp-.*deleted when the selection changes/);
});

test('the same moment pressed again is one product; another order is another', async () => {
  const h = harness();
  await h.grow();
  await h.moment.compute(0);
  await h.moment.compute(0);
  await h.moment.compute(1);
  assert.deepEqual(h.kinds(), ['Lasso: selection', 'Moment 0', 'Moment 1']);
  assert.equal(h.applies().length, 1, 'one temporary cube for the selection');
});

test('concurrent presses: one cube per kind, one record; a moment after Isolate uses its cube', async () => {
  const h = harness();
  await h.grow();
  h.setHold(true);
  const p = [h.lasso.run('isolate'), h.moment.compute(0), h.lasso.run('isolate'), h.moment.compute(1)];
  await h.release();
  await Promise.all(p);
  h.setHold(false);
  // Isolate first: the moments use its cube (no temporary one).
  assert.deepEqual(h.applies().map((a) => a[2]), ['']);
  assert.equal(h.kinds().filter((k) => k === 'Lasso: isolated cube').length, 1);
  assert.match(h.sel.sel.recipes.at(-1).parameters['Measured on'], /its isolated cube c_blank_outside_1\.fits \(outside blanked\)$/);
  // Remove, pressed twice for the same mask: one cube.
  await h.lasso.run('remove');
  await h.lasso.run('remove');
  assert.deepEqual(h.applies().map((a) => a[1]), ['blank_outside', 'blank_inside']);
  assert.equal(h.kinds().filter((k) => k === 'Lasso: removed cube').length, 1);
  // A moment first, then Isolate: two concurrent moments share one temporary
  // cube; Isolate makes its own (named as the desktop's), never the temporary one.
  const g = harness();
  await g.grow();
  g.setHold(true);
  const q = [g.moment.compute(0), g.moment.compute(1), g.lasso.run('isolate'), g.lasso.run('isolate')];
  await g.release();
  await Promise.all(q);
  g.setHold(false);
  const kinds = g.applies().map((a) => (a[2] ? 'tmp' : 'named'));
  assert.deepEqual(kinds, ['tmp', 'named']);
  assert.equal(g.kinds().filter((k) => k === 'Lasso: isolated cube').length, 1);
  assert.match(g.sel.sel.recipes.find((r) => r.product.label === 'Lasso: isolated cube').parameters.File, /^c_blank_outside_\d\.fits$/);
});

test('a new grow or Clear drops the moment and deletes only the temporary cube', async () => {
  const h = harness();
  await h.grow();
  await h.moment.compute(0);
  const tmp = h.applies()[0][2];
  assert.ok(h.moment.state.result);
  await h.grow();                              // a new selection
  h.moment.sync(); h.lasso.sync();
  await h.flush();
  assert.equal(h.moment.state.result, null);
  const dels = h.log.filter((e) => e[0] === 'DELETE').map((e) => e[1]);
  assert.deepEqual(dels, ['/v1/datasets/ds_blank_outside_1', `/v1/exports/${tmp}`]);
  assert.match(h.lasso.state.notice, new RegExp(`deleted ${tmp.replace('.', '\\.')}`));
  // A cube asked for (Isolate) is kept when the selection goes, and the moment used it.
  await h.lasso.run('isolate');
  await h.moment.compute(2);
  h.sel.clear();
  h.moment.sync(); h.lasso.sync();
  await h.flush();
  assert.equal(h.moment.state.result, null);
  assert.equal(h.log.filter((e) => e[0] === 'DELETE').length, 2, 'the isolated cube stays');
});

test('a first moment answering after the selection changed is dropped, its busy line too', async () => {
  const h = harness();
  await h.grow();
  h.setHold(true);
  const p = h.moment.compute(0);
  await h.flush();
  assert.match(h.moment.state.busy, /moment 0/);
  h.setHold(false);
  await h.grow();                              // while the isolate is in flight
  h.moment.sync();                             // the page's frame loop
  h.setHold(true);
  await h.release();
  await p;
  assert.equal(h.moment.state.result, null);
  assert.equal(h.moment.state.busy, '', 'not stuck busy');
  assert.ok(!h.kinds().includes('Moment 0'));
  // Without the sync in between: the early return clears it too.
  const g = harness();
  await g.grow();
  g.setHold(true);
  const q = g.moment.compute(1);
  await g.flush();
  g.setHold(false);
  await g.grow();
  g.setHold(true);
  await g.release();
  await q;
  assert.equal(g.moment.state.busy, '');
});

test('a shared selection: its own mask when it has one; else its grow, checked; else refused', async () => {
  const h = harness();
  await h.grow();
  const L = h.sel.sel.lasso;
  const grows = () => h.log.filter((e) => e[0] === 'grow').length;
  // With its mask (as #188's shared selection brings it): used as is, no grow.
  h.sel.sel.lasso = { bytes: new ArrayBuffer(4), origin: [1, 1, 1], dims: [1, 1, 1], request: L.request, count: 7 };
  let g0 = grows();
  await h.lasso.run('isolate');
  assert.equal(h.lasso.state.error, '');
  assert.equal(grows(), g0);
  // A mask whose size is not its box: refused.
  h.sel.sel.lasso = { bytes: new ArrayBuffer(8), origin: [1, 1, 1], dims: [1, 1, 1], request: L.request, count: 7 };
  await h.lasso.run('remove');
  assert.match(h.lasso.state.error, /mask has 8 bytes, its box 1×1×1 needs 4/);
  // Its grow only: grown again, origin, dims and count checked.
  h.sel.sel.lasso = { request: L.request, count: 7, origin: [1, 1, 1], dims: [1, 1, 1] };
  g0 = grows();
  await h.lasso.run('isolate');
  assert.equal(h.lasso.state.error, '');
  assert.equal(grows(), g0 + 1);
  assert.deepEqual(TL.seedsOf(h.sel.sel.lasso.request).seed, [L.request.seed_z, L.request.seed_y, L.request.seed_x]);
  h.sel.sel.lasso = { request: L.request, count: 99 };
  await h.lasso.run('remove');
  assert.match(h.lasso.state.error, /gives 7 voxels in 1×1×1 at \(1, 1, 1\), not its 99 voxels: not applied/);
  h.sel.sel.lasso = { request: L.request, count: 7, origin: [2, 1, 1], dims: [1, 1, 1] };
  await h.lasso.run('remove');
  assert.match(h.lasso.state.error, /not its 7 voxels in 1×1×1 at \(2, 1, 1\)/);
  // Edited by hand and without its mask: refused, with the reason.
  h.sel.sel.lasso = { request: L.request, count: 7, handEdited: true };
  g0 = grows();
  await h.lasso.run('isolate');
  assert.match(h.lasso.state.error, /edited by hand and came without its mask/);
  assert.equal(grows(), g0);
});

test('the moment of a received selection never records a grow it does not have', async () => {
  const recipes = [];
  // Exactly the contract fixed with #196 (no count, no stats: the map is not cropped).
  const box = { request: null, bytes: new ArrayBuffer(4), origin: [1, 1, 1], dims: [1, 1, 1], fullShape: [2, 2, 3],
    received: true, kind: 'box', source: 'headset', handEdited: false, box: [1, 1, 1, 1, 1, 1], parameters: null, shared: {} };
  const mask = { ...box, kind: 'lasso' };
  for (const L of [box, mask]) {
    recipes.length = 0;
    const sel = { sel: { lasso: L }, record: (product, params, origin, id) => { recipes.push({ product, params }); return { product: { id: id || 'm1' } }; } };
    const lassoTools = { apply: async () => ({ id: 'ds_iso', file: 'x_vr-moment-tmp-a.fits', temp: true, trimBox: 0 }) };
    const map = new Float32Array([1, 2, 3, 4]);
    const backend = { _json: async () => ({ valid: true, width: 2, height: 2, moment_unit: 'JY/BEAM m/s', bunit: 'JY/BEAM',
      data_base64: Buffer.from(map.buffer).toString('base64') }) };
    const tool = TM.createMomentTool({ selection: sel, lassoTools, backend: () => backend, datasetId: () => 'ds_1',
      info: () => ({ full_shape: [2, 2, 3] }), onChange: () => {} });
    await tool.compute(0);
    assert.equal(tool.state.error, '');
    const r = recipes[0].product;
    assert.ok(r.replay.every((st) => st.route !== '/v1/cube/kinematic_select_bin'), L.kind);
    assert.deepEqual(r.replay.map((st) => st.route), ['/v1/cube/mask_apply_bin', '/v1/products/moment']);
    if (L.kind === 'box') assert.equal(r.replay[0].request.width, 1);
    else assert.match(r.replay_caveat, /received without the request it was made with/);
    assert.match(r.replay_caveat, /^The last step measures the cube the step before it registers/);
  }
});

test('without a selection: the whole field over the velocity range, with the desktop dialog\'s mask (#257)', async () => {
  const h = harness();
  h.state.clip.min[2] = 0.25; h.state.clip.max[2] = 0.75;     // channels 1-2 of 4
  await h.moment.compute(1);
  assert.equal(h.moment.state.error, '');
  assert.equal(h.applies().length, 0, 'nothing isolated');
  const post = h.log.find((e) => e[1] === '/v1/products/moment');
  assert.deepEqual(post[2], { dataset_id: 'ds_1', moment_order: 1, channel_start: 1, channel_end: 2,
    mask_enabled: true, threshold_value: 0, threshold_auto: true });
  const fields = schemaFields('MomentProductRequest');
  for (const k of Object.keys(TM.wholeFieldRequest(0, [0, 3]))) assert.ok(fields.has(k), k);
  const r = h.moment.state.result;
  assert.ok(r.whole);
  assert.deepEqual(r.box, [0, 2, 0, 2], 'the whole map, not cropped');
  // Recorded, with its own replay (one step on the opened dataset) and Provenance.
  assert.deepEqual(h.kinds(), ['Moment 1']);
  const rec = h.sel.sel.recipes[0];
  assert.deepEqual(rec.product.replay.map((s) => s.route), ['/v1/products/moment']);
  assert.equal(rec.product.replay[0].request.dataset_id, 'ds_1');
  assert.match(JSON.stringify(rec), /whole field/);
  // A lasso grown afterwards does not drop it; a moment then is of the lasso object.
  await h.grow();
  h.moment.sync();
  assert.ok(h.moment.state.result && h.moment.state.result.whole);
  await h.moment.compute(0);
  assert.ok(!h.moment.state.result.whole);
  assert.equal(h.applies().length, 1, 'the lasso object is isolated for it');
});

test('a spectrum pinned stays through Clear, is drawn with the next one, and a second press unpins it (#257)', () => {
  const h = harness();
  assert.equal(h.sel.pinSpectrum(), false, 'nothing to pin');
  h.sel.sel.spectrum = { x: 1, y: 1, channel: 0, values: [1, 2, 3, 4], axis: [1, 2, 3, 4], axisLabel: 'velocity (km/s)', unit: 'Jy/beam', ms: 1 };
  assert.equal(h.sel.pinSpectrum(), true);
  assert.ok(h.sel.hasPin());
  h.sel.clear();
  assert.ok(h.sel.hasPin(), 'Clear keeps the pinned spectrum');
  h.state.open = { dataset_id: 'ds_other' };
  assert.ok(!h.sel.hasPin(), 'another cube: not shown');
  h.state.open = { dataset_id: 'ds_1' };
  assert.equal(h.sel.pinSpectrum(), true);
  assert.ok(!h.sel.hasPin(), 'unpinned');
});
