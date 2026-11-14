// The page's side of the headset hub (issue #237).
// node --test backend/tests/vr/headset.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import * as H from '../../app/static/vr/js/headset.js';
import { parseProduct, openIn, parseLook, parseClip, aboutCube } from './desktop_hub_parser.mjs';

// ── Pure parts ─────────────────────────────────────────────────────────────

test('transport and label: adb on loopback, lan elsewhere, pair in the loader', () => {
  assert.equal(H.transportOf({ hostname: 'localhost' }, false), 'adb');
  assert.equal(H.transportOf({ hostname: '127.0.0.1' }, false), 'adb');
  assert.equal(H.transportOf({ hostname: '[::1]' }, false), 'adb');
  assert.equal(H.transportOf({ hostname: '192.168.1.20' }, false), 'lan');
  assert.equal(H.transportOf({ hostname: 'hpc.example.org' }, false), 'lan');
  assert.equal(H.transportOf({ hostname: 'localhost' }, true), 'pair');
  assert.equal(H.headsetLabel('Mozilla/5.0 (X11; Linux x86_64; Quest 3) OculusBrowser/40.0'), 'Quest 3');
  assert.equal(H.headsetLabel('Mozilla/5.0 (Macintosh) Chrome/132'), 'Desktop browser');
});

test('client id: kept per tab, matches the hub\'s pattern', () => {
  const kept = new Map();
  const storage = { getItem: (k) => kept.get(k) ?? null, setItem: (k, v) => kept.set(k, v) };
  const a = H.hubClientId(storage);
  assert.match(a, /^[A-Za-z0-9_.:-]{1,64}$/);
  assert.equal(H.hubClientId(storage), a, 'the same after a reload of the tab');
  kept.set(H.CLIENT_KEY, 'bad id with spaces');
  assert.notEqual(H.hubClientId(storage), 'bad id with spaces');
  assert.match(H.hubClientId(null), /^vr-[a-z0-9]{12}$/);
});

test('cube record: none for the synthetic cube (no dataset, no file)', () => {
  assert.equal(H.cubeRecord('', '/x.fits'), null);
  assert.equal(H.cubeRecord('ds1', ''), null);
  assert.deepEqual(H.cubeRecord('ds1', '/data/a.fits'), { dataset_id: 'ds1', path: '/data/a.fits', kind: 'cube' });
  assert.deepEqual(H.cubeRecord('ds1', '/data/v.fits', 'velocity'), { dataset_id: 'ds1', path: '/data/v.fits', kind: 'velocity' });
});

test('clip box: voxels of the full cube both ends included, the whole cube is null, round trip with the desktop\'s crop', () => {
  const full = [100, 80, 40];
  assert.equal(H.voxelBox({ min: [0, 0, 0], max: [1, 1, 1] }, full), null);
  assert.deepEqual(H.voxelBox({ min: [0.1, 0, 0.25], max: [0.5, 1, 0.75] }, full), [[10, 49], [0, 79], [10, 29]]);
  // The desktop parses it as a clip, and crops voxel i over i/n .. (i+1)/n (cropCommandBox).
  const c = parseClip(H.clipPayload([[10, 49], [0, 79], [10, 29]], 'ds'));
  assert.equal(c.set, true);
  assert.deepEqual(c.min, [10, 0, 10]);
  assert.deepEqual(c.max, [49, 79, 29]);
  assert.equal(parseClip(H.clipPayload(null, 'ds')).set, false);
  // Box -> page clip -> box: the same voxels, on any box.
  let seed = 7;
  const rnd = (n) => { seed = (seed * 16807) % 2147483647; return seed % n; };
  for (let k = 0; k < 200; k++) {
    const box = full.map((n) => { const a = rnd(n), b = rnd(n); return [Math.min(a, b), Math.max(a, b)]; });
    const clip = H.clipOfBox(box, full);
    const back = H.voxelBox(clip, full);
    const whole = box.every((r, i) => r[0] === 0 && r[1] === full[i] - 1);
    assert.deepEqual(back, whole ? null : box);
  }
  assert.equal(H.clipOfBox([[0, 100], [0, 1], [0, 1]], full), null, 'outside the cube');
  assert.equal(H.clipOfBox([[5, 4], [0, 1], [0, 1]], full), null, 'hi below lo');
});

