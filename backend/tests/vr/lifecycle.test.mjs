// The page's lifecycle (epic #166): versions, failure kinds, backoff, the
// watch state machine and the request hooks.
// node --test backend/tests/vr/lifecycle.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import * as L from '../../app/static/vr/js/lifecycle.js';
import { requestError, requestHooks, hookedFetch, Backend } from '../../app/static/vr/js/api.js';

test('the page build comes from its versioned address', () => {
  assert.equal(L.pageBuild('http://localhost:8000/vr/b/0a1b2c3d4e5f/js/main.js'), '0a1b2c3d4e5f');
  assert.equal(L.pageBuild('http://localhost:8000/vr/js/main.js'), '');
  assert.equal(L.pageBuild(''), '');
});

test('versions: same build ok, another build reload (soft), API out of range', () => {
  const info = { build: 'aaa111', vr_api: 2, vr_api_min: 1 };
  assert.deepEqual(L.compareVersions({ build: 'aaa111', api: 1 }, info), { ok: true });
  assert.deepEqual(L.compareVersions({ build: '', api: 2 }, info), { ok: true });       // unversioned page: API only
  const r = L.compareVersions({ build: 'bbb222', api: 1 }, info);
  assert.equal(r.ok, false); assert.equal(r.kind, 'reload'); assert.equal(r.soft, true);
  const old = L.compareVersions({ build: 'aaa111', api: 1 }, { build: 'aaa111', vr_api: 3, vr_api_min: 2 });
  assert.equal(old.kind, 'reload'); assert.ok(!old.soft);
  const newer = L.compareVersions({ build: 'aaa111', api: 3 }, info);
  assert.equal(newer.kind, 'update-app');
  assert.deepEqual(L.compareVersions({ build: 'x', api: 1 }, null), { ok: true });
});

test('failures are classified from the Backend errors', () => {
  assert.equal(L.classifyFailure(requestError(0, 'the backend did not answer')), 'network');
  assert.equal(L.classifyFailure(requestError(401, 'Missing or invalid X-Visivo-Token header.')), 'auth');
  assert.equal(L.classifyFailure(new Error('403: Operator token required')), 'auth');
  assert.equal(L.classifyFailure(requestError(404, "Unknown dataset_id 'ds-1'")), 'dataset-gone');
  assert.equal(L.classifyFailure(new Error('404: Session not found.')), 'session-gone');
  assert.equal(L.classifyFailure(new Error('400: X-Visivo-Session must be high-entropy')), 'session-gone');
  assert.equal(L.classifyFailure(new Error('429: slow down')), 'busy');
  assert.equal(L.classifyFailure(new Error('500: boom')), 'server');
  assert.equal(L.classifyFailure(new Error('422: bad')), 'other');
  assert.equal(L.classifyFailure(new TypeError('Failed to fetch')), 'network');
  assert.equal(L.classifyFailure(null), 'other');
});

test('backoff doubles to its cap and resets', () => {
  const b = new L.Backoff({ base: 1000, max: 15000 });
  assert.deepEqual([b.next(), b.next(), b.next(), b.next(), b.next(), b.next()], [1000, 2000, 4000, 8000, 15000, 15000]);
  b.reset();
  assert.equal(b.next(), 1000);
});

test('loopback and headset detection', () => {
  for (const h of ['localhost', '127.0.0.1', '127.1.2.3', '[::1]', '::1']) assert.ok(L.isLoopback(h), h);
  for (const h of ['192.168.1.4', 'mac.local', '']) assert.ok(!L.isLoopback(h), h);
  assert.ok(L.isHeadsetBrowser('Mozilla/5.0 (X11; Linux x86_64; Quest 3) AppleWebKit/537.36 OculusBrowser/40.0 Chrome/132 VR Safari/537.36'));
  assert.ok(!L.isHeadsetBrowser('Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) Chrome/142'));
});

