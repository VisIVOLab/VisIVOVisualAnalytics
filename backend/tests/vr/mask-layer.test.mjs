// A label mask over the cube (issue #282): the backend's wire, the edits
// (New / Add / Subtract that never take another source's voxels, undo), the
// save body (read back as the backend reads it), the drawing on the reduced
// grid (interior, shell, the selected source), the skip cells, the pick and
// the labels.
import test from 'node:test';
import assert from 'node:assert/strict';
import * as ML from '../../app/static/vr/js/mask-layer.js';
import * as MW from '../../app/static/vr/js/mask-window.js';
import { createMasks } from '../../app/static/vr/js/mask-ui.js';
import { commandUsable, PRODUCT_KINDS, productSummary } from '../../app/static/vr/js/headset.js';
import { WINDOWS, WindowPointer } from '../../app/static/vr/js/windows.js';
import * as HM from '../../app/static/vr/js/hand-menu.js';

const shape = [10, 8, 6];
const lin = (x, y, z) => (z * shape[1] + y) * shape[0] + x;

/** The backend's wire (app/mask_layer.py pack_layer) for {id: [indices]}. */
function wire(sources) {
  const ids = Object.keys(sources).map(Number).sort((a, b) => a - b);
  const all = [];
  const srcs = ids.map((id) => {
    const idx = [...sources[id]].sort((a, b) => a - b);
    const offset = all.length;
    all.push(...idx);
    const xyz = idx.map((v) => [v % shape[0], Math.floor(v / shape[0]) % shape[1], Math.floor(v / (shape[0] * shape[1]))]);
    const mm = (k) => [Math.min(...xyz.map((p) => p[k])), Math.max(...xyz.map((p) => p[k]))];
    const mean = (k) => xyz.reduce((s, p) => s + p[k], 0) / xyz.length;
    return { id, count: idx.length, offset, bbox: [...mm(0), ...mm(1), ...mm(2)], centroid: [mean(0), mean(1), mean(2)] };
  });
  let text = new TextEncoder().encode(JSON.stringify({ shape, sources: srcs, max_id: Math.max(0, ...ids), n_voxels: all.length, mask_name: 'm.fits' }));
  const pad = (4 - ((4 + text.length) % 4)) % 4;
  const buf = new Uint8Array(4 + text.length + pad + 4 * all.length);
  new DataView(buf.buffer).setUint32(0, text.length + pad, true);
  buf.set(text, 4);
  buf.fill(32, 4 + text.length, 4 + text.length + pad);
  new Uint32Array(buf.buffer, 4 + text.length + pad).set(all);
  text = null;
  return buf.buffer;
}

const box = (x0, x1, y0, y1, z0, z1) => {
  const out = [];
  for (let z = z0; z <= z1; z++) for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) out.push(lin(x, y, z));
  return out;
};

test('the wire is read: sources, sorted indices, bbox and centroid', () => {
  const p = ML.parseLayer(wire({ 5: box(3, 5, 2, 3, 1, 2), 2: [lin(8, 6, 4)] }));
  assert.deepEqual(p.shape, shape);
  assert.deepEqual(p.sources.map((s) => s.id), [2, 5]);
  assert.equal(p.sources[1].count, 12);
  assert.deepEqual(p.sources[1].bbox, [3, 5, 2, 3, 1, 2]);
  assert.equal(p.maxId, 5);
  const L = new ML.MaskLayer(p);
  assert.equal(L.idAt(lin(4, 3, 2)), 5);
  assert.equal(L.idAt(lin(8, 6, 4)), 2);
  assert.equal(L.idAt(lin(0, 0, 0)), 0);
  assert.throws(() => ML.parseLayer(new ArrayBuffer(2)));
});

test('sorted set operations', () => {
  const a = Uint32Array.from([1, 3, 5, 7]), b = Uint32Array.from([2, 3, 7, 9]);
  assert.deepEqual([...ML.union(a, b)], [1, 2, 3, 5, 7, 9]);
  assert.deepEqual([...ML.minus(a, b)], [1, 5]);
  assert.ok(ML.sortedHas(a, 5) && !ML.sortedHas(a, 4));
});

test('a lasso grow becomes sorted full-grid indices', () => {
  // A 3 x 2 x 1 box at (2, 1, 3), two voxels selected.
  const mask = new Float32Array([1, 0, 0, 0, 0, 1]);
  assert.deepEqual([...ML.growIndices(mask, [3, 2, 1], [2, 1, 3], shape)], [lin(2, 1, 3), lin(4, 2, 3)]);
});