test('look payload: the desktop reads every field (colormap, data-value threshold and colour max, opacity, gamma, mode)', () => {
  const p = H.lookPayload({ colormap: 'inferno', threshold: 0.0123, colourMax: 0.5, opacity: 0.4, gamma: 0.5, mode: 1,
    zStretch: 2, datasetId: 'ds1' });
  const l = parseLook(p);
  assert.deepEqual(l, { colormap: 'inferno', threshold: 0.0123, colourMax: 0.5, opacity: 0.4, gamma: 0.5, mode: '1', stretch: '' });
  assert.equal(p.z_stretch, 2);
  assert.equal(p.dataset_id, 'ds1');
  // A value that is not finite is left out, never sent as null or NaN (the hub refuses NaN).
  const q = H.lookPayload({ colormap: 'grey', threshold: NaN, colourMax: Infinity });
  assert.ok(!('threshold' in q) && !('colour_max' in q));
  assert.doesNotThrow(() => JSON.parse(JSON.stringify(q)));
});

test('save plans: the desktop\'s routes and bodies, files named after the cube', () => {
  const moment = H.savePlan({ product: 'moment', dataset_id: 'iso1', request: { moment_order: 1, channel_start: 0, channel_end: 39,
    mask_enabled: false, threshold_value: 0, threshold_auto: false } }, 'ngc3137');
  assert.equal(moment.route, '/v1/products/moment_fits');
  assert.deepEqual(moment.body, { moment_order: 1, channel_start: 0, channel_end: 39, mask_enabled: false, threshold_value: 0,
    threshold_auto: false, dataset_id: 'iso1', output_basename: 'ngc3137_vr_m1.fits' });
  const pv = H.savePlan({ product: 'pv', dataset_id: 'ds', request: { vertices: [[1, 2], [30, 40]], width_pixels: 3 } }, 'm33');
  assert.deepEqual(pv, { route: '/v1/products/pv_fits', body: { dataset_id: 'ds', vertices: [[1, 2], [30, 40]], width_pixels: 3, output_basename: 'm33_vr_pv.fits' } });
  const sp = H.savePlan({ product: 'spectrum', dataset_id: 'ds', request: { x: 12, y: 7 } }, 'm33');
  assert.deepEqual(sp, { route: '/v1/products/spectrum_table', body: { dataset_id: 'ds', x: 12, y: 7, radius: 0, format: 'csv', output_basename: '' } });
  assert.equal(H.savePlan({ product: 'isolate', file: 'a_isolated.fits' }), null, 'mask_apply_bin wrote it already');
  assert.equal(H.savePlan({ product: 'catalogue', path: '/data/c.csv' }), null);
  assert.equal(H.stemOf('/data/NGC 3137.fits.gz'), 'NGC_3137');
  assert.equal(H.stemOf('/data/run.bin.head'), 'run');
  assert.equal(H.stemOf(''), 'cube');
});

test('every product payload opens on the desktop (VrHeadsetHub.h parseProduct and openIn)', () => {
  const recipe = { schema: 'visivo-recipe/1', product: { id: 'vr-moment-3', kind: 'moment', label: 'Moment 0',
    replay: [{ route: '/v1/products/moment', request: { moment_order: 0 } }] }, parameters: { Order: 'Moment 0' } };
  const cases = [
    [{ product: 'moment', dataset_id: 'iso', request: { moment_order: 0 } }, 'm_vr_m0.fits', 'ImageViewer'],
    [{ product: 'pv', dataset_id: 'ds', request: { vertices: [[0, 0], [5, 5]], width_pixels: 1 } }, 'm_vr_pv.fits', 'PvViewer'],
    [{ product: 'spectrum', dataset_id: 'ds', request: { x: 1, y: 2 } }, 'm_spectrum_x1_y2.csv', 'SpectrumPlot'],
    [{ product: 'isolate', dataset_id: 'iso', file: 'm_isolated.fits' }, 'm_isolated.fits', 'CubeWindow'],
    [{ product: 'remove', dataset_id: 'rem', file: 'm_removed.fits' }, 'm_removed.fits', 'CubeWindow'],
    [{ product: 'catalogue', dataset_id: 'cat', path: '/data/sources.csv', request: { path: '/data/sources.csv' } }, '', 'File'],
    [{ product: 'flow', dataset_id: 'fl', path: '/data/vfield.fits', request: { path: '/data/vfield.fits' } }, '', 'File'],
  ];
  for (const [detail, file, where] of cases) {
    const payload = H.productPayload(detail, file || detail.file, recipe);
    assert.ok(H.PRODUCT_KINDS.includes(payload.product));
    assert.ok(!('output_path' in payload), 'the backend sets output_path, never the page');
    // What the hub relays: the file it found under the exports gets its output_path.
    const relayed = payload.file ? { ...payload, output_path: `/ws/exports/${payload.file}` } : payload;
    const p = parseProduct({ summary: 's', payload: relayed });
    assert.equal(p.kind, detail.product);
    assert.equal(openIn(p), where, JSON.stringify(detail));
    assert.equal(p.recipeId, 'vr-moment-3');
    assert.equal(p.replay.length, 1);
    if (file) assert.equal(p.workspaceFilename, file);
  }
  assert.match(H.productSummary({ product: 'moment', request: { moment_order: 2 } }, 'a.fits', 'a_vr_m2.fits'), /^Moment 2 of the selection of a\.fits → a_vr_m2\.fits$/);
  assert.ok(H.productSummary({ product: 'pv', request: { vertices: Array(500).fill([1, 2]) } }, 'x'.repeat(400), 'f').length <= 300);
});

