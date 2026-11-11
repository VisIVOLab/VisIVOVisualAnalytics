// Lasso refinement in the headset (issue #191): add and carve seeds, Isolate
// and Remove, as the desktop makes and records them (node --test).
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import * as TL from '../../app/static/vr/js/tools-lasso.js';
import * as Rc from '../../app/static/vr/js/recipe.js';
import { createSelection } from '../../app/static/vr/js/select-ui.js';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, '../../..');

test('seeds: a plain seed starts again, add and carve append to the selection shown', () => {
  let s = TL.nextSeeds(null, 'add', [1, 2, 3]);          // no selection: a new one, as the desktop's ⇧-click
  assert.deepEqual(s, { seed: [1, 2, 3], add: [], sub: [] });
  s = TL.nextSeeds(s, 'add', [4, 5, 6]);
  s = TL.nextSeeds(s, 'carve', [7, 8, 9]);
  s = TL.nextSeeds(s, 'add', [10, 11, 12]);
  assert.deepEqual(s, { seed: [1, 2, 3], add: [[4, 5, 6], [10, 11, 12]], sub: [[7, 8, 9]] });
  const before = JSON.stringify(s);
  TL.nextSeeds(s, 'add', [0, 0, 0]);
  assert.equal(JSON.stringify(s), before, 'the current seeds are not changed in place');
  assert.deepEqual(TL.nextSeeds(s, 'new', [5, 5, 5]), { seed: [5, 5, 5], add: [], sub: [] });
});

test('the grow request carries the seed lists as LassoReplay writes them', () => {
  const s = { seed: [83, 297, 465], add: [[74, 284, 442]], sub: [[81, 300, 471]] };
  const g = TL.growRequest(s, {});
  assert.deepEqual(Object.keys(g), ['seed_z', 'seed_y', 'seed_x', 'add_seeds', 'sub_seeds', 'reach', 'lambda_v',
    'sigma', 'nsigma', 'roi_radius', 'reach_max']);
  assert.deepEqual(g.add_seeds, [[74, 284, 442]]);
  assert.deepEqual(g.sub_seeds, [[81, 300, 471]]);
  const p = Rc.lassoDisplayParams(g);
  assert.equal(p['Extra seeds added'], '(74, 284, 442)');
  assert.equal(p['Extra seeds carved'], '(81, 300, 471)');
});

test('the apply step is the desktop\'s lassoApplyStep (no ROI recorded)', () => {
  const st = TL.applyStep('blank_outside');
  assert.equal(st.route, '/v1/cube/mask_apply_bin');
  assert.deepEqual(st.request, { mode: 'blank_outside' });
  // src/app/LassoReplay.cpp, lassoApplyStep(mode, handEdited = false).
  assert.equal(st.body, 'the previous call\'s response body, the selection as a little-endian float32 mask, '
    + 'posted back with origin_x, origin_y, origin_z from its X-Visivo-Origin header and width, height, depth '
    + 'from X-Visivo-Width, X-Visivo-Height, X-Visivo-Depth');
  assert.equal(TL.APPLY_MODES.isolate, 'blank_outside');
  assert.equal(TL.APPLY_MODES.remove, 'blank_inside');
});

test('the HISTORY recipe is lassoHistoryItems joined by "; "', () => {
  const g = Rc.lassoGrowRequest([69, 83, 80], { sigma: 0.0012345678901234567 }, [[1, 2, 3], [4, 5, 6]], []);
  assert.equal(TL.historyRecipe(g),
    'seed_zyx=69,83,80; reach=8; lambda_v=1.00; nsigma=2.5; sigma=0.0012345678901234567 (noise estimate); '
    + 'smooth_zyx=0,0,0; reach_max=64; add_seeds=(1,2,3)|(4,5,6); sub_seeds=none; hand_edited=no');
  const r = TL.historyRecipe(Rc.lassoGrowRequest([1, 2, 3], { smooth_v: 2, smooth_xy: 6 }), 12);
  assert.ok(r.includes('sigma=robust (whole cube)'));
  assert.ok(r.includes('smooth_zyx=2,6,6'));
  assert.ok(r.endsWith('; domain_trimmed=largest box half-width 12 voxels'));
});

