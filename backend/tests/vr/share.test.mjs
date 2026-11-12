// Selections shared with the desktop (issue #188): node --test backend/tests/vr/share.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import {
  newClientId, publishQuery, boxGeometry, lassoMeta, boxMeta, receivedNote, parseMask, createShare,
  POLL_WAIT_S, BACKOFF_MS, NOT_READY_MS, PUBLISH_TIMEOUT_MS, READ_TIMEOUT_MS, RETRY_429_MS, isUnsupported, reconcileDecision,
  confirmDraw, nonceOf, newNonce, noteLocal, publishOutcomeOf, instanceVerdict,
} from '../../app/static/vr/js/share.js';
import { readFileSync } from 'node:fs';

// The cases both clients are tested on (the desktop: tests/test_shared_selection.cpp).
const CASES = JSON.parse(readFileSync(new URL('./share_rule_cases.json', import.meta.url), 'utf8'));
const stateOf = (o = {}) => {
  const s = { ...CASES.state, ...o };
  return { client: s.client, shownNonce: s.shown, localNonce: s.local, floorSeq: s.floor, fullShape: s.full };
};

const info = (o = {}) => ({
  seq: 5, kind: 'lasso', source: 'desktop', client: 'desk-1', dataset_id: 'ds_a',
  origin: [3, 4, 5], dims: [2, 2, 1], full_shape: [10, 12, 8], count: 3,
  stats: { valid: true, flux_jy_kms: 6.0614, selected_voxels: 3 }, meta: { nonce: 'n-desk' }, ...o,
});

test('a client id and a nonce are short random ids of their own', () => {
  const a = newClientId(), b = newClientId();
  assert.match(a, /^vr-[0-9a-z]{12}$/);
  assert.notEqual(a, b);
  assert.match(newNonce(), /^[0-9a-z]{16}$/);
  assert.notEqual(newNonce(), newNonce());
  assert.equal(nonceOf(info()), 'n-desk');
  assert.equal(nonceOf(info({ meta: {} })), '');
});

test('the publish query names the box, the side and the record', () => {
  const q = publishQuery({ datasetId: 'ds_1', kind: 'lasso', client: 'vr-x', origin: [1, 2, 3], dims: [4, 5, 6],
    meta: lassoMeta({ seed_z: 1, seed_y: 2, seed_x: 3, reach: 8 }, { Reach: '8' }, 0) });
  assert.equal(q.get('dataset_id'), 'ds_1');
  assert.equal(q.get('source'), 'headset');
  assert.equal(q.get('kind'), 'lasso');
  assert.deepEqual(['origin_x', 'origin_y', 'origin_z', 'width', 'height', 'depth'].map((k) => q.get(k)), ['1', '2', '3', '4', '5', '6']);
  const meta = JSON.parse(q.get('meta'));
  assert.deepEqual(meta.grow, { seed_z: 1, seed_y: 2, seed_x: 3, reach: 8 });
  assert.equal(meta.made_in, 'VR page');
});

test('a box is its inclusive range as origin and dims', () => {
  assert.deepEqual(boxGeometry([[2, 7], [3, 3], [0, 9]]), { origin: [2, 3, 0], dims: [6, 1, 10] });
  assert.deepEqual(boxMeta([[2, 7], [3, 3], [0, 9]]).box, [[2, 7], [3, 3], [0, 9]]);
});

test('the shared cases: the rule, confirm, a local selection, publish outcomes, instances', () => {
  for (const c of CASES.decision) {
    const latest = c.latest === null ? null : { ...CASES.base, ...c.latest };
    assert.equal(reconcileDecision(latest, stateOf(c.state)), c.expect, c.name);
  }
  for (const c of CASES.confirm) {
    assert.equal(confirmDraw(c.now, c.fetched, { shownNonce: c.before.shown, floorSeq: c.before.floor },
      { shownNonce: c.after.shown, floorSeq: c.after.floor }), c.expect, c.name);
  }
  for (const c of CASES.note_local) {
    const out = noteLocal({ shownNonce: c.state.shown, localNonce: c.state.local, floorSeq: c.state.floor }, c.seen);
    assert.deepEqual([out.shownNonce, out.localNonce, out.floorSeq], [c.expect.shown, c.expect.local, c.expect.floor], c.name);
  }
  for (const c of CASES.publish_outcome) assert.equal(publishOutcomeOf(c.status, c.detail), c.expect, `${c.status} ${c.detail || ''}`);
  for (const c of CASES.instance) assert.equal(instanceVerdict(c.known, c.seen), c.expect, `${c.known}->${c.seen}`);
});