test('a payload over the hub\'s limit loses the recipe\'s parameters, then the recipe, never the file', () => {
  const big = { product: 'moment', file: 'f.fits', request: { a: 1 }, recipe: { product: { id: 'x' }, parameters: { p: 'y'.repeat(20000) } } };
  const r1 = H.fitPayload(big);
  assert.equal(r1.trimmed, 'recipe parameters');
  assert.deepEqual(r1.payload.recipe.parameters, {});
  assert.equal(r1.payload.file, 'f.fits');
  const huge = { ...big, recipe: { product: { id: 'x', replay: [{ route: 'r', body: 'z'.repeat(20000) }] }, parameters: {} } };
  const r2 = H.fitPayload(huge);
  assert.equal(r2.trimmed, 'recipe');
  assert.ok(!('recipe' in r2.payload));
  assert.equal(H.fitPayload({ product: 'pv', request: { v: 'q'.repeat(20000) } }).payload, null);
  assert.equal(H.fitPayload({ product: 'pv' }).trimmed, '');
});

test('commands: only a show with a dataset and a path is acted on', () => {
  assert.equal(H.commandUsable({ seq: 3, type: 'show', dataset_id: 'd', path: '/p.fits' }), true);
  assert.equal(H.commandUsable({ seq: 3, type: 'show', dataset_id: 'd' }), false);
  assert.equal(H.commandUsable({ seq: 0, type: 'show', dataset_id: 'd', path: '/p' }), false);
  assert.equal(H.commandUsable({ seq: 4, type: 'close', dataset_id: 'd', path: '/p' }), false);
  assert.equal(H.commandsQuery('vr-1', 7).toString(), 'client=vr-1&after=7&wait=20');
});

// ── The controller, against a fake hub ─────────────────────────────────────

/** Timers under the test's control. */
function clock() {
  let now = 0, n = 0;
  const timers = new Map();
  return {
    setTimeout: (f, ms) => { const id = ++n; timers.set(id, { at: now + ms, f }); return id; },
    clearTimeout: (id) => { timers.delete(id); },
    async advance(ms) {
      const end = now + ms;
      for (;;) {
        await flush();
        const due = [...timers.entries()].filter(([, t]) => t.at <= end).sort((a, b) => a[1].at - b[1].at)[0];
        if (!due) break;
        timers.delete(due[0]);
        now = due[1].at;
        due[1].f();
      }
      now = end;
      await flush();
    },
    pending: () => timers.size,
  };
}
async function flush() { for (let i = 0; i < 20; i++) await new Promise((r) => setImmediate(r)); }

