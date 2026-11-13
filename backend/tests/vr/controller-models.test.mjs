// The controller models' loader: decode size, sharing, profile cache, retry, preload.
// node --test backend/tests/vr/controller-models.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { ControllerModels, RETRY_MS, decodeModelImages } from '../../app/static/vr/js/controller-models.js';

const MODELS = new URL('../../app/static/vr/models/', import.meta.url);
const flush = async () => { for (let i = 0; i < 50; i++) await new Promise((r) => setImmediate(r)); };

/** A fake createImageBitmap: records each call's size and counts closed bitmaps. */
function fakeBitmaps({ failOn = 0 } = {}) {
  const S = { calls: [], closed: 0, n: 0 };
  globalThis.createImageBitmap = async (src, opts = {}) => {
    S.n++;
    if (failOn && S.n === failOn) throw new Error('decode failed');
    S.calls.push({ w: opts.resizeWidth, h: opts.resizeHeight });
    return { width: opts.resizeWidth || 8, height: opts.resizeHeight || 8, close() { S.closed++; } };
  };
  return S;
}

/** A fetch over the vendored files, counting requests; `fail(url)` makes one fail. */
function fakeFetch(fail = () => false) {
  const F = { count: new Map() };
  F.fetch = async (u) => {
    const href = String(u);
    F.count.set(href, (F.count.get(href) || 0) + 1);
    if (fail(href)) return { ok: false, status: 503 };
    const bytes = readFileSync(new URL(href));
    return { ok: true, status: 200, json: async () => JSON.parse(bytes.toString('utf8')),
      arrayBuffer: async () => bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength) };
  };
  return F;
}

function fakeRenderer() {
  return { uploads: [], uploadModel(model, bitmaps, opts) { const h = { model, bitmaps, opts }; this.uploads.push(h); return h; } };
}

const src = (hand, profiles = ['meta-quest-touch-plus', 'generic-trigger-squeeze-thumbstick']) => ({ handedness: hand, profiles });

test('a Touch Plus controller: its texture decoded straight to 1024², its model uploaded once and shared', async () => {
  const B = fakeBitmaps();
  const F = fakeFetch();
  const R = fakeRenderer();
  const cm = new ControllerModels(R, { base: MODELS, fetchImpl: F.fetch });
  const a = src('right'), b = src('right'), l = src('left');
  assert.equal(cm.entry(a).status, 'loading');
  cm.entry(b); cm.entry(l);
  await flush();
  for (const s of [a, b, l]) assert.equal(cm.entry(s).status, 'ready', cm.entry(s).error);
  assert.equal(R.uploads.length, 2, 'one upload per asset (left, right), shared by the two right sources');
  assert.notEqual(cm.entry(a).rig, cm.entry(b).rig, 'each source moves its own rig');
  // Never the 2048² image first: every decode is at the target size.
  assert.deepEqual(B.calls, [{ w: 1024, h: 1024 }, { w: 1024, h: 1024 }]);
  // The profile JSON fetched once for both hands.
  assert.equal(F.count.get(new URL('meta-quest-touch-plus/profile.json', MODELS).href), 1);
  // A lost context decodes again from the file's bytes.
  const again = await R.uploads[0].opts.redecode();
  assert.equal(again.filter(Boolean).length, 1);
  assert.deepEqual(B.calls.at(-1), { w: 1024, h: 1024 });
  // The pose of a ready source.
  const I = new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
  assert.ok(cm.pose(a, I, { buttons: [], axes: [] }));
});

test('a failed load is tried once more after 10 s, then no more', async () => {
  fakeBitmaps();
  let t = 0, failing = true;
  const F = fakeFetch((u) => failing && u.endsWith('right.glb'));
  const cm = new ControllerModels(fakeRenderer(), { base: MODELS, fetchImpl: F.fetch, now: () => t });
  const s = src('right');
  cm.entry(s); await flush();
  assert.equal(cm.entry(s).status, 'failed');
  t = RETRY_MS - 1; cm.entry(s); await flush();
  assert.equal(cm.entry(s).status, 'failed', 'not before 10 s');
  failing = false;
  t = RETRY_MS + 1; cm.entry(s); await flush();
  assert.equal(cm.entry(s).status, 'ready', 'the second try loads it');
  // Always failing: two tries, then the page's own model for good.
  const F2 = fakeFetch((u) => u.endsWith('left.glb'));
  let t2 = 0;
  const cm2 = new ControllerModels(fakeRenderer(), { base: MODELS, fetchImpl: F2.fetch, now: () => t2 });
  const l = src('left');
  for (let k = 0; k < 4; k++) { cm2.entry(l); await flush(); t2 += RETRY_MS + 1; }
  assert.equal(F2.count.get(new URL('meta-quest-touch-plus/left.glb', MODELS).href), 2);
  // No vendored profile: no retry at all.
  const cm3 = new ControllerModels(fakeRenderer(), { base: MODELS, fetchImpl: fakeFetch().fetch, now: () => t2 });
  const v = src('right', ['valve-index']);
  cm3.entry(v); await flush();
  t2 += RETRY_MS + 1; cm3.entry(v); await flush();
  assert.equal(cm3.entry(v).tries, 2);
  assert.match(cm3.entry(v).error, /no vendored profile/);
});

test('decoding: one image at a time, and the decoded ones closed when one fails', async () => {
  const png = readFileSync(new URL('meta-quest-touch-plus/right.glb', MODELS));   // any bytes: the size comes from the header or not at all
  const model = { materials: [{ texture: 0 }, { texture: 1 }], images: [{ mime: 'image/png', bytes: new Uint8Array(png) }, { mime: 'image/png', bytes: new Uint8Array(png) }] };
  const B = fakeBitmaps({ failOn: 2 });
  await assert.rejects(decodeModelImages(model), /decode failed/);
  assert.equal(B.closed, 1);
});

test('the models a headset used are remembered and loaded on the next page before Enter VR', async () => {
  fakeBitmaps();
  const store = new Map();
  globalThis.localStorage = { getItem: (k) => (store.has(k) ? store.get(k) : null), setItem: (k, v) => store.set(k, String(v)) };
  try {
    const cm = new ControllerModels(fakeRenderer(), { base: MODELS, fetchImpl: fakeFetch().fetch });
    assert.deepEqual(await cm.preload(), [], 'nothing remembered: nothing loaded');
    cm.entry(src('left', ['meta-quest-touch-plus-v2', 'meta-quest-touch-plus'])); cm.entry(src('right', ['meta-quest-touch-plus-v2']));
    await flush();
    assert.deepEqual(JSON.parse(store.get('visivo.vr.controllerProfiles')), { left: 'meta-quest-touch-plus-v2', right: 'meta-quest-touch-plus-v2' });
    const R2 = fakeRenderer();
    const next = new ControllerModels(R2, { base: MODELS, fetchImpl: fakeFetch().fetch });
    assert.deepEqual((await next.preload()).sort(), ['meta-quest-touch-plus-v2/left', 'meta-quest-touch-plus-v2/right']);
    assert.equal(R2.uploads.length, 2);
    // The session's sources then find them uploaded: no second upload.
    next.entry(src('left', ['meta-quest-touch-plus-v2'])); await flush();
    assert.equal(R2.uploads.length, 2);
    // A broken record is ignored.
    store.set('visivo.vr.controllerProfiles', '{not json');
    assert.deepEqual(await new ControllerModels(fakeRenderer(), { base: MODELS, fetchImpl: fakeFetch().fetch }).preload(), []);
  } finally {
    delete globalThis.localStorage;
  }
});