test('the note says who made it, the voxels and the flux', () => {
  assert.equal(receivedNote(info()), 'Kinematic Lasso from the desktop: 3 voxels, flux 6.061 Jy km/s');
  assert.equal(receivedNote(info({ kind: 'box', stats: { flux_jy_kms: null } })), 'box from the desktop: 3 voxels');
  assert.match(receivedNote(info(), true), /from this page/);
});

const hdrs = (o) => ({ get: (k) => (k in o ? String(o[k]) : null) });

test('a shared mask is read as the grow route answers', () => {
  const buf = new Float32Array([1, 0, 1, 1]).buffer;
  const m = parseMask(buf, hdrs({ 'X-Visivo-Width': 2, 'X-Visivo-Height': 2, 'X-Visivo-Depth': 1,
    'X-Visivo-Origin': '3,4,5', 'X-Visivo-Selected-Count': 3 }));
  assert.deepEqual(m.dims, [2, 2, 1]);
  assert.deepEqual(m.origin, [3, 4, 5]);
  assert.equal(m.count, 3);
  assert.deepEqual([...m.mask], [1, 0, 1, 1]);
  assert.throws(() => parseMask(new ArrayBuffer(8), hdrs({ 'X-Visivo-Width': 2, 'X-Visivo-Height': 2,
    'X-Visivo-Depth': 1, 'X-Visivo-Origin': '0,0,0' })), /bad shared mask/);
});

test('only a bare "Not Found" or a 405 means a backend without the hub', () => {
  assert.equal(isUnsupported(404, '{"detail":"Not Found"}'), true);
  assert.equal(isUnsupported(405, ''), true);
  assert.equal(isUnsupported(404, '{"detail":"Session not found."}'), false);
  assert.equal(isUnsupported(404, '{"detail":"Unknown dataset_id \'ds\'. Open the dataset first"}'), false);
  assert.equal(isUnsupported(404, 'garbage'), false);
  assert.equal(isUnsupported(500, '{"detail":"Not Found"}'), false);
});

// ── The controller against a model of the hub ──────────────────────────────
// hub.latest is what GET ?wait=0 answers; the long poll answers from
// hub.polls (204 or 'entry' = the latest; none queued: it waits until
// released); POST stores the publish as the latest (or hub.onPost decides);
// the mask route serves the latest seq only, after hub.maskGate if set.

function fakeHub() {
  const hub = { latest: null, seq: 0, polls: [], calls: [], maskGate: null, onPost: null, waiting: [] };
  const resp = (status, body, headers = {}) => ({
    status, ok: status >= 200 && status < 300, headers: hdrs(headers),
    text: async () => (typeof body === 'string' ? body : JSON.stringify(body)),
    json: async () => body,
    arrayBuffer: async () => body,
  });
  hub.store = (o) => { hub.latest = info({ seq: ++hub.seq, ...o }); return hub.latest; };
  hub.release = () => { const w = hub.waiting.shift(); if (w) w(); };
  hub.fetch = async (url, opts = {}) => {
    hub.calls.push({ url, opts });
    if (opts.keepalive) throw new Error('keepalive must not be used');
    if (url.startsWith('/v1/share/selection/mask')) {
      if (hub.maskGate) await hub.maskGate;
      const seq = Number(new URL(url, 'http://x').searchParams.get('seq'));
      if (!hub.latest || hub.latest.seq !== seq) return resp(404, '{"detail":"no longer the latest"}');
      return resp(200, new Float32Array([1, 0, 1, 1]).buffer, { 'X-Visivo-Width': 2, 'X-Visivo-Height': 2,
        'X-Visivo-Depth': 1, 'X-Visivo-Origin': '3,4,5', 'X-Visivo-Selected-Count': 3 });
    }
    if (opts.method === 'POST') {
      const q = new URL(url, 'http://x').searchParams;
      const entry = { client: q.get('client'), source: 'headset', meta: JSON.parse(q.get('meta')) };
      if (hub.onPost) return hub.onPost(entry, opts, resp);
      const e = hub.store(entry);
      return resp(200, { valid: true, seq: e.seq, count: 3, stats: {} });
    }
    const q = new URL(url, 'http://x').searchParams;
    if (q.get('wait') === '0') return hub.latest ? resp(200, hub.latest) : resp(204, '');
    const next = hub.polls.shift();
    if (next === undefined) {
      await new Promise((r) => hub.waiting.push(r));
      return resp(204, '');
    }
    if (next instanceof Error) throw next;
    if (next.status) return resp(next.status, next.body ?? '');
    return hub.latest ? resp(200, hub.latest) : resp(204, '');
  };
  return hub;
}