/** A fake hub: records every request; `commands` are handed to the poll once. */
function fakeHub({ instance = 'inst-1' } = {}) {
  const calls = [];
  const s = { instance, commands: [], files: 0, unsupported: false, forget: false, saveStatus: [], polls: [] };
  const resp = (status, body) => ({ status, text: async () => (typeof body === 'string' ? body : JSON.stringify(body)) });
  async function fetch(url, init = {}) {
    const u = new URL(url, 'http://h');
    const body = init.body ? JSON.parse(init.body) : null;
    calls.push({ method: init.method || 'GET', path: u.pathname, query: Object.fromEntries(u.searchParams), body, headers: init.headers, keepalive: !!init.keepalive });
    if (s.unsupported) return resp(404, { detail: 'Not Found' });
    if (u.pathname === '/v1/vr/headset/hello') { s.forget = false; return resp(200, { client: body.client, server_time: 1, instance: s.instance }); }
    if (s.forget && u.pathname.startsWith('/v1/vr/')) return resp(404, { detail: 'Unknown headset client in this session: say hello first' });
    if (u.pathname === '/v1/vr/headset/state') return resp(200, { client: body.client, server_time: 1, instance: s.instance });
    if (u.pathname === '/v1/vr/activity') return resp(200, { seq: calls.length });
    if (u.pathname === '/v1/vr/headset/commands') {
      const after = Number(u.searchParams.get('after'));
      s.polls.push(after);
      const cmds = s.commands.filter((c) => c.seq > after);
      if (!cmds.length) {
        // A long poll: answers when aborted (the test does not wait 20 s).
        await new Promise((res) => { if (init.signal) init.signal.addEventListener('abort', res); s.release = res; });
        if (init.signal && init.signal.aborted) throw Object.assign(new Error('aborted'), { name: 'AbortError' });
        return resp(200, { commands: [], next: after, instance: s.instance });
      }
      return resp(200, { commands: cmds, next: Math.max(...cmds.map((c) => c.seq)), instance: s.instance });
    }
    if (u.pathname.startsWith('/v1/products/')) {
      const st = s.saveStatus.length ? s.saveStatus.shift() : 200;
      if (st !== 200) return resp(st, { detail: 'busy' });
      s.files++;
      const name = body.output_basename || `cube_spectrum_x${body.x}_y${body.y}.csv`;
      return resp(200, { valid: true, error: '', output_path: `/ws/exports/${name}`, workspace_filename: name });
    }
    if (init.method === 'DELETE') return resp(200, { removed: true });
    return resp(404, { detail: 'Not Found' });
  }
  return { calls, s, fetch, posts: (path) => calls.filter((c) => c.path === path && c.method === 'POST') };
}

function makeHub(t, fh, extra = {}) {
  const logs = [];
  const commands = [];
  const hub = H.createHub({
    client: 'vr-test', transport: 'adb', label: 'Quest 3',
    auth: () => ({ token: 'tok', session: extra.session ? extra.session() : 'sess-1' }),
    onCommand: (c) => { commands.push(c); },
    log: (level, event, message) => logs.push([level, event, message]),
    fetch: fh.fetch, setTimeout: t.setTimeout, clearTimeout: t.clearTimeout,
  });
  return { hub, logs, commands };
}

test('hello once, then the state when it changes, in one post per burst', async () => {
  const t = clock(), fh = fakeHub();
  const { hub } = makeHub(t, fh);
  hub.start();
  await t.advance(10);
  assert.equal(fh.posts('/v1/vr/headset/hello').length, 1);
  assert.deepEqual(fh.posts('/v1/vr/headset/hello')[0].body, { client: 'vr-test', transport: 'adb', label: 'Quest 3' });
  const h = fh.calls[0].headers;
  assert.equal(h['X-Visivo-Session'], 'sess-1');
  assert.equal(h['X-Visivo-Token'], 'tok');
  const cube = H.cubeRecord('ds1', '/data/a.fits');
  for (let i = 0; i < 5; i++) hub.noteState({ in_vr: i > 2, cube, staged_cube: null });
  await t.advance(H.STATE_DELAY_MS + 10);
  const states = fh.posts('/v1/vr/headset/state');
  assert.equal(states.length, 1);
  assert.deepEqual(states[0].body, { client: 'vr-test', in_vr: true, cube, staged_cube: null });
  hub.noteState({ in_vr: true, cube, staged_cube: null });
  await t.advance(500);
  assert.equal(fh.posts('/v1/vr/headset/state').length, 1, 'the same state is not sent again');
  // No token in any body (the hub refuses one).
  assert.ok(fh.calls.every((c) => !JSON.stringify(c.body || {}).includes('tok')));
  hub.stop();
});