test('the apply query names the route\'s parameters', () => {
  const src = readFileSync(path.join(REPO, 'backend/app/routers/cube.py'), 'utf8');
  const start = src.indexOf('async def cube_mask_apply_bin(');
  const sig = src.slice(start, src.indexOf(') -> CubeMaskResponse:', start));
  const names = new Set([...sig.matchAll(/(\w+): (?:str|int) = Query/g)].map((m) => m[1]));
  const q = TL.applyQuery('ds_1', [1, 2, 3], [4, 5, 6], 'blank_inside', 'x');
  for (const k of Object.keys(q)) assert.ok(names.has(k), k);
  for (const k of ['dataset_id', 'origin_x', 'origin_y', 'origin_z', 'width', 'height', 'depth', 'mode']) assert.ok(k in q, k);
});

test('the new cube\'s Provenance is the desktop\'s applyLassoMask labels', () => {
  const g = Rc.lassoGrowRequest([1, 2, 3]);
  const p = TL.applyParams(g, 'blank_inside', 13216, 0, 'cube_lasso-inside.fits');
  assert.equal(p.Mode, 'remove (inside blanked)');
  assert.equal(p['Selected voxels'], '13216');
  assert.equal(p.File, 'cube_lasso-inside.fits');
  assert.equal(p['Hand-edited'], 'no');
  assert.ok(!('Domain trimmed' in p));
  assert.equal(TL.applyParams(g, 'blank_outside', 1, 5, '').Mode, 'isolate (outside blanked)');
  assert.ok(TL.applyParams(g, 'blank_outside', 1, 5, '')['Domain trimmed'].includes('half-width 5'));
});

/** createSelection over a stub page and backend that records every grow request. */
function harness() {
  const grows = [];
  const posts = [];
  const state = {
    info: { full_shape: [8, 8, 8], factors: [1, 1, 1], reduced_shape: [8, 8, 8], bunit: 'Jy/beam',
      axes: { x: { name: 'RA', unit: 'deg', world: Array(8).fill(0) }, y: { name: 'DEC', unit: 'deg', world: Array(8).fill(0) },
        z: { name: 'VELO', unit: 'm/s', world: [], full_velocity_kms: null } } },
    cube: { data: new Uint8Array(512).fill(200), dims: [8, 8, 8], encoding: 'uint8' }, open: { dataset_id: 'ds_1' },
    clip: { min: [0, 0, 0], max: [1, 1, 1] }, extent: [1, 1, 1], texScale: [1, 1, 1], thrNorm: 0.1, mode: 0,
  };
  const backend = {
    session: 's',
    subvolume: async (ds, x0, x1, y0, y1, z0, z1) => ({ values: new Float32Array([1]), dims: [1, 1, 1], origin: [x0, y0, z0] }),
    pixelToSky: async () => ({ valid: false }),
    kinematicSelect: async (ds, req) => { grows.push(req); return { mask: new Float32Array([1]), bytes: new ArrayBuffer(4), dims: [1, 1, 1], origin: [4, 4, 4], count: grows.length, trimBox: 0, ms: 1 }; },
    maskStats: async () => ({ bbox: [4, 4, 4, 4, 4, 4], flux_jy_kms: 1 }),
    _binary: async (url, body) => {
      posts.push({ url, bytes: body.byteLength });
      const mode = new URLSearchParams(url.split('?')[1]).get('mode');
      return { buf: new TextEncoder().encode(JSON.stringify({ valid: true, new_dataset_id: `ds_${mode}`, workspace_filename: `c_${mode}.fits`,
        output_path: `/x/c_${mode}.fits`, width: 8, height: 8, depth: 8 })).buffer };
    },
  };
  const sel = createSelection({ state, renderer: { setMask: () => {}, createPanel: () => ({}), updatePanel: () => {} },
    backend: () => backend, path: () => '/data/cube.fits', onChange: () => {} });
  const tools = TL.createLassoTools({ selection: sel, backend: () => backend, datasetId: () => 'ds_1', onChange: () => {} });
  const ray = (x, y) => ({ o: [(x + 0.5) / 8, (y + 0.5) / 8, 1.5], d: [0, 0, -1] });
  return { sel, tools, grows, posts, ray };
}