const flush = async (n = 30) => { for (let i = 0; i < n; i++) await new Promise((r) => setTimeout(r, 0)); };

function controller(hub, o = {}) {
  const got = [];
  const timers = [];
  let nonces = 0;
  const share = createShare({
    clientId: 'vr-me', fetch: hub.fetch, nonce: () => `n-page-${++nonces}`,
    backend: () => ({ headers: () => ({ 'X-Visivo-Token': 't', 'X-Visivo-Session': 's' }) }),
    datasetId: () => 'ds_a', fullShape: () => [10, 12, 8],
    onReceive: o.onReceive || ((i, m, own) => { got.push([i.seq, own]); return true; }),
    setTimeout: (f, ms) => { timers.push({ f, ms }); return timers.length; }, clearTimeout: (id) => { if (timers[id - 1]) timers[id - 1].f = null; },
  });
  return { share, got, timers };
}
const lasso = (share) => share.publish('lasso', { bytes: new ArrayBuffer(16), origin: [3, 4, 5], dims: [2, 2, 1], meta: lassoMeta({ seed_z: 5 }) });

test('sharing under a proxy prefix publishes, polls and reads masks at that prefix', async (t) => {
  const previousWindow = globalThis.window;
  globalThis.window = { location: { pathname: '/node/r33/proxy/8000/vr/' } };
  t.after(() => {
    if (previousWindow === undefined) delete globalThis.window;
    else globalThis.window = previousWindow;
  });
  const prefix = '/node/r33/proxy/8000';
  const hub = fakeHub();
  const fetch = hub.fetch;
  const calls = [];
  hub.fetch = (url, opts) => {
    assert.ok(url.startsWith(`${prefix}/v1/share/selection`), url);
    calls.push({ url, opts });
    return fetch(url.slice(prefix.length), opts);
  };
  const c = controller(hub);
  t.after(() => c.share.stop());
  c.share.start('ds_a');
  await flush();
  await lasso(c.share);
  hub.store({ client: 'desk', meta: { nonce: 'desktop-new' } });
  hub.release();
  await flush();
  assert.deepEqual(c.got, [[2, false]]);
  assert.ok(calls.some(({ opts }) => opts.method === 'POST'));
  assert.ok(calls.some(({ url }) => url.includes('/mask?')));
  assert.ok(calls.some(({ url }) => new URL(url, 'http://x').searchParams.get('wait') === String(POLL_WAIT_S)));
});

test('an entry already in the hub is not drawn; a later one is; our own echo is not', async () => {
  const hub = fakeHub();
  hub.store({ client: 'desk', meta: { nonce: 'old' } });      // seq 1, there before
  const t = controller(hub);
  t.share.start('ds_a');
  await flush();
  assert.deepEqual(t.got, []);
  assert.equal(t.share.st.lastSeq, 1);
  hub.store({ client: 'desk', meta: { nonce: 'd2' } });       // seq 2, the desktop's
  hub.release();
  await flush();
  assert.deepEqual(t.got, [[2, false]]);
  assert.equal(t.share.st.shownNonce, 'd2');
  await lasso(t.share);                                       // seq 3, ours
  await flush();
  hub.release();
  await flush();
  assert.deepEqual(t.got, [[2, false]], 'our own echo is not drawn');
  assert.equal(t.share.st.shownNonce, 'n-page-1');
  const polls = hub.calls.filter((c) => /wait=20/.test(c.url));
  assert.equal(polls[0].opts.headers['X-Visivo-Session'], 's');
  t.share.stop();
});