test('look and clip: one event 500 ms after the last change, the latest wins, nothing for the baseline', async () => {
  const t = clock(), fh = fakeHub();
  const { hub } = makeHub(t, fh);
  hub.start();
  await t.advance(10);
  const look = (thr) => H.lookPayload({ colormap: 'viridis', threshold: thr, colourMax: 1, opacity: 0.4, gamma: 0.5, mode: 0, datasetId: 'ds1' });
  hub.rebase('look', '/data/a.fits', look(0.1));
  hub.look('/data/a.fits', look(0.1));            // the baseline: nothing
  await t.advance(1000);
  assert.equal(fh.posts('/v1/vr/activity').length, 0);
  // A slider dragged: many values within 500 ms -> one event, the last value.
  for (let i = 1; i <= 8; i++) { hub.look('/data/a.fits', look(0.1 + i * 0.01)); await t.advance(100); }
  assert.equal(fh.posts('/v1/vr/activity').length, 0, 'not while it still moves');
  // Called again with the same value by the page's timer: the wait is not restarted.
  for (let i = 0; i < 4; i++) { hub.look('/data/a.fits', look(0.18)); await t.advance(100); }
  await t.advance(200);
  const ev = fh.posts('/v1/vr/activity');
  assert.equal(ev.length, 1);
  assert.equal(ev[0].body.kind, 'look');
  assert.equal(ev[0].body.cube_path, '/data/a.fits');
  assert.equal(parseLook(ev[0].body.payload).threshold, 0.18);
  assert.ok(aboutCube(ev[0].body, '/data/a.fits', 'ds1'));
  // A clip.
  hub.clip('/data/a.fits', H.clipPayload([[1, 5], [0, 9], [2, 3]], 'ds1'));
  await t.advance(H.DEBOUNCE_MS + 10);
  const clips = fh.posts('/v1/vr/activity').filter((c) => c.body.kind === 'clip');
  assert.equal(clips.length, 1);
  assert.deepEqual(parseClip(clips[0].body.payload), { set: true, min: [1, 0, 2], max: [5, 9, 3] });
  hub.stop();
});

test('a cube switch sends what was pending for the old cube first, then the cube', async () => {
  const t = clock(), fh = fakeHub();
  const { hub } = makeHub(t, fh);
  hub.start();
  await t.advance(10);
  hub.look('/data/a.fits', { colormap: 'inferno', dataset_id: 'a' });
  await t.advance(100);
  hub.cube('/data/b.fits', { path: '/data/b.fits', dataset_id: 'b', kind: 'cube', binning: [2, 2, 1] });
  await t.advance(10);
  const ev = fh.posts('/v1/vr/activity').map((c) => [c.body.kind, c.body.cube_path]);
  assert.deepEqual(ev, [['look', '/data/a.fits'], ['cube', '/data/b.fits']]);
  assert.deepEqual(fh.posts('/v1/vr/activity')[1].body.payload, { path: '/data/b.fits', dataset_id: 'b', kind: 'cube', binning: [2, 2, 1] });
  hub.stop();
});

test('products: saved with the desktop\'s route, then one event naming the file; a PV path settles first; Isolate needs no save', async () => {
  const t = clock(), fh = fakeHub();
  const { hub } = makeHub(t, fh);
  hub.start();
  await t.advance(10);
  const where = { cubePath: '/data/m33.fits', cubeName: 'm33.fits', stem: 'm33' };
  const rec = (id, kind) => ({ schema: 'visivo-recipe/1', product: { id, kind, label: kind }, parameters: {} });
  hub.product({ product: 'spectrum', dataset_id: 'ds', request: { x: 3, y: 4 } }, rec('vr-spectrum-1', 'spectrum'), where, 'k1');
  await t.advance(10);
  // A PV path drawn point by point: re-recorded under one id; saved once, after it settles.
  for (let n = 2; n <= 5; n++) {
    hub.product({ product: 'pv', dataset_id: 'ds', request: { vertices: Array.from({ length: n }, (_, i) => [i, i]), width_pixels: 1 } },
      rec('vr-pv-2', 'pv'), where, 'pv-key');
    await t.advance(400);
  }
  assert.equal(fh.posts('/v1/products/pv_fits').length, 0, 'not while the path grows');
  await t.advance(H.PV_SETTLE_MS);
  hub.product({ product: 'isolate', dataset_id: 'iso', file: 'm33_isolated.fits' }, rec('vr-lasso-3', 'lasso'), where, 'k3');
  await t.advance(10);
  assert.equal(fh.posts('/v1/products/spectrum_table').length, 1);
  assert.equal(fh.posts('/v1/products/pv_fits').length, 1);
  assert.equal(fh.posts('/v1/products/pv_fits')[0].body.vertices.length, 5, 'the whole path');
  const ev = fh.posts('/v1/vr/activity').map((c) => c.body);
  assert.deepEqual(ev.map((e) => e.payload.product), ['spectrum', 'pv', 'isolate']);
  assert.deepEqual(ev.map((e) => e.payload.file), ['cube_spectrum_x3_y4.csv', 'm33_vr_pv.fits', 'm33_isolated.fits']);
  for (const e of ev) {
    assert.equal(e.kind, 'product');
    assert.equal(e.cube_path, '/data/m33.fits');
    assert.ok(e.summary.length > 0 && e.summary.length <= 300);
    assert.notEqual(openIn(parseProduct({ summary: e.summary, payload: { ...e.payload, output_path: `/x/${e.payload.file}` } })), 'Nothing');
  }
  hub.stop();
});

