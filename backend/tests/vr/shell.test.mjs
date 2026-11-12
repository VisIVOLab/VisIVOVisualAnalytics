// The shell's automatic reopen of the cube (epic #166): node --test backend/tests/vr/shell.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import { createShell } from '../../app/static/vr/js/shell.js';
import { PAGE_VR_API, keepRecoveryPending } from '../../app/static/vr/js/lifecycle.js';
import { requestError } from '../../app/static/vr/js/api.js';

const flush = async (n = 10) => { for (let i = 0; i < n; i++) await new Promise((r) => setTimeout(r, 0)); };

/** A shell with no DOM, no backend and a clock the test moves. `answers` are
 *  what each ctx.recoverDataset call does, in turn: a value to resolve with,
 *  or an Error to throw. */
function shellWith(answers, t) {
  let now = 1_000_000;
  const realNow = Date.now;
  Date.now = () => now;
  t.after(() => { Date.now = realNow; });
  const calls = [];
  const doc = { createElement: () => ({ getContext: () => null }), getElementById: () => null };
  const win = {
    location: { hostname: 'localhost', protocol: 'http:', host: 'localhost', pathname: '/vr/' },
    navigator: { userAgent: 'node' },
    addEventListener() {}, setInterval() {},
  };
  const ctx = {
    inVR: () => false, exitVR() {}, enterVR() {}, onGuideSeen() {}, onRetry() {}, summary: () => ({}),
    hasBackendCube: () => true, loading: () => false, loadGen: () => 1,
    recoverDataset: async (reason) => {
      calls.push(reason);
      const a = answers.shift();
      if (a instanceof Error) throw a;
      return a;
    },
  };
  const shell = createShell({ page: { build: 't', api: PAGE_VR_API }, renderer: () => null,
    backend: () => null, token: () => '', ctx, doc, win });
  // A check of the lifecycle: what Lifecycle tells the shell after each answer.
  const check = async (extra = {}) => {
    shell.life.onChange({ state: 'ok', prev: 'ok', info: { instance: 'b' }, ...extra });
    await flush();
  };
  return { shell, ctx, calls, check, advance: (ms) => { now += ms; } };
}

test('a reopen that fails for now is tried again at a later healthy check, after the throttle', async (t) => {
  const s = shellWith([requestError(0, 'the backend did not answer in time'), { ok: true, detail: 'dataset d2' }], t);
  await s.check({ restarted: true });
  assert.deepEqual(s.calls, ['restarted'], 'the restart reopens the cube');
  s.advance(5000);
  await s.check();
  assert.equal(s.calls.length, 1, 'not before the throttle');
  s.advance(6000);
  await s.check();
  assert.deepEqual(s.calls, ['restarted', 'restarted'], 'tried again after the timeout');
  s.advance(20000);
  await s.check();
  assert.equal(s.calls.length, 2, 'a success clears it');
});

test('a 5xx or a busy backend keeps the reopen pending too', async (t) => {
  const s = shellWith([requestError(502, 'bad gateway'), requestError(503, 'busy'), { ok: true }], t);
  await s.check({ restarted: true });
  s.advance(11000);
  await s.check();
  s.advance(11000);
  await s.check();
  assert.equal(s.calls.length, 3);
  s.advance(11000);
  await s.check();
  assert.equal(s.calls.length, 3);
});

test('a changed file waits for the user, and a superseded reopen leaves nothing pending', async (t) => {
  // PR #200: a file whose revision changed is opened again by the user only.
  const changed = { ok: false, detail: 'The file changed, or its revision cannot be verified: open it again to reload its voxels and coordinates.' };
  const s = shellWith([changed], t);
  await s.check({ restarted: true });
  s.advance(11000);
  await s.check();
  assert.equal(s.calls.length, 1);

  const u = shellWith([requestError(0, 'the backend did not answer'), { ok: false, superseded: true }], t);
  await u.check({ restarted: true });
  u.advance(11000);
  await u.check();
  assert.equal(u.calls.length, 2);
  u.advance(11000);
  await u.check();
  assert.equal(u.calls.length, 2);
});

test('a user reopen that fails for now is retried under the throttle, not as the user', async (t) => {
  const s = shellWith([requestError(500, 'boom'), { ok: true }], t);
  await s.shell.recover('user');
  await flush();
  await s.check();
  assert.equal(s.calls.length, 1, 'the throttle holds');
  s.advance(11000);
  await s.check();
  assert.deepEqual(s.calls, ['user', 'dataset-gone']);
});

test('a failure that settles after a load replaced the cube leaves nothing pending', async (t) => {
  let fail;
  const s = shellWith([], t);
  let gen = 1;
  s.ctx.loadGen = () => gen;
  s.ctx.recoverDataset = (reason) => { s.calls.push(reason); return new Promise((_, rej) => { fail = rej; }); };
  const p = s.shell.recover('dataset-gone');
  gen = 2;                                       // a new cube loaded meanwhile
  fail(requestError(500, 'the old reopen failed'));
  const r = await p;
  assert.equal(r.superseded, true);
  s.advance(11000);
  await s.check();
  assert.equal(s.calls.length, 1, 'the new cube is not reopened');
});

test('a reopen that needs the user cancels a retry left pending before it', async (t) => {
  const changed = { ok: false, detail: 'The file changed, or its revision cannot be verified: open it again to reload its voxels and coordinates.' };
  const s = shellWith([requestError(500, 'boom'), changed], t);
  await s.shell.recover('dataset-gone');         // pending: a 5xx
  await s.shell.recover('user');                 // the user's: the file changed
  s.advance(11000);
  await s.check();
  assert.deepEqual(s.calls, ['dataset-gone', 'user'], 'no automatic retry after it');
});

test('which failures keep the reopen pending', () => {
  assert.equal(keepRecoveryPending({ ok: true }, ''), false);
  assert.equal(keepRecoveryPending({ ok: false, superseded: true }, 'network'), false);
  for (const kind of ['auth', 'network', 'busy', 'server']) assert.equal(keepRecoveryPending({ ok: false }, kind), true, kind);
  for (const kind of ['', 'other', 'dataset-gone', 'session-gone']) assert.equal(keepRecoveryPending({ ok: false }, kind), false, kind);
});