test('every notice offers a way out and says what to do', () => {
  const kinds = ['unreachable', 'http-error', 'restarted', 'reopened', 'reopen-failed', 'auth', 'reload', 'update-app',
    'context-lost', 'context-failed', 'context-restored', 'xr-ended', 'budget'];
  for (const k of kinds) {
    for (const inVR of [false, true]) {
      const n = L.noticeFor(k, { inVR, loopback: true, headset: true, retryInS: 4, attempts: 2, detail: 'd', why: 'w' });
      assert.equal(n.id, k);
      assert.ok(['error', 'warning', 'info'].includes(n.level), k);
      assert.ok(n.title && n.lines.length >= 1, k);
      assert.ok(n.actions.length >= 1 && n.actions.length <= 4, k);
      for (const a of n.actions) assert.ok(['retry', 'reload', 'exit-vr', 'dismiss', 'hide', 'reopen', 'enter-vr'].includes(a.id), `${k}: ${a.id}`);
      // A sticky notice (it stays until the condition ends) must not trap the user in VR.
      if (n.sticky && inVR && !['context-lost', 'context-failed', 'budget'].includes(k)) {
        assert.ok(n.actions.some((a) => a.id === 'exit-vr'), `${k} in VR has no Exit VR`);
      }
      // Every sticky notice can be got out of the way without leaving VR.
      if (n.sticky && !['context-lost', 'context-failed'].includes(k)) {
        assert.ok(n.actions.some((a) => a.id === 'hide' || a.id === 'dismiss'), `${k} cannot be hidden`);
        if (n.actions.some((a) => a.id === 'hide')) assert.ok(n.badge, `${k} hides without a badge`);
      }
    }
  }
  const usb = L.noticeFor('unreachable', { loopback: true, headset: true, retryInS: 4 }).lines.join(' ');
  assert.match(usb, /adb reverse/);
  assert.match(usb, /Open on Headset/);
  assert.match(usb, /Trying again by itself/);
  assert.doesNotMatch(usb, /in \d+ s/, 'no countdown on the card');
  const lan = L.noticeFor('unreachable', { loopback: false, headset: true }).lines.join(' ');
  assert.doesNotMatch(lan, /adb reverse/);
  assert.match(L.noticeFor('auth').lines.join(' '), /Open on Headset/);
});

// ── The watch ─────────────────────────────────────────────────────────────

function fakeTimers() {
  const q = [];
  return {
    q,
    setTimeout(f, ms) { const h = { f, ms }; q.push(h); return h; },
    clearTimeout(h) { const i = q.indexOf(h); if (i >= 0) q.splice(i, 1); },
    async fire() { const h = q.shift(); h.f(); await new Promise((r) => setImmediate(r)); return h.ms; },
  };
}

test('the watch: down with backoff, back, restarted, token refused, mismatch', async () => {
  const timers = fakeTimers();
  const events = [];
  let answer = { build: 'b1', vr_api: 1, vr_api_min: 1, instance: 'i1', token_ok: true };
  let fail = false;
  const life = new L.Lifecycle({
    page: { build: 'b1', api: 1 }, timers, heartbeatMs: 5000,
    fetchInfo: async () => { if (fail) throw requestError(0, 'down'); return answer; },
    onChange: (ev) => events.push(ev),
  });
  life.start();
  await life.inFlight; await new Promise((r) => setImmediate(r));
  assert.equal(life.state, 'ok');
  assert.equal(events.at(-1).restarted, false);
  assert.equal(timers.q.at(-1).ms, 5000);

  fail = true;
  await timers.fire();
  assert.equal(life.state, 'down');
  assert.equal(events.at(-1).retryInMs, 1000);
  assert.equal(await timers.fire(), 1000);
  assert.equal(events.at(-1).retryInMs, 2000);
  assert.equal(await timers.fire(), 2000);
  assert.equal(events.at(-1).attempts, 3);

  fail = false;
  answer = { ...answer, instance: 'i2' };          // it restarted meanwhile
  await timers.fire();
  assert.equal(life.state, 'ok');
  assert.equal(events.at(-1).prev, 'down');
  assert.equal(events.at(-1).restarted, true);
  assert.equal(life.backoff.attempts, 0);

  answer = { ...answer, token_ok: false };
  await timers.fire();
  assert.equal(life.state, 'auth');

  answer = { ...answer, token_ok: true, vr_api_min: 2, vr_api: 2 };
  await timers.fire();
  assert.equal(life.state, 'mismatch');
  assert.equal(events.at(-1).version.kind, 'reload');

  answer = { ...answer, vr_api_min: 1, build: 'b2' };    // only another build: a soft reload, still ok
  await timers.fire();
  assert.equal(life.state, 'ok');
  assert.equal(events.at(-1).version.soft, true);
  life.stop();
  assert.equal(timers.q.length, 0);
});