test('a save that meets busy heavy slots is tried again; one that fails goes to the diagnostics only', async () => {
  const t = clock(), fh = fakeHub();
  const { hub, logs } = makeHub(t, fh);
  hub.start();
  await t.advance(10);
  fh.s.saveStatus = [429, 200];
  hub.product({ product: 'moment', dataset_id: 'iso', request: { moment_order: 0 } }, null, { cubePath: '/c.fits', stem: 'c' }, 'm');
  await t.advance(H.SAVE_RETRY_MS[0] + 50);
  assert.equal(fh.posts('/v1/products/moment_fits').length, 2);
  assert.equal(fh.posts('/v1/vr/activity').length, 1);
  fh.s.saveStatus = [500];
  hub.product({ product: 'moment', dataset_id: 'iso', request: { moment_order: 1 } }, null, { cubePath: '/c.fits', stem: 'c' }, 'm1');
  await t.advance(50);
  assert.equal(fh.posts('/v1/vr/activity').length, 1, 'no event for a product not saved');
  assert.ok(logs.some(([lvl, ev, msg]) => lvl === 'warning' && ev === 'hub-failed' && /moment not saved/.test(msg)));
  assert.equal(hub.st.saveFailures, 1);
  hub.stop();
});

test('commands: a show is applied once, acknowledged by the next poll; a backend restart resets the cursor', async () => {
  const t = clock(), fh = fakeHub();
  const { hub, commands } = makeHub(t, fh);
  fh.s.commands = [{ seq: 41, type: 'show', dataset_id: 'd2', path: '/data/b.fits', kind: 'cube' }];
  hub.start();
  await t.advance(10);
  assert.equal(commands.length, 1);
  assert.equal(commands[0].path, '/data/b.fits');
  assert.deepEqual(fh.s.polls.slice(0, 2), [0, 41], 'the next poll acknowledges it');
  // No keepalive on the long poll (share.js: it froze pages with a backend going down).
  assert.ok(fh.calls.filter((c) => c.path === '/v1/vr/headset/commands').every((c) => !c.keepalive));
  // The backend restarts: seqs begin again at 1, and the page counts from 0.
  fh.s.instance = 'inst-2';
  fh.s.commands = [{ seq: 1, type: 'show', dataset_id: 'd3', path: '/data/c.fits' }];
  fh.s.release();
  await t.advance(10);
  assert.deepEqual(commands.map((c) => c.path), ['/data/b.fits', '/data/c.fits']);
  assert.equal(hub.st.after, 1);
  assert.equal(hub.st.instance, 'inst-2');
  hub.stop();
});

test('a 404 for this page says hello again; a backend without a hub stops it for good', async () => {
  const t = clock(), fh = fakeHub();
  const { hub, logs } = makeHub(t, fh);
  hub.start();
  await t.advance(10);
  fh.s.forget = true;                      // the hub forgot the page (a restart of the backend)
  hub.look('/a.fits', { colormap: 'grey' });
  await t.advance(H.DEBOUNCE_MS + 10);
  assert.equal(fh.posts('/v1/vr/headset/hello').length, 2);
  assert.equal(fh.posts('/v1/vr/activity').length, 2, 'refused once, sent after the new hello');
  hub.stop();
  const t2 = clock(), fh2 = fakeHub();
  fh2.s.unsupported = true;
  const two = makeHub(t2, fh2);
  two.hub.start();
  await t2.advance(5000);
  assert.equal(two.hub.st.unsupported, true);
  assert.equal(two.hub.st.running, false);
  assert.equal(fh2.calls.length, 1, 'one hello, nothing more');
  assert.ok(two.logs.some(([, ev]) => ev === 'hub-unsupported'));
  assert.ok(!logs.some(([lvl]) => lvl === 'error'), 'never an error for the user');
});