test('in the page: add and carve grow with the seed lists, a new seed starts again', async () => {
  const h = harness();
  h.sel.setTool('lasso');
  await h.sel.act(h.ray(4, 4));
  h.tools.setSeedMode('add');
  await h.sel.act(h.ray(1, 1));
  h.tools.setSeedMode('carve');
  await h.sel.act(h.ray(6, 2));
  assert.equal(h.grows.length, 3);
  assert.ok(!('add_seeds' in h.grows[0]));
  assert.deepEqual(h.grows[1].add_seeds.length, 1);
  assert.deepEqual([h.grows[2].seed_z, h.grows[2].seed_y, h.grows[2].seed_x], [h.grows[0].seed_z, h.grows[0].seed_y, h.grows[0].seed_x]);
  assert.equal(h.grows[2].add_seeds.length, 1);
  assert.equal(h.grows[2].sub_seeds.length, 1);
  assert.equal(h.grows[2].sub_seeds[0][1], 2);       // y of the carve
  assert.equal(h.grows[2].sub_seeds[0][2], 6);       // x
  const last = h.sel.sel.recipes.at(-1);
  assert.deepEqual(last.product.replay[0].request, h.grows[2]);
  assert.ok(last.parameters['Extra seeds carved']);
  h.tools.setSeedMode('new');
  await h.sel.act(h.ray(3, 3));
  assert.ok(!('add_seeds' in h.grows[3]) && !('sub_seeds' in h.grows[3]));
});

test('Isolate posts the mask shown, records the desktop\'s two steps, and is reused for the moment', async () => {
  const h = harness();
  h.sel.setTool('lasso');
  await h.sel.act(h.ray(4, 4));
  await h.tools.run('isolate');
  assert.equal(h.posts.length, 1);
  assert.ok(h.posts[0].url.startsWith('/v1/cube/mask_apply_bin?'));
  const q = new URLSearchParams(h.posts[0].url.split('?')[1]);
  assert.equal(q.get('mode'), 'blank_outside');
  assert.deepEqual(['origin_x', 'origin_y', 'origin_z', 'width', 'height', 'depth'].map((k) => Number(q.get(k))), [4, 4, 4, 1, 1, 1]);
  assert.ok(q.get('recipe').startsWith('seed_zyx='));
  assert.equal(h.posts[0].bytes, 4);
  const r = h.sel.sel.recipes.at(-1);
  assert.equal(r.product.label, 'Lasso: isolated cube');
  assert.deepEqual(r.product.replay.map((s) => s.route), ['/v1/cube/kinematic_select_bin', '/v1/cube/mask_apply_bin']);
  assert.equal(r.parameters.Mode, 'isolate (outside blanked)');
  assert.equal(r.parameters.File, 'c_blank_outside.fits');
  // The same selection isolated again (the moment asks for it): no second cube.
  const again = await h.tools.apply('blank_outside');
  assert.equal(again.id, 'ds_blank_outside');
  assert.equal(h.posts.length, 1);
  await h.tools.run('remove');
  assert.equal(h.posts.length, 2);
  assert.equal(h.sel.sel.recipes.at(-1).product.label, 'Lasso: removed cube');
  assert.equal(h.tools.state.applied.mode, 'blank_inside');
  // A new grow is a new mask: Isolate makes a new cube.
  await h.sel.act(h.ray(2, 2));
  await h.tools.apply('blank_outside');
  assert.equal(h.posts.length, 3);
});

test('Isolate without a selection says so and posts nothing', async () => {
  const h = harness();
  await h.tools.run('isolate');
  assert.equal(h.posts.length, 0);
  assert.match(h.tools.state.error, /Grow a selection first/);
});