test('M1: a lasso made while a large mask is fetched is not drawn over', async () => {
  const hub = fakeHub();
  const t = controller(hub);
  t.share.start('ds_a');
  await flush();
  let open;
  hub.maskGate = new Promise((r) => { open = r; });
  hub.store({ client: 'desk', meta: { nonce: 'd1' } });       // B, seq 1
  hub.release();
  await flush();                                              // B's mask is being fetched
  // A local lasso, published while B's mask is on its way; its answer is slow.
  let answer;
  hub.onPost = (entry, opts, resp) => new Promise((r) => { answer = () => { const e = hub.store(entry); r(resp(200, { valid: true, seq: e.seq, count: 3 })); }; });
  const pub = lasso(t.share);
  open();
  await flush();
  assert.deepEqual(t.got, [], 'B is not drawn over the local lasso');
  answer();
  await pub;
  await flush();
  assert.deepEqual(t.got, []);
  assert.equal(t.share.st.shownNonce, 'n-page-1');
  assert.equal(nonceOf(hub.latest), 'n-page-1', 'the hub holds ours too');
  t.share.stop();
});

test('M2: a publish whose answer is lost after the hub kept it still wins', async () => {
  const hub = fakeHub();
  const t = controller(hub);
  t.share.start('ds_a');
  await flush();
  hub.store({ client: 'desk', meta: { nonce: 'd1' } });       // B, seq 1, not yet seen by the page
  hub.onPost = (entry) => { hub.store(entry); throw new Error('connection reset'); };   // A kept, answer lost
  await lasso(t.share);
  await flush();
  assert.deepEqual(t.got, [], 'B (older) is never drawn: the hub holds ours');
  assert.equal(nonceOf(hub.latest), 'n-page-1');
  assert.equal(t.share.st.publishOutcome, 'failed');
  hub.release();
  await flush();
  assert.deepEqual(t.got, []);
  t.share.stop();
});

test('B drawn before our A reached the hub: A is drawn back when it lands', async () => {
  const hub = fakeHub();
  const t = controller(hub);
  t.share.start('ds_a');
  await flush();
  let land;
  hub.onPost = (entry, opts, resp) => new Promise((r) => { land = () => { const e = hub.store(entry); r(resp(200, { valid: true, seq: e.seq, count: 3 })); }; });
  const pub = lasso(t.share);                                 // A on its way
  hub.store({ client: 'desk', meta: { nonce: 'd1' } });       // B lands first
  hub.release();
  await flush();
  assert.deepEqual(t.got, [[1, false]], 'B is the hub\'s latest: drawn');
  land();                                                     // A lands (seq 2) and is answered
  await pub;
  await flush();
  assert.deepEqual(t.got, [[1, false], [2, true]], 'A, the hub\'s latest and ours, is drawn back');
  assert.equal(t.share.st.shownNonce, 'n-page-1');
  t.share.stop();
});

test('a publish refused as stale converges on the later one', async () => {
  const hub = fakeHub();
  const t = controller(hub);
  t.share.start('ds_a');
  await flush();
  hub.onPost = (entry, opts, resp) => { hub.store({ client: 'desk', meta: { nonce: 'd9' } }); return resp(409, { detail: 'stale' }); };
  await lasso(t.share);
  await flush();
  assert.equal(t.share.st.publishOutcome, 'stale');
  assert.deepEqual(t.got, [[1, false]]);
  assert.equal(t.share.st.shownNonce, 'd9');
  t.share.stop();
});

test('a hung publish times out, counts as settled and reads the hub', async () => {
  const hub = fakeHub();
  const t = controller(hub);
  t.share.start('ds_a');
  await flush();
  let signal;
  hub.onPost = (entry, opts) => { signal = opts.signal; return new Promise((res, rej) => { opts.signal.addEventListener('abort', () => rej(new Error('aborted'))); }); };
  const mark = t.timers.length;   // the publish's own timer, not the long poll's
  const pub = lasso(t.share);
  await flush();
  const to = t.timers.slice(mark).find((x) => x.ms === PUBLISH_TIMEOUT_MS && x.f);
  assert.ok(to, 'the publish has a timeout');
  hub.store({ client: 'desk', meta: { nonce: 'd1' } });       // meanwhile the desktop's entry
  const before = hub.calls.filter((c) => /wait=0/.test(c.url)).length;
  to.f();
  await pub;
  await flush();
  assert.equal(signal.aborted, true);
  assert.equal(t.share.st.publishOutcome, 'timeout');
  assert.ok(hub.calls.filter((c) => /wait=0/.test(c.url)).length > before, 'the hub is read again');
  assert.deepEqual(t.got, [[1, false]], 'the hub\'s latest is the desktop\'s: drawn');
  t.share.stop();
});