test('stop (the page stood down): goodbye to the hub, nothing more sent, a new hello on start', async () => {
  const t = clock(), fh = fakeHub();
  const { hub } = makeHub(t, fh);
  hub.start();
  await t.advance(10);
  hub.stop();
  await t.advance(10);
  const del = fh.calls.filter((c) => c.method === 'DELETE');
  assert.equal(del.length, 1);
  assert.equal(del[0].query.client, 'vr-test');
  assert.equal(del[0].keepalive, false);
  hub.look('/a.fits', { colormap: 'grey' });
  hub.product({ product: 'isolate', file: 'x.fits' }, null, { cubePath: '/a.fits' }, 'z');
  await t.advance(2000);
  assert.equal(fh.posts('/v1/vr/activity').length, 0, 'a page that stood down tells the hub nothing');
  hub.stop({ unloading: true });
  hub.start();
  await t.advance(10);
  assert.equal(fh.posts('/v1/vr/headset/hello').length, 2);
  hub.stop({ unloading: true });
  await t.advance(10);
  assert.equal(fh.calls.filter((c) => c.method === 'DELETE').at(-1).keepalive, true, 'a closing page: the goodbye may outlive it');
});

test('a new session (a load made one): the poll moves there with a hello, the old session forgets the page', async () => {
  const t = clock(), fh = fakeHub();
  let session = 'sess-1';
  const { hub } = makeHub(t, fh, { session: () => session });
  hub.start();
  await t.advance(10);
  session = 'sess-2';
  hub.resync();
  await t.advance(10);
  const hellos = fh.posts('/v1/vr/headset/hello');
  assert.deepEqual(hellos.map((c) => c.headers['X-Visivo-Session']), ['sess-1', 'sess-2']);
  const del = fh.calls.find((c) => c.method === 'DELETE');
  assert.equal(del.headers['X-Visivo-Session'], 'sess-1');
  hub.stop();
});

test('the page wires the hub: requests through pageFetch, no keepalive on the poll, sends from timers', () => {
  const src = readFileSync(new URL('../../app/static/vr/js/headset.js', import.meta.url), 'utf8');
  assert.match(src, /import \{ pageFetch \} from '\.\/transport\.js'/);
  assert.ok(!/keepalive: true/.test(src), 'never a literal keepalive');
  const main = readFileSync(new URL('../../app/static/vr/js/main.js', import.meta.url), 'utf8');
  assert.match(main, /setInterval\(hubTick, 250\)/);
  assert.match(main, /onRecord: \(r, detail\) => reportProduct\(r, detail\)/);
  assert.match(main, /onCommand: \(cmd\) => showFromDesktop\(cmd\)/);
  // The frame loop never calls the hub.
  const frame = main.slice(main.indexOf('function onXRFrame('), main.indexOf('function setEyeBufferScale('));
  assert.ok(frame.length > 1000 && !/hub\./.test(frame));
});

test('a product waiting when the page stands down is dropped; a restart says hello only after the goodbye', async () => {
  const t = clock(), fh = fakeHub();
  let session = 'sess-1';
  const { hub } = makeHub(t, fh, { session: () => session });
  hub.start();
  await t.advance(10);
  // A PV path still settling, then the page stands down: not saved, not sent.
  hub.product({ product: 'pv', dataset_id: 'ds', request: { vertices: [[0, 0], [5, 5]], width_pixels: 1 } }, null, { cubePath: '/a.fits' }, 'pv');
  hub.stop();
  await t.advance(H.PV_SETTLE_MS + 100);
  assert.equal(fh.posts('/v1/products/pv_fits').length, 0);
  // A product made in one session is not saved in another.
  hub.start();
  await t.advance(10);
  hub.product({ product: 'pv', dataset_id: 'ds', request: { vertices: [[0, 0], [5, 5]], width_pixels: 1 } }, null, { cubePath: '/a.fits' }, 'pv2');
  session = 'sess-2';
  await t.advance(H.PV_SETTLE_MS + 100);
  assert.equal(fh.posts('/v1/products/pv_fits').length, 0, 'made in sess-1, the page is in sess-2 now');
  // The goodbye goes before the next hello of the same page.
  hub.stop();
  hub.start();
  await t.advance(50);
  const order = fh.calls.filter((c) => c.method === 'DELETE' || c.path === '/v1/vr/headset/hello').map((c) => c.method);
  assert.deepEqual(order.slice(-2), ['DELETE', 'POST']);
  hub.stop();
});