test('a request that saw no backend starts a check at once', async () => {
  const timers = fakeTimers();
  let calls = 0;
  const life = new L.Lifecycle({ timers, fetchInfo: async () => { calls++; throw requestError(0, 'x'); } });
  life.stopped = false;
  assert.equal(life.report(requestError(0, 'x')), 'network');
  await life.inFlight;
  assert.equal(calls, 1);
  assert.equal(life.state, 'down');
  assert.equal(life.report(requestError(0, 'x')), 'network');     // already down: the backoff decides
  assert.equal(calls, 1);
  assert.equal(life.report(requestError(404, 'Unknown dataset_id')), 'dataset-gone');
});

test('requests report failures and turn a rejected fetch into a network error', async () => {
  const seen = [];
  requestHooks.onFailure = (e, path) => seen.push([e.status, e.network, path]);
  const realFetch = globalThis.fetch;
  try {
    globalThis.fetch = async () => { throw new TypeError('Failed to fetch'); };
    await assert.rejects(hookedFetch('/v1/vr/info', {}), (e) => e.network === true && /^network:/.test(e.message));
    globalThis.fetch = async () => new Response(JSON.stringify({ detail: "Unknown dataset_id 'x'" }), { status: 404 });
    const b = new Backend('tok');
    await assert.rejects(b.reducedInfo({ dataset_id: 'x' }), (e) => e.status === 404 && e.message === "404: Unknown dataset_id 'x'");
    globalThis.fetch = async (url, init) => {
      assert.equal(init.headers['X-Visivo-Token'], 'tok');
      return new Response(JSON.stringify({ build: 'b', token_ok: true }), { status: 200 });
    };
    assert.equal((await b.info()).token_ok, true);
  } finally {
    globalThis.fetch = realFetch;
    requestHooks.onFailure = null;
  }
  assert.deepEqual(seen, [[0, true, '/v1/vr/info'], [404, false, '/v1/cube/reduced_info']]);
});

test('the watch: HTTP errors are not network errors', async () => {
  const timers = fakeTimers();
  const events = [];
  let fail = requestError(404, 'Not Found');
  const life = new L.Lifecycle({ timers, page: { build: 'b', api: 1 },
    fetchInfo: async () => { if (fail) throw fail; return { build: 'b', vr_api: 1, vr_api_min: 1, instance: 'i', token_ok: true }; },
    onChange: (ev) => events.push(ev) });
  life.start(); await life.inFlight;
  assert.equal(life.state, 'mismatch');
  assert.equal(events.at(-1).version.kind, 'update-app');
  assert.equal(life.backoff.attempts, 0, 'a 404 is an answer: no backoff');
  fail = requestError(502, 'Bad Gateway');
  await timers.fire();
  assert.equal(life.state, 'down');
  assert.equal(events.at(-1).status, 502);
  assert.equal(life.backoff.attempts, 1);
  fail = requestError(0, 'nothing');
  await timers.fire();
  assert.equal(events.at(-1).status, 0);
  fail = requestError(401, 'no');
  await timers.fire();
  assert.equal(life.state, 'auth');
  fail = null;
  await timers.fire();
  assert.equal(life.state, 'ok');
  life.stop();
});

test('the watch survives a listener that throws, and a throwing fetch', async () => {
  const timers = fakeTimers();
  let calls = 0;
  const life = new L.Lifecycle({ timers, fetchInfo: () => { calls++; throw new Error('sync throw'); },
    onChange: () => { throw new Error('listener bug'); } });
  const origError = console.error; console.error = () => {};
  try {
    life.start();
    await life.inFlight;
    assert.equal(life.inFlight, null);
    assert.equal(timers.q.length, 1, 'the next check is still scheduled');
    await timers.fire();
    assert.equal(calls, 2);
    assert.equal(timers.q.length, 1);
  } finally { console.error = origError; life.stop(); }
});