test('edits: Add takes free voxels only, Subtract gives back, New replaces, a new source gets the next ID, undo', () => {
  const L = new ML.MaskLayer(ML.parseLayer(wire({ 5: box(3, 5, 2, 3, 1, 2), 2: [lin(8, 6, 4)] })));
  const grow = Uint32Array.from([lin(6, 2, 1), lin(8, 6, 4), lin(3, 2, 1)].sort((a, b) => a - b));
  L.apply(5, grow, 'add');
  assert.equal(L.get(5).count, 13, 'one free voxel added; source 2\'s voxel left alone, its own kept');
  assert.equal(L.get(2).count, 1);
  assert.equal(L.get(5).bbox[1], 6);
  L.apply(5, Uint32Array.from(box(3, 5, 2, 3, 1, 1)), 'carve');
  assert.equal(L.get(5).count, 7);
  L.apply(5, Uint32Array.from([lin(0, 0, 0), lin(8, 6, 4)].sort((a, b) => a - b)), 'new');
  assert.deepEqual([...L.get(5).idx], [lin(0, 0, 0)]);
  const r = L.apply(null, Uint32Array.from([lin(1, 1, 1), lin(0, 0, 0)].sort((a, b) => a - b)), 'new');
  assert.equal(r.id, 6);
  assert.deepEqual([...L.get(6).idx], [lin(1, 1, 1)], 'never another source\'s voxel');
  assert.ok(L.get(6).isNew);
  assert.equal(L.apply(null, Uint32Array.from([1]), 'carve').id, 0, 'Subtract with no source does nothing');
  // Undo, step by step back to the start.
  assert.equal(L.undo(), 6);
  assert.equal(L.get(6), null);
  assert.equal(L.maxId, 5);
  L.undo(); L.undo(); L.undo();
  assert.equal(L.get(5).count, 12);
  assert.equal(L.get(5).edited, false);
  assert.ok(!L.canUndo());
  assert.equal(L.undo(), 0);
});

test('the save body: edited and new sources with their voxels, emptied originals removed', () => {
  const L = new ML.MaskLayer(ML.parseLayer(wire({ 5: box(3, 5, 2, 3, 1, 2), 2: [lin(8, 6, 4)] })));
  L.apply(2, Uint32Array.from([lin(8, 6, 4)]), 'carve');
  L.apply(null, Uint32Array.from([lin(1, 1, 1)]), 'new');
  const { body, edits, removed } = L.saveBody({ dataset_id: 'c1', mask_path: '/d/m.fits' });
  assert.equal(edits, 1);
  assert.equal(removed, 1);
  // Read as app/mask_layer.py parse_edits reads it.
  const n = new DataView(body.buffer).getUint32(0, true);
  const head = JSON.parse(new TextDecoder().decode(body.subarray(4, 4 + n)));
  const pos = 4 + n + ((4 - ((4 + n) % 4)) % 4);
  assert.deepEqual(head.edits, [{ id: 6, count: 1, new: true }]);
  assert.deepEqual(head.removed, [2]);
  assert.equal(head.mask_path, '/d/m.fits');
  assert.deepEqual([...new Uint32Array(body.buffer.slice(pos))], [lin(1, 1, 1)]);
});

test('the reduced grid: interior, shell and the selected source; the skip cells', () => {
  const L = new ML.MaskLayer(ML.parseLayer(wire({ 5: box(2, 7, 1, 6, 1, 4), 2: [lin(9, 7, 5)] })));
  const enc = ML.encodeLayer(L, [1, 1, 1], shape, 0);
  assert.deepEqual(enc.origin, [2, 1, 1]);
  assert.deepEqual(enc.dims, [8, 7, 5]);
  const at = (x, y, z) => enc.data[((z - enc.origin[2]) * enc.dims[1] + (y - enc.origin[1])) * enc.dims[0] + (x - enc.origin[0])];
  assert.equal(at(4, 3, 2), ML.ENC.inside);
  assert.equal(at(2, 3, 2), ML.ENC.shell);
  assert.equal(at(9, 7, 5), ML.ENC.shell);
  assert.equal(at(8, 3, 2), ML.ENC.none);
  const sel = ML.encodeLayer(L, [1, 1, 1], shape, 2);
  assert.equal(sel.data[sel.data.length - 1], ML.ENC.selected);
  // Binned 2 x 2 x 1: any full voxel marks its block.
  const b = ML.encodeLayer(L, [2, 2, 1], [5, 4, 6], 0);
  assert.deepEqual(b.origin, [1, 0, 1]);
  assert.deepEqual(b.dims, [4, 4, 5]);
  const cells = ML.layerCells(enc, [2, 1, 1], 8);
  assert.deepEqual([...cells], [1, 1]);
  assert.equal(ML.encodeLayer(new ML.MaskLayer(ML.parseLayer(wire({}))), [1, 1, 1], shape), null);
  // The values the shader tells apart.
  assert.ok(ML.ENC.inside / 255 > 0.15 && ML.ENC.inside / 255 < 0.35);
  assert.ok(ML.ENC.shell / 255 > 0.35 && ML.ENC.shell / 255 < 0.5, 'below the selection\'s 0.5');
});

