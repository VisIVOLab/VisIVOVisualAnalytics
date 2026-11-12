// The reduced-cube requests of the VR page's Backend (part of #166), against a
// fake fetch: a pyramid build (202) is an error marked `preparing`, never a
// description; deadlines end as `timedOut`; a request is resent once only after
// a failure before it went out, and only a failure that stays is reported.
// node --test backend/tests/vr/api-reduced.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';

globalThis.window = { location: { pathname: '/vr/', protocol: 'http:', host: 'localhost:8000' } };
const api = await import('../../app/static/vr/js/api.js');
const { reopenCube } = await import('../../app/static/vr/js/lifecycle.js');

function respond(status, body, headers = {}) {
  const text = typeof body === 'string' ? body : JSON.stringify(body);
  return {
    ok: status >= 200 && status < 300, status,
    headers: { get: (k) => headers[k] ?? null },
    text: async () => text,
  };
}

/** A fetch that never answers until its signal aborts it, as a browser's. */
function hangingFetch(calls) {
  return (url, init) => new Promise((_, reject) => {
    calls.push(url);
    const sig = init && init.signal;
    if (sig) sig.addEventListener('abort', () => reject(new DOMException('aborted', 'AbortError')), { once: true });
  });
}

const failures = [];
api.requestHooks.onFailure = (e, path) => failures.push([e.message, path]);

test('a pyramid being built (202) is a `preparing` error, not the cube\'s description', async () => {
  globalThis.fetch = async () => respond(202, { valid: false, preparing: true, progress: 0.4 });
  const b = new api.Backend('t');
  await assert.rejects(b.reducedInfo({ dataset_id: 'd' }), (e) => e.preparing === true && e.progress === 0.4);
  globalThis.fetch = async () => respond(200, { full_shape: [8, 8, 4], factors: [1, 1, 1] });
  assert.deepEqual((await b.reducedInfo({ dataset_id: 'd' })).full_shape, [8, 8, 4]);
});

test('a reduced_info that never answers ends at its deadline as `timedOut`, sent once', async () => {
  const calls = [];
  globalThis.fetch = hangingFetch(calls);
  failures.length = 0;
  const b = new api.Backend('t');
  const t0 = Date.now();
  await assert.rejects(b.reducedInfo({ dataset_id: 'd' }, { timeoutMs: 60 }),
    (e) => e.network === true && e.timedOut === true && /in time/.test(e.message));
  assert.ok(Date.now() - t0 < 2000);
  assert.equal(calls.length, 1, 'a timeout is not resent');
  assert.equal(failures.length, 1, 'and is reported once, to the lifecycle');
});

test('reduced_info has no deadline of its own on a page load (opening a large cube can take long)', async () => {
  const calls = [];
  globalThis.fetch = hangingFetch(calls);
  const b = new api.Backend('t');
  let settled = false;
  b.reducedInfo({ dataset_id: 'd' }).then(() => { settled = true; }, () => { settled = true; });
  await new Promise((r) => setTimeout(r, 150));
  assert.equal(settled, false);
  assert.equal(calls.length, 1);
});

test('a failure before the request went out is resent; reported only if it stays', async () => {
  failures.length = 0;
  let n = 0;
  globalThis.fetch = async () => { if (n++ === 0) throw new TypeError('Failed to fetch'); return respond(200, { factors: [1, 1, 1] }); };
  const b = new api.Backend('t');
  assert.deepEqual((await b.reducedInfo({ dataset_id: 'd' })).factors, [1, 1, 1]);
  assert.equal(n, 2);
  assert.equal(failures.length, 0, 'a failure the resend recovered is not a backend-down notice');
  n = 0;
  globalThis.fetch = async () => { n++; throw new TypeError('Failed to fetch'); };
  await assert.rejects(b.reducedInfo({ dataset_id: 'd' }), (e) => e.network === true);
  assert.equal(n, 2);
  assert.equal(failures.length, 1, 'the failure that stays is reported once');
});

test('retryOnNetworkError: hookedFetch-shaped errors and timeouts', async () => {
  const net = Object.assign(new Error('network: the backend did not answer'), { network: true, cause: new TypeError('Failed to fetch') });
  const late = Object.assign(new Error('network: the backend did not answer in time'), { network: true, timedOut: true, cause: new DOMException('x', 'AbortError') });
  let n = 0;
  assert.equal(await api.retryOnNetworkError(async () => { if (n++ === 0) throw net; return 'ok'; }, { waitMs: 1 }), 'ok');
  n = 0;
  await assert.rejects(api.retryOnNetworkError(async () => { n++; throw late; }, { waitMs: 1 }), (e) => e.timedOut);
  assert.equal(n, 1);
});

test('the reduced cube\'s deadline covers its headers, ends as `timedOut` and is reported', async () => {
  const calls = [];
  globalThis.fetch = hangingFetch(calls);
  failures.length = 0;
  const b = new api.Backend('t');
  await assert.rejects(b.reducedCube({ dataset_id: 'd' }, null, 60),
    (e) => e.timedOut === true && /no reduced cube within/.test(e.message));
  assert.equal(calls.length, 1);
  assert.equal(failures.length, 1);
});

test('a recovery that meets a pyramid build waits for it instead of reporting a wrong shape', async () => {
  const r = await reopenCube({ path: '/c.fits', fullShape: [8, 8, 4], fileRevision: 'r', gen: 1 }, {
    open: async () => ({ dataset_id: 'd2' }),
    newSession: async () => {},
    reducedInfo: async () => { throw api.preparingError(0.25); },
    classify: () => 'other',
    loadGen: () => 1,
  });
  assert.equal(r.ok, false);
  assert.equal(r.preparing, true);
  assert.match(r.detail, /preparing the cube's brick pyramid \(25 %\)/);
  assert.doesNotMatch(r.detail, /shape/);
});