test('a token check the backend skipped keeps a refused token refused', async () => {
  const timers = fakeTimers();
  let tok = false;
  const life = new L.Lifecycle({ timers, page: { build: 'b', api: 1 },
    fetchInfo: async () => ({ build: 'b', vr_api: 1, vr_api_min: 1, instance: 'i', token_ok: tok }) });
  life.start(); await life.inFlight;
  assert.equal(life.state, 'auth');
  tok = null;
  await timers.fire();
  assert.equal(life.state, 'auth');
  tok = true;
  await timers.fire();
  assert.equal(life.state, 'ok');
  tok = null;
  await timers.fire();
  assert.equal(life.state, 'ok');
  life.stop();
});

// ── Reopening the cube ────────────────────────────────────────────────────

function reopenIo({ gen, bumpAt = '', sessionGone = false, shape = [64, 64, 40] }) {
  const g = { v: gen };
  const calls = [];
  let opened = 0;
  return {
    g, calls,
    io: {
      loadGen: () => g.v,
      classify: L.classifyFailure,
      open: async (p) => {
        calls.push(['open', p]);
        opened++;
        if (bumpAt === 'open') g.v++;
        if (sessionGone && opened === 1) throw requestError(404, 'Session not found.');
        return { dataset_id: `ds-${opened}` };
      },
      newSession: async () => { calls.push(['newSession']); if (bumpAt === 'newSession') g.v++; },
      reducedInfo: async (o) => { calls.push(['info', o.dataset_id]); if (bumpAt === 'info') g.v++; return { full_shape: shape }; },
    },
  };
}

test('reopen: the same cube, a new dataset id', async () => {
  const { io, calls } = reopenIo({ gen: 3 });
  const r = await L.reopenCube({ path: '/d/c.fits', fullShape: [64, 64, 40], gen: 3 }, io);
  assert.equal(r.ok, true);
  assert.equal(r.open.dataset_id, 'ds-1');
  assert.deepEqual(calls, [['open', '/d/c.fits'], ['info', 'ds-1']]);
});

test('reopen: a session gone means a new session, then the path', async () => {
  const { io, calls } = reopenIo({ gen: 0, sessionGone: true });
  const r = await L.reopenCube({ path: 'p', fullShape: [64, 64, 40], gen: 0 }, io);
  assert.equal(r.ok, true);
  assert.deepEqual(calls.map((c) => c[0]), ['open', 'newSession', 'open', 'info']);
});

test('reopen: a load that finished meanwhile wins, at every step', async () => {
  for (const bumpAt of ['open', 'info', 'newSession']) {
    const { io } = reopenIo({ gen: 7, bumpAt, sessionGone: bumpAt === 'newSession' });
    const r = await L.reopenCube({ path: 'p', fullShape: [64, 64, 40], gen: 7 }, io);
    assert.equal(r.superseded, true, bumpAt);
    assert.equal(r.ok, false);
  }
});