test('a selection that arrives before the cube is drawn is offered again', async () => {
  const hub = fakeHub();
  let ready = false;
  const got = [];
  const t = controller(hub, { onReceive: (i) => { if (!ready) return false; got.push(i.seq); return true; } });
  t.share.start('ds_a');
  await flush();
  hub.store({ client: 'desk', meta: { nonce: 'd1' } });
  hub.polls.push('entry');
  hub.release();
  await flush();
  assert.deepEqual(got, []);
  assert.equal(t.share.st.lastSeq, 0, 'the cursor stays on a selection not drawn');
  const wait = t.timers.find((x) => x.ms === NOT_READY_MS && x.f);
  assert.ok(wait);
  ready = true;
  hub.polls.push('entry');
  wait.f();
  await flush();
  assert.deepEqual(got, [1]);
  assert.equal(t.share.st.lastSeq, 1);
  t.share.stop();
});

test('a local clear keeps the entries seen so far off the screen', async () => {
  const hub = fakeHub();
  const t = controller(hub);
  t.share.start('ds_a');
  await flush();
  hub.store({ client: 'desk', meta: { nonce: 'd1' } });
  hub.release();
  await flush();
  assert.deepEqual(t.got, [[1, false]]);
  t.share.dismiss();
  await t.share.reconcile();                                  // the safety net runs
  assert.deepEqual(t.got, [[1, false]], 'not drawn again after Clear');
  hub.store({ client: 'desk', meta: { nonce: 'd2' } });
  await t.share.reconcile();
  assert.deepEqual(t.got, [[1, false], [2, false]], 'a newer one is');
  t.share.stop();
});

test('a backend without the hub stops the link; a session 404 and errors back off', async () => {
  const hub = fakeHub();
  hub.fetch = async () => ({ status: 404, ok: false, text: async () => '{"detail":"Not Found"}' });
  const t = controller(hub);
  t.share.start('ds_a');
  await flush();
  assert.equal(t.share.st.running, false);
  assert.equal(t.share.st.unsupported, true);
  assert.match(t.share.hudLines().join('\n'), /does not share selections/);

  const hub2 = fakeHub();
  const t2 = controller(hub2);
  t2.share.start('ds_a');
  await flush();
  hub2.polls.push({ status: 404, body: '{"detail":"Session not found."}' });
  hub2.release();      // the waiting poll answers 204 -> reconcile -> next poll gets the 404
  await flush();
  assert.equal(t2.share.st.unsupported, false);
  assert.equal(t2.share.st.running, true);
  assert.ok(t2.timers.some((x) => x.ms === BACKOFF_MS[0]));
  assert.match(t2.share.st.error, /404/);
  t2.share.stop();
  assert.equal(t2.share.st.running, false);
});