test('a source is picked along the ray, inside the crop only', () => {
  const L = new ML.MaskLayer(ML.parseLayer(wire({ 5: box(3, 5, 2, 3, 1, 2), 2: box(3, 5, 2, 3, 4, 4) })));
  const o = [(4 + 0.5) / 10, (2.5 + 0.5) / 8, -0.5], d = [0, 0, 1];
  const p = ML.pickSource(L, o, d);
  assert.equal(p.id, 5);
  assert.deepEqual(p.voxel, [4, 3, 1]);
  const behind = ML.pickSource(L, o, d, { min: [0, 0, 3.5 / 6], max: [1, 1, 1] });
  assert.equal(behind.id, 2);
  assert.equal(ML.pickSource(L, [0.05, 0.05, -1], d), null);
});

test('labels at the centroids, the selected one first and larger; the visit row', () => {
  const L = new ML.MaskLayer(ML.parseLayer(wire({ 5: box(3, 5, 2, 3, 1, 2), 2: [lin(8, 6, 4)] })));
  const labels = ML.layerLabels(L, [1, 0.8, 0.6], { min: [0, 0, 0], max: [1, 1, 1] }, 5);
  assert.deepEqual(labels.map((l) => l.text), ['5', '2']);
  assert.ok(labels[0].height > labels[1].height);
  const near = ML.layerLabels(L, [1, 0.8, 0.6], { min: [0, 0, 0], max: [1, 1, 0.5] }, 0);
  assert.deepEqual(near.map((l) => l.text), ['5']);
  assert.deepEqual(ML.sourceRow(L, 5), { key: 'm:5', label: 'Source 5', x: 4, y: 2.5, z: 1.5 });
  assert.equal(ML.sourceOutline(L, 5).ellipse.length, 5);
});

test('the Mask window: no overlap, the greyed reasons, pressed through the pointer', () => {
  const v = { loaded: true, mode: 'outline', labels: true, selected: 5, picking: false, newSource: false, seedMode: 'add', canUndo: true, canSave: false, nSources: 17 };
  const lay = MW.maskLayout(v);
  const box = (w) => [w.rect[0], w.rect[1], w.rect[0] + w.rect[2], w.rect[1] + w.rect[3]];
  for (let i = 0; i < lay.length; i++) {
    const a = box(lay[i]);
    assert.ok(a[0] >= 0 && a[1] >= 0 && a[2] <= MW.MASK_PX[0] && a[3] <= MW.MASK_PX[1], lay[i].id);
    for (let j = i + 1; j < lay.length; j++) {
      const b = box(lay[j]);
      assert.ok(!(Math.min(a[2], b[2]) > Math.max(a[0], b[0]) && Math.min(a[3], b[3]) > Math.max(a[1], b[1])), `${lay[i].id} / ${lay[j].id}`);
    }
  }
  const by = (l, id) => l.find((w) => w.id === id);
  assert.ok(by(lay, 'mode:outline').on && by(lay, 'seed:add').on && by(lay, 'labels').on);
  assert.equal(by(lay, 'save').disabled, 'Nothing edited yet.');
  const none = MW.maskLayout({ ...v, loaded: false });
  assert.match(by(none, 'pick').disabled, /No mask/);
  const nosel = MW.maskLayout({ ...v, selected: 0 });
  assert.equal(by(nosel, 'visit').disabled, 'Pick a source first.');
  assert.match(by(nosel, 'seed:new').disabled, /Pick a source, or New/);
  const wp = new WindowPointer(lay);
  const w = by(lay, 'mode:residual');
  const hit = { x: w.rect[0] + 5, y: w.rect[1] + 5, t: 1 };
  wp.update({ bar: null, content: hit }, false);
  assert.deepEqual(wp.update({ bar: null, content: hit }, true).map((a) => [a.id, a.content]), [['mode:residual', true]]);
  assert.deepEqual(WINDOWS.mask.contentPx, MW.MASK_PX);
});