test('every way a cube is installed bumps the load generation (main.js)', async () => {
  const { readFileSync } = await import('node:fs');
  const main = readFileSync(new URL('../../app/static/vr/js/main.js', import.meta.url), 'utf8');
  const body = (name) => {
    const i = main.indexOf(`function ${name}(`);
    assert.ok(i >= 0, name);
    return main.slice(i, main.indexOf('\n}\n', i));
  };
  // installCube is where a cube goes on show: synthetic, benchmark and backend loads alike.
  assert.match(body('installCube'), /state\.loadGen\+\+/);
  assert.match(body('loadSynthetic'), /installCube\(/);
  assert.match(body('loadReduced'), /installCube\(/);
  // The recovery reads the generation before its first await.
  const rec = body('recoverDataset');
  assert.ok(rec.indexOf('gen: state.loadGen') < rec.indexOf('= await'), 'snapshot before the first await');
});

test('a load that fails does not cancel a recovery, and loads touch the page\'s session only when they win (main.js)', async () => {
  const { readFileSync } = await import('node:fs');
  const main = readFileSync(new URL('../../app/static/vr/js/main.js', import.meta.url), 'utf8');
  const body = (name) => {
    const i = main.indexOf(`function ${name}(`);
    assert.ok(i >= 0, name);
    return main.slice(i, main.indexOf('\n}\n', i));
  };
  // 1. The generation moves when a load starts and when a cube goes on show,
  //    not when a load ends (one that failed installed nothing).
  const wl = body('withLoading');
  assert.match(wl, /state\.loadGen\+\+;\s*\n\s*try/);
  assert.doesNotMatch(wl.slice(wl.indexOf('finally')), /loadGen/);
  // 2. Every load and the recovery work on their own handle; nothing in them
  //    changes the page's session (ensureBackend()) but commitHandle.
  for (const name of ['loadFromBackend', 'loadPaired', 'recoverDataset']) {
    const b = body(name);
    assert.match(b, /loadHandle\(\)/, name);
    assert.doesNotMatch(b, /ensureBackend\(\)\.(adoptSession|newSession|open)\(/, name);
  }
  const lp = body('loadPaired');
  assert.ok(lp.indexOf('loads.begin(') < lp.indexOf('loadHandle()'));
  const ns = lp.indexOf('b.newSession()');
  assert.ok(ns > 0 && lp.lastIndexOf('stillCurrent(ticket)', ns) > lp.indexOf('pairingFallback('), 'checked before the new session');
  const lr = body('loadReduced');
  assert.ok(lr.lastIndexOf('stillCurrent(ticket)') < lr.indexOf('commitHandle(b)'), 'committed only after the last check');
  assert.ok(lr.indexOf('commitHandle(b)') < lr.indexOf('installCube('));
  const rd = body('recoverDataset');
  assert.ok(rd.indexOf('if (!r.ok) return r;') < rd.indexOf('commitHandle(b)'), 'a superseded recovery commits nothing');
  // The handle starts from the page's session and copies it back when it wins.
  assert.match(body('loadHandle'), /adoptSession\(ensureBackend\(\)\.session\)/);
  assert.match(body('commitHandle'), /const page = ensureBackend\(\);[\s\S]*page\.adoptSession\(h\.session\)/);
});

test('a load whose token changed meanwhile does not commit its session (main.js)', async () => {
  const { readFileSync } = await import('node:fs');
  const main = readFileSync(new URL('../../app/static/vr/js/main.js', import.meta.url), 'utf8');
  const i = main.indexOf('function commitHandle(');
  const body = main.slice(i, main.indexOf('\n}\n', i));
  assert.ok(body.indexOf('h.token !== page.token') < body.indexOf('adoptSession'), 'the token is compared before anything changes');
  assert.match(body, /return false;/);
  assert.match(main, /if \(!commitHandle\(b\)\) throw supersededLoad\(\);/);
  assert.match(main, /if \(!commitHandle\(b\)\) return \{ ok: false, superseded: true/);
  // The same rule on Backend objects: a handle made under the old token.
  const { Backend } = await import('../../app/static/vr/js/api.js');
  let page = new Backend('old-token');
  page.adoptSession('s-old');
  const h = new Backend(page.token); h.adoptSession(page.session); h.adoptSession('s-new-under-old');
  page = new Backend('new-token');                        // the token field changed mid-load
  const commit = (hh) => { if (hh.token !== page.token) return false; page.adoptSession(hh.session); return true; };
  assert.equal(commit(h), false);
  assert.equal(page.session, '', 'the new token\'s backend keeps no session of the old token');
});

test('a session handle: a superseded load never moves the shared session', async () => {
  // The pattern main.js follows, on the Backend class itself.
  const { Backend } = await import('../../app/static/vr/js/api.js');
  const page = new Backend('tok');
  page.adoptSession('s-page');
  const loads = new L.LoadTracker();
  const handle = () => { const h = new Backend(page.token); h.adoptSession(page.session); return h; };
  const older = loads.begin('/a.fits'), ha = handle();
  const newer = loads.begin('/b.fits'), hb = handle();
  ha.adoptSession('s-desktop');                 // the older (paired) load adopts another session
  assert.equal(page.session, 's-page', 'not the page\'s yet');
  assert.ok(!loads.isCurrent(older));           // it lost: nothing committed
  if (loads.isCurrent(newer)) page.adoptSession(hb.session);
  assert.equal(page.session, 's-page');
});

test('reopen: another shape is refused, other errors are thrown', async () => {
  const { io } = reopenIo({ gen: 0, shape: [10, 10, 10] });
  const r = await L.reopenCube({ path: 'p', fullShape: [64, 64, 40], gen: 0 }, io);
  assert.equal(r.ok, false);
  assert.match(r.detail, /10×10×10/);
  const bad = { ...io, open: async () => { throw requestError(500, 'boom'); } };
  await assert.rejects(L.reopenCube({ path: 'p', fullShape: [1], gen: 0 }, bad), /500/);
});

test('render: which errors give up, the token as a subprotocol', async () => {
  const R = await import('../../app/static/vr/js/refine.js');
  assert.ok(R.renderUnavailable('503: Server-side rendering not available'));
  assert.ok(R.renderUnavailable('404: Not Found'));
  assert.ok(!R.renderUnavailable("404: Unknown dataset_id 'ds_1'."), 'a gone dataset is reopened, not given up');
  assert.ok(!R.renderUnavailable('network: the backend did not answer'));
  assert.ok(R.subprotocolSafe('AbC-_123.x'));
  assert.ok(!R.subprotocolSafe('has space'));
  assert.ok(!R.subprotocolSafe('a,b'));
  assert.ok(!R.subprotocolSafe(''));
});

// ── Loads: the newest wins, each with its own path ────────────────────────

test('loads: a slower older load never replaces the newer one, and keeps its own path', async () => {
  const loads = new L.LoadTracker();
  const shown = [];
  // The page's load: path captured at the start, checked after every await.
  const load = async (path, wait) => {
    const t = loads.begin(path);
    await new Promise((r) => setTimeout(r, wait));
    if (!loads.isCurrent(t)) throw L.supersededLoad();
    shown.push(t.path);
    return t.path;
  };
  let field = '/data/A.fits';
  const a = load(field, 30);          // A takes longer...
  field = '/data/B.fits';              // the field changes while A loads
  const b = load(field, 5);            // ...and B, started later, finishes first
  assert.equal(await b, '/data/B.fits');
  await assert.rejects(a, (e) => e.superseded === true);
  assert.deepEqual(shown, ['/data/B.fits'], 'A, finishing last, is not shown');
  // A load alone keeps the path it started with, whatever the field says now.
  const c = load('/data/C.fits', 5);
  field = '/data/D.fits';
  assert.equal(await c, '/data/C.fits');
  assert.equal(loads.isCurrent(null), false);
});

test('main.js passes the started path to installCube, never the field', async () => {
  const { readFileSync } = await import('node:fs');
  const main = readFileSync(new URL('../../app/static/vr/js/main.js', import.meta.url), 'utf8');
  const i = main.indexOf('function installCube(');
  const body = main.slice(i, main.indexOf('\n}\n', i));
  assert.doesNotMatch(body, /\$\('path'\)/, 'installCube must not read the path field');
  assert.match(body, /state\.openPath = open \? path : ''/);
  assert.match(main, /installCube\(info, cube, open, ticket\.path\)/);
  // Every backend load checks it is still the newest after its transfer.
  const lr = main.slice(main.indexOf('async function loadReduced('));
  assert.ok(lr.indexOf('stillCurrent(ticket)') < lr.indexOf('installCube('));
});

// ── A check that hangs ────────────────────────────────────────────────────

test('a check that never answers ends, the next one is scheduled, and Retry asks again', async () => {
  const timers = fakeTimers();
  const events = [];
  let calls = 0;
  // A fetch that never settles and ignores its signal: the worst case.
  const life = new L.Lifecycle({ timers, page: { build: 'b', api: 1 }, probeTimeoutMs: 5000,
    fetchInfo: () => { calls++; return new Promise(() => {}); }, onChange: (ev) => events.push(ev) });
  life.start();
  assert.equal(calls, 1);
  assert.ok(life.inFlight, 'one check pending');
  // Retry while it hangs: cancelled, and a new request made at once.
  const retried = life.retryNow();
  await new Promise((r) => setImmediate(r));
  assert.equal(calls, 2, 'Retry made a new request');
  assert.equal(events.length, 0, 'the cancelled check reports nothing (no backoff counted)');
  // The guard of the new check ends it: "nothing answered", the next check scheduled.
  const guard = timers.q.find((h) => h.ms === 5000);
  assert.ok(guard, 'the check has a deadline');
  timers.q.splice(timers.q.indexOf(guard), 1);
  guard.f();
  await retried;
  assert.equal(life.state, 'down');
  assert.equal(events.at(-1).status, 0);
  assert.equal(life.inFlight, null);
  assert.ok(timers.q.some((h) => h.ms === 1000), 'the next check is scheduled (backoff 1 s)');
  // And it is a real check: firing it asks again.
  const next = timers.q.find((h) => h.ms === 1000);
  timers.q.splice(timers.q.indexOf(next), 1);
  next.f();
  await new Promise((r) => setImmediate(r));
  assert.equal(calls, 3);
  life.stop();
});

test('Retry with nothing pending checks at once; the signal reaches fetchInfo', async () => {
  const timers = fakeTimers();
  const signals = [];
  const life = new L.Lifecycle({ timers, page: { build: 'b', api: 1 },
    fetchInfo: async ({ signal }) => { signals.push(signal); return { build: 'b', vr_api: 1, vr_api_min: 1, instance: 'i', token_ok: true }; } });
  await life.retryNow();
  assert.equal(signals.length, 1);
  assert.ok(signals[0] && typeof signals[0].aborted === 'boolean');
  assert.equal(life.state, 'ok');
  assert.ok(!timers.q.some((h) => h.ms === 5000), 'the deadline is cleared once answered');
  life.stop();
});

test('requests: /v1/vr/info and a recovery handle give up on a hung connection', async () => {
  const realFetch = globalThis.fetch;
  const seen = [];
  requestHooks.onFailure = (e, path) => seen.push([e.message, path]);
  try {
    // 1. Nothing answers (the request hangs until aborted).
    globalThis.fetch = (url, init) => new Promise((_, reject) => {
      init.signal.addEventListener('abort', () => reject(new DOMException('aborted', 'AbortError')));
    });
    const b = new Backend('tok');
    const t0 = Date.now();
    await assert.rejects(b.info({ timeoutMs: 40 }), (e) => e.network === true && /in time/.test(e.message));
    assert.ok(Date.now() - t0 < 1000);
    // 2. The headers come, the body never does (and the fetch does not abort it).
    globalThis.fetch = async () => ({ ok: true, status: 200, json: () => new Promise(() => {}), text: () => new Promise(() => {}) });
    await assert.rejects(b.info({ timeoutMs: 40 }), (e) => e.network === true);
    // 3. A recovery handle's requests (datasets/open, sessions/new, reduced_info) have the same deadline.
    b.timeoutMs = 40;
    await assert.rejects(b._json('POST', '/v1/datasets/open', { path: 'x' }), (e) => e.network === true);
    await assert.rejects(b.newSession(), (e) => e.network === true);
    await assert.rejects(b.reducedInfo({ dataset_id: 'd' }), (e) => e.network === true);
    // 4. Cancelled by the page (Retry): an aborted error, not reported as a failure.
    globalThis.fetch = (url, init) => new Promise((_, reject) => {
      init.signal.addEventListener('abort', () => reject(new DOMException('aborted', 'AbortError')));
    });
    const ctl = new AbortController();
    const p = b.info({ signal: ctl.signal, timeoutMs: 10000 });
    const n = seen.length;
    ctl.abort('retry');
    await assert.rejects(p, (e) => e.aborted === true);
    assert.equal(seen.length, n, 'a cancelled check is not a failure');
    // 5. A page load has no deadline by default (a large cube may take long to open).
    assert.equal(new Backend('t').timeoutMs, 0);
  } finally {
    globalThis.fetch = realFetch;
    requestHooks.onFailure = null;
  }
});

test('main.js gives the recovery its deadline and passes the signal to the check', async () => {
  const { readFileSync } = await import('node:fs');
  const main = readFileSync(new URL('../../app/static/vr/js/main.js', import.meta.url), 'utf8');
  const i = main.indexOf('async function recoverDataset(');
  const body = main.slice(i, main.indexOf('\n}\n', i));
  assert.ok(body.indexOf('b.timeoutMs = RECOVERY_TIMEOUT_MS') < body.indexOf('reopenCube('));
  assert.match(main, /publicInfo: \(opts\) => new Backend\(''\)\.info\(opts\)/);
  const shell = readFileSync(new URL('../../app/static/vr/js/shell.js', import.meta.url), 'utf8');
  assert.match(shell, /fetchInfo: \(opts\) => .*\.info\(opts\)/);
});