test('stop, start, stop while the goodbye is on its way: the stopped page is not registered again', async () => {
  let release = null;
  const calls = [];
  const fetch = async (url, init = {}) => {
    const path = new URL(url, 'http://h').pathname;
    calls.push([init.method || 'GET', path]);
    if (init.method === 'DELETE') return new Promise((res) => { release = () => res({ status: 200, text: async () => '{"removed":true}' }); });
    if (path.endsWith('/hello')) return { status: 200, text: async () => '{"instance":"i"}' };
    return new Promise((_, rej) => init.signal && init.signal.addEventListener('abort', () => rej(new Error('aborted'))));
  };
  const hub = H.createHub({ client: 'p', transport: 'adb', auth: () => ({ token: 't', session: 's' }), fetch, onCommand: () => {} });
  hub.start(); await flush();
  hub.stop(); hub.start(); await flush(); hub.stop();
  const hellos = () => calls.filter((c) => c[1].endsWith('/hello')).length;
  const before = hellos();
  release(); await flush();
  assert.equal(hellos(), before, 'no hello from the stopped run');
  assert.equal(hub.st.helloSession, '');
  if (release) release();
});

test('a goodbye that is not confirmed in time: the next hello uses a new client id', async () => {
  const t = clock();
  const calls = [];
  const kept = [];
  const fetch = async (url, init = {}) => {
    const u = new URL(url, 'http://h');
    calls.push([init.method || 'GET', u.pathname, init.body ? JSON.parse(init.body).client : u.searchParams.get('client')]);
    if (init.method === 'DELETE') {
      // Never answers: aborted by the page's deadline (it may still reach the hub later).
      return new Promise((_, rej) => init.signal.addEventListener('abort', () => rej(new Error('aborted'))));
    }
    if (u.pathname.endsWith('/hello')) return { status: 200, text: async () => '{"instance":"i"}' };
    return new Promise((_, rej) => init.signal && init.signal.addEventListener('abort', () => rej(new Error('aborted'))));
  };
  const hub = H.createHub({ client: 'vr-old', transport: 'adb', auth: () => ({ token: 't', session: 's' }), fetch, onCommand: () => {},
    onClient: (id) => kept.push(id), setTimeout: t.setTimeout, clearTimeout: t.clearTimeout });
  hub.start(); await t.advance(10);
  hub.stop(); hub.start();
  await t.advance(H.REQUEST_TIMEOUT_MS + 100);
  const hellos = calls.filter((c) => c[1].endsWith('/hello')).map((c) => c[2]);
  assert.equal(hellos.length, 2);
  assert.equal(hellos[0], 'vr-old');
  assert.notEqual(hellos[1], 'vr-old', 'a late DELETE of vr-old cannot remove it');
  assert.deepEqual(kept, [hellos[1]], 'kept for the tab');
  hub.stop();
});

test('a stop while the first hello is on its way: its goodbye goes before the next run\'s hello', async () => {
  let n = 0;
  const order = [];
  const fetch = async (url, init = {}) => {
    const path = new URL(url, 'http://h').pathname;
    order.push(`${init.method || 'GET'} ${path.split('/').pop()}`);
    if (path.endsWith('/hello')) {
      if (++n === 1) return new Promise((_, rej) => init.signal.addEventListener('abort', () => rej(new Error('aborted'))));
      return { status: 200, text: async () => '{"instance":"i"}' };
    }
    if (init.method === 'DELETE') return { status: 200, text: async () => '{"removed":true}' };
    return new Promise((_, rej) => init.signal && init.signal.addEventListener('abort', () => rej(new Error('aborted'))));
  };
  const hub = H.createHub({ client: 'p', transport: 'adb', auth: () => ({ token: 't', session: 's' }), fetch, onCommand: () => {} });
  hub.start(); await flush();
  hub.stop(); hub.start(); await flush();
  assert.deepEqual(order.slice(0, 3), ['POST hello', 'DELETE headset', 'POST hello']);
  assert.equal(hub.st.helloSession, 's', 'the running page is registered');
  hub.stop();
});