test('the hub: a mask command is used, a mask product is known; the Select tab\'s Mask row', () => {
  assert.ok(commandUsable({ seq: 3, type: 'mask', dataset_id: 'ds', path: '/d/m.fits' }));
  assert.ok(!commandUsable({ seq: 3, type: 'teleport', dataset_id: 'ds', path: '/d/m.fits' }));
  assert.ok(PRODUCT_KINDS.includes('mask'));
  assert.match(productSummary({ product: 'mask', label: 'Refined mask x.fits' }, 'c.fits', 'x.fits'), /Refined mask x\.fits → x\.fits/);
  const ctx = { ...HM.defaultContext(), kind: 'cube' };
  assert.match(HM.widgetById(HM.menuLayout('select', ctx), 'mask-pick').disabled, /No mask/);
  const on = { ...ctx, mask: { loaded: true, shown: true, picking: true, canUndo: true, canSave: true } };
  assert.equal(HM.pressesTo('mask-save', 'display', on), 2);
  assert.ok(HM.widgetById(HM.menuLayout('select', on), 'mask-pick').armed);
});

test('the controller: load, pick, a lasso press edits the source, undo, save body', async () => {
  const sources = { 5: box(3, 5, 2, 3, 1, 2), 2: [lin(8, 6, 4)] };
  const calls = [];
  const backend = {
    session: 's1',
    maskLayer: async (id, mask) => { calls.push(['layer', id, mask]); return wire(sources); },
    kinematicSelect: async (id, req) => {
      calls.push(['grow', req.seed_x, req.seed_y, req.seed_z]);
      const mask = new Float32Array(2); mask[0] = 1; mask[1] = 1;      // two voxels from (6, 2, 1)
      return { mask, dims: [2, 1, 1], origin: [6, 2, 1] };
    },
    maskLayerSave: async (body) => { calls.push(['save', body.length]); return { valid: true, workspace_filename: 'm_vr_mask.fits', output_path: '/w/m_vr_mask.fits', edited: [5], removed: [], n_sources: 2, n_voxels: 15 }; },
  };
  const drawn = [];
  const reports = [];
  let seed = 'add';
  const state = { info: { factors: [1, 1, 1], reduced_shape: shape, full_shape: shape }, open: { dataset_id: 'c1' }, clip: { min: [0, 0, 0], max: [1, 1, 1] },
    extent: [1, 0.8, 0.6], openPath: '/d/cube.fits' };
  const m = createMasks({ renderer: { setMaskLayer: (e) => drawn.push(e), createPanel: () => ({}), updatePanel() {} }, state, backend: () => backend,
    seedMode: () => seed, setSeedMode: (x) => { seed = x; }, onVisit() {}, onChange() {}, report: (r, d) => { reports.push([r, d]); return true; }, buzz() {} });
  await m.load({ maskPath: '/d/m_mask.fits' });
  assert.equal(calls[0][2].maskPath, '/d/m_mask.fits');
  assert.equal(m.state.layer.ids().join(), '2,5');
  assert.ok(drawn.at(-1) && drawn.at(-1).data.length > 0);
  assert.equal(m.modeCode(), ML.MODE_CODE.outline);
  // Pick: the next ray picks source 5; the lasso then edits it.
  m.press('pick');
  assert.ok(m.pickRay({ o: [(4 + 0.5) / 10, (2.5 + 0.5) / 8, -0.5], d: [0, 0, 1] }));
  assert.equal(m.state.selected, 5);
  assert.ok(m.refining());
  await m.lasso({ seed: [6, 2, 1] }, 'add', {}, undefined, () => false);
  assert.equal(m.state.layer.get(5).count, 14);
  assert.deepEqual(calls.at(-1), ['grow', 6, 2, 1]);
  assert.ok(m.undo());
  assert.equal(m.state.layer.get(5).count, 12);
  await m.lasso({ seed: [6, 2, 1] }, 'add', {}, undefined, () => false);
  const r = await m.save();
  assert.equal(r.workspace_filename, 'm_vr_mask.fits');
  const [recipe, detail] = reports[0];
  assert.equal(detail.product, 'mask');
  assert.equal(detail.file, 'm_vr_mask.fits');
  assert.equal(recipe.product.kind, 'mask');
  assert.deepEqual(recipe.product.replay.map((x) => x.route), ['/v1/cube/mask_layer_bin', '/v1/cube/kinematic_select_bin', '/v1/cube/mask_layer_save']);
  // Mask only and Residuals hide the backend's refined frames; Off draws nothing.
  m.press('mode:residual');
  assert.ok(m.hidesRefined());
  m.press('mode:off');
  assert.equal(drawn.at(-1), null);
  assert.equal(m.labels().length, 0);
  m.press('mode:outline');
  assert.deepEqual(m.labels().map((l) => l.text).sort(), ['2', '5']);
  // Another cube: the layer goes.
  state.open = { dataset_id: 'c2' };
  m.onCube();
  assert.equal(m.state.layer, null);
});