test('publishing carries a nonce, the token and the session; without a backend nothing is sent', async () => {
  const hub = fakeHub();
  const t = controller(hub);
  t.share.start('ds_a');
  await flush();
  const r = await lasso(t.share);
  assert.equal(r.seq, 1);
  const c = hub.calls.find((x) => x.opts.method === 'POST');
  assert.equal(c.opts.headers['Content-Type'], 'application/octet-stream');
  assert.match(c.url, /client=vr-me/);
  assert.equal(JSON.parse(new URL(c.url, 'http://x').searchParams.get('meta')).nonce, 'n-page-1');
  assert.match(t.share.st.note, /shared with the desktop \(3 voxels/);
  t.share.stop();
  const off = createShare({ fetch: hub.fetch, backend: () => null, datasetId: () => '', fullShape: () => null, onReceive: () => {} });
  assert.equal(await off.publish('box', { origin: [0, 0, 0], dims: [1, 1, 1] }), null);
});

test('a publish the hub refuses for good stays local, and a later entry still wins', async () => {
  const hub = fakeHub();
  const t = controller(hub);
  t.share.start('ds_a');
  await flush();
  hub.onPost = (entry, opts, resp) => resp(422, { detail: 'The selection\'s box holds 20000000 voxels, over the 16777216-voxel limit' });
  await lasso(t.share);
  await flush();
  assert.equal(t.share.st.publishOutcome, 'refused');
  assert.match(t.share.st.note, /^not shared: .*voxel limit/);
  assert.equal(t.share.st.localNonce, '', 'no longer this page\'s latest own');
  assert.equal(t.share.st.shownNonce, 'n-page-1', 'still on screen, local');
  hub.store({ client: 'desk', meta: { nonce: 'd1' } });
  hub.release();
  await flush();
  assert.deepEqual(t.got, [[1, false]], 'the desktop\'s later entry wins');
  t.share.stop();
});

test('a publish answered 429 is retried once', async () => {
  const hub = fakeHub();
  const t = controller(hub);
  t.share.start('ds_a');
  await flush();
  let n = 0;
  hub.onPost = (entry, opts, resp) => {
    if (++n === 1) return resp(429, { detail: 'busy' });
    const e = hub.store(entry);
    return resp(200, { valid: true, seq: e.seq, count: 3 });
  };
  const pub = lasso(t.share);
  await flush();
  const retry = t.timers.find((x) => x.ms === RETRY_429_MS && x.f);
  assert.ok(retry, 'waits before the retry');
  retry.f();
  const r = await pub;
  assert.equal(n, 2);
  assert.equal(r.seq, 1);
  assert.equal(t.share.st.publishOutcome, 'kept');
  t.share.stop();
});

test('a hung read of the hub times out instead of hanging the link', async () => {
  const hub = fakeHub();
  const plain = hub.fetch;
  let hang = true;
  hub.fetch = (url, opts = {}) => {
    if (hang && /wait=0/.test(url)) {
      return new Promise((res, rej) => opts.signal.addEventListener('abort', () => rej(new Error('aborted'))));
    }
    return plain(url, opts);
  };
  const t = controller(hub);
  t.share.start('ds_a');
  await flush();
  const to = t.timers.find((x) => x.ms === READ_TIMEOUT_MS && x.f);
  assert.ok(to, 'the read has a timeout');
  hang = false;
  to.f();
  await flush();
  assert.match(t.share.st.error, /no answer in 30 s/);
  assert.ok(t.timers.some((x) => x.ms === BACKOFF_MS[0]), 'the link backs off and goes on');
  t.share.stop();
});

test('stop() aborts the reads in flight', async () => {
  const hub = fakeHub();
  const signals = [];
  hub.fetch = (url, opts = {}) => { signals.push(opts.signal); return new Promise(() => {}); };
  const t = controller(hub);
  t.share.start('ds_a');
  await flush(5);
  t.share.stop();
  assert.ok(signals.length && signals.every((x) => x.aborted));
});

test('a backend restart (another instance) resets the cursor, the floor and the nonces', async () => {
  const hub = fakeHub();
  const t = controller(hub);
  hub.store({ client: 'desk', meta: { nonce: 'old' }, instance: 'i-1' });   // seq 1
  hub.store({ client: 'desk', meta: { nonce: 'old2' }, instance: 'i-1' });  // seq 2: the floor
  t.share.start('ds_a');
  await flush();
  assert.equal(t.share.st.floorSeq, 2);
  assert.equal(t.share.st.instance, 'i-1');
  // The backend restarts: seqs begin again at 1, below the old floor.
  hub.seq = 0;
  hub.store({ client: 'desk', meta: { nonce: 'new' }, instance: 'i-2' });
  hub.release();
  await flush();
  assert.equal(t.share.st.instance, 'i-2');
  assert.deepEqual(t.got, [[1, false]], 'the new instance\'s entry is drawn despite its low seq');
  t.share.stop();
});

test('a publish refused because the selection was cleared after it arrived stays local', async () => {
  const hub = fakeHub();
  const t = controller(hub);
  t.share.start('ds_a');
  await flush();
  hub.onPost = (entry, opts, resp) => resp(409, { detail: 'cleared: the selection was cleared after this publish arrived' });
  await lasso(t.share);
  await flush();
  assert.equal(t.share.st.publishOutcome, 'cleared');
  assert.match(t.share.st.note, /cleared after it was sent/);
  assert.equal(t.share.st.localNonce, '');
  assert.equal(t.share.st.shownNonce, 'n-page-1', 'still on screen, local');
  t.share.stop();
});
