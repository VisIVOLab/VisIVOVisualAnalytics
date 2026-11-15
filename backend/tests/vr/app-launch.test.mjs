// The headset app (issue #255): the launch ticket, the room in VR, the page
// in VR that never stands down, and how main.js wires them.
// node --test backend/tests/vr/app-launch.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import * as APP from '../../app/static/vr/js/app-launch.js';
import * as XE from '../../app/static/vr/js/xr-entry.js';
import * as H from '../../app/static/vr/js/headset.js';
import { SinglePage, stoodDownText, theyWin, PINNED_ENTERING, PINNED_IN_VR } from '../../app/static/vr/js/single-page.js';
import { parsePairing, strippedAddress } from '../../app/static/vr/js/pairing.js';
import { noticeFor } from '../../app/static/vr/js/lifecycle.js';

const main = readFileSync(new URL('../../app/static/vr/js/main.js', import.meta.url), 'utf8');
const TICKET = 'Qx7v3sQ1-a_bZ9kLmN0pRsTuVwXyZ012345678abcd';
const GRANT = { token: 'tok-123456789', session: 'sess-abcdef', launch: 'Ln_0a1b2c3d4',
  cube: { dataset_id: 'ds-1', path: '/data/ngc3137.fits', kind: 'cube', box: [[0, 9], [0, 9], [2, 5]] } };

// ── The launch ──────────────────────────────────────────────────────────────

test('the app\'s launches: launch=immersive, an installed app too; the way out is quitting', () => {
  assert.equal(APP.launchedImmersive('?launch=immersive'), true);
  assert.equal(APP.launchedImmersive('?launch=app'), false);
  assert.equal(APP.launchedImmersive(''), false);
  assert.equal(XE.launchedAsApp('?launch=immersive', null), true);
  assert.equal(APP.exitLabel(true), 'Quit VisIVO VR');
  assert.equal(APP.exitLabel(false), 'Exit VR');
});

test('the ticket rides the fragment and leaves the address with the pairing keys', () => {
  const p = parsePairing(`#ticket=${TICKET}`);
  assert.equal(p.ticket, TICKET);
  assert.equal(p.present, true);
  assert.equal(strippedAddress('/vr/', '?launch=immersive', `#ticket=${TICKET}&keep=1`), '/vr/?launch=immersive#keep=1');
  assert.equal(APP.cleanTicket(` ${TICKET} `), TICKET);
  for (const bad of ['', 'short', 'x'.repeat(200), 'a b c'.repeat(10), null]) assert.equal(APP.cleanTicket(bad), '');
});

test('a grant is checked before it is used', () => {
  assert.deepEqual(APP.validGrant(GRANT), GRANT);
  assert.deepEqual(APP.validGrant({ ...GRANT, cube: null }), { ...GRANT, cube: null });
  assert.equal(APP.validGrant({ ...GRANT, cube: { ...GRANT.cube, kind: undefined } }).cube.kind, 'cube');
  for (const bad of [null, 'x', { ...GRANT, token: '' }, { ...GRANT, session: 5 }, { ...GRANT, launch: 'bad launch' },
    { ...GRANT, cube: { dataset_id: 'd' } }, { ...GRANT, cube: 'nope' }]) {
    assert.equal(APP.validGrant(bad), null, JSON.stringify(bad));
  }
});

/** A fetch stand-in recording its calls, answering with `status` and `body`. */
function fakeFetch(status, body, { reject = false } = {}) {
  const calls = [];
  const fetch = async (url, init) => {
    calls.push({ url, init });
    if (reject) throw new TypeError('Failed to fetch');
    return { status, ok: status >= 200 && status < 300, json: async () => body };
  };
  return { fetch, calls };
}

test('the exchange: the ticket in a JSON body, never in the address; the grant back', async () => {
  const f = fakeFetch(200, GRANT);
  const grant = await APP.exchangeTicket(TICKET, { fetch: f.fetch, url: '/v1/vr/launch/exchange' });
  assert.deepEqual(grant, GRANT);
  assert.equal(f.calls.length, 1);
  const { url, init } = f.calls[0];
  assert.equal(url, '/v1/vr/launch/exchange');
  assert.ok(!url.includes(TICKET));
  assert.equal(init.method, 'POST');
  assert.equal(init.headers['Content-Type'], 'application/json');
  assert.deepEqual(JSON.parse(init.body), { ticket: TICKET });
  assert.equal(init.cache, 'no-store');
  assert.equal(init.credentials, 'omit');
});

test('the exchange\'s failures say what to do', async () => {
  const kinds = [];
  for (const [status, body, opts] of [[404, { detail: 'x' }], [429, {}], [500, {}], [200, { nope: 1 }], [0, null, { reject: true }]]) {
    try {
      await APP.exchangeTicket(TICKET, { fetch: fakeFetch(status, body, opts).fetch, url: '/x' });
      assert.fail('resolved');
    } catch (err) {
      kinds.push(err.kind);
      assert.ok(!String(err.message).includes(TICKET), 'the ticket is never in an error');
    }
  }
  assert.deepEqual(kinds, ['expired', 'busy', 'failed', 'failed', 'network']);
  // A malformed ticket is never sent.
  const f = fakeFetch(200, GRANT);
  await assert.rejects(APP.exchangeTicket('bad', { fetch: f.fetch, url: '/x' }), (e) => e.kind === 'expired');
  assert.equal(f.calls.length, 0);
});

test('the exchange\'s deadline covers the body too', async () => {
  let fire = null;
  const fetch = async () => ({ status: 200, ok: true, json: () => new Promise(() => {}) });
  const p = APP.exchangeTicket(TICKET, { fetch, url: '/x', timeoutMs: 10, setTimer: (f) => { fire = f; return 1; }, clearTimer: () => {} });
  await new Promise((r) => setImmediate(r));
  fire();
  await assert.rejects(p, (e) => e.kind === 'network');
});

test('the exchange has a deadline', async () => {
  let fire = null;
  const fetch = (url, init) => new Promise((_, reject) => init.signal.addEventListener('abort', () => reject(new Error('aborted'))));
  const p = APP.exchangeTicket(TICKET, { fetch, url: '/x', timeoutMs: 10, setTimer: (f) => { fire = f; return 1; }, clearTimer: () => {} });
  fire();
  await assert.rejects(p, (e) => e.kind === 'network');
});

// ── The room in VR ──────────────────────────────────────────────────────────

test('the room says what the app waits for, and nothing once a cube is on show', () => {
  const base = { immersive: true, exchanging: false, error: null, connected: false, loading: false, shown: false, name: '', origin: 'localhost:18900' };
  assert.equal(APP.roomState({ ...base, immersive: false }), null);
  assert.equal(APP.roomState({ ...base, shown: true, loading: true }), null);
  assert.equal(APP.roomState({ ...base, exchanging: true }).kind, 'app-loading');
  assert.deepEqual(APP.roomState({ ...base, connected: true, loading: true, name: 'ngc3137.fits' }),
    { kind: 'app-loading', ctx: { name: 'ngc3137.fits', origin: 'localhost:18900' } });
  assert.deepEqual(APP.roomState(base), { kind: 'app-empty', ctx: { connected: false, origin: 'localhost:18900' } });
  assert.equal(APP.roomState({ ...base, connected: true }).ctx.connected, true);
  const failed = APP.roomState({ ...base, error: { kind: 'expired', message: 'm' } });
  assert.equal(failed.kind, 'app-failed');
  assert.equal(failed.ctx.why, 'expired');
  assert.equal(APP.originLabel({ host: 'localhost:18900' }), 'localhost:18900');
});

test('the room\'s notices: in front, one card, the way out labelled as quitting', () => {
  const ctx = { inVR: true, app: true, origin: 'localhost:18900' };
  for (const kind of ['app-loading', 'app-empty', 'app-failed']) {
    const n = noticeFor(kind, ctx);
    assert.equal(n.id, 'app-room', kind);
    assert.ok(!n.sticky && !n.autoHideMs, `${kind} stays in front until the page clears it`);
    assert.deepEqual(n.actions, [{ id: 'exit-vr', label: 'Quit VisIVO VR' }], kind);
  }
  assert.match(noticeFor('app-empty', ctx).lines.join(' '), /localhost:18900/);
  assert.match(noticeFor('app-empty', ctx).lines.join(' '), /Open on Headset/);
  assert.match(noticeFor('app-empty', { ...ctx, connected: true }).lines.join(' '), /Show on Headset/);
  assert.match(noticeFor('app-loading', { ...ctx, name: 'a.fits' }).title, /a\.fits/);
  assert.match(noticeFor('app-failed', { ...ctx, why: 'expired' }).title, /expired/);
  // Every notice offering the way out says Quit in the app, Exit VR elsewhere.
  assert.deepEqual(noticeFor('unreachable', { inVR: true, app: true }).actions.find((a) => a.id === 'exit-vr'), { id: 'exit-vr', label: 'Quit VisIVO VR' });
  assert.deepEqual(noticeFor('unreachable', { inVR: true }).actions.find((a) => a.id === 'exit-vr'), { id: 'exit-vr', label: 'Exit VR' });
  // A cube that cannot be shown in the app's session never says "shown when you leave VR".
  const waiting = noticeFor('cube-waiting', { inVR: true, app: true, detail: 'x' });
  assert.doesNotMatch(`${waiting.title} ${waiting.lines.join(' ')}`, /when you leave VR|Enter VR again/);
});

// ── Entering at load ────────────────────────────────────────────────────────

test('the app enters VR before its cube, once its launch is settled', () => {
  const base = { launched: true, tried: false, vrSupported: true, active: true, inXR: false, cubeReady: false, immersive: true, settled: true };
  assert.equal(XE.shouldAutoEnter(base), true);
  assert.equal(XE.shouldAutoEnter({ ...base, settled: false }), false, 'the ticket is being exchanged');
  assert.equal(XE.shouldAutoEnter({ ...base, immersive: false }), false, 'an installed app still waits for a cube');
  for (const k of ['tried', 'inXR', 'busy']) assert.equal(XE.shouldAutoEnter({ ...base, [k]: true }), false, k);
  assert.equal(XE.shouldAutoEnter({ ...base, active: false }), false);
});

test('main.js: the ticket is exchanged at start; the capacity is set before the session (#229); no offers in the app', () => {
  const start = main.slice(main.indexOf('// ── Start ──'));
  assert.match(start, /else if \(pairing\.ticket\) exchangeLaunch\(\);/);
  assert.ok(start.indexOf('ensureCapacity(null);') < start.indexOf('exchangeLaunch()'), 'capacity at start, before anything');
  // The auto-entry goes through enterXR, which sets the capacity before requestSession.
  const auto = main.slice(main.indexOf('function maybeAutoEnter()'), main.indexOf('function maybeOffer()'));
  assert.match(auto, /immersive: entry\.immersive, settled: appLaunch\.settled && performance\.now\(\) - appLaunch\.claimedAt > ARBITRATION_MS/);
  assert.ok(auto.indexOf('single.setPinned(PINNED_ENTERING)') < auto.indexOf("enterXR('immersive-vr', false)"), 'pinned before the session is asked for');
  // The other pages have answered the claim before the app asks for its session.
  const start2 = main.slice(main.indexOf('// ── Start ──'));
  assert.match(start2, /single\.claim\(\);[^\n]*\nappLaunch\.claimedAt = performance\.now\(\);/);
  const enter = main.slice(main.indexOf('async function enterXR('), main.indexOf('async function startSession('));
  assert.ok(enter.indexOf('ensureCapacity(state.cube, { page: true })') < enter.indexOf('navigator.xr.requestSession('));
  const offer = main.slice(main.indexOf('function maybeOffer()'), main.indexOf('navigator.xr.offerSession('));
  assert.match(offer, /if \(entry\.immersive\) return;/);
});

test('main.js: a used ticket is gone; the grant becomes the page\'s; the hello names the launch', () => {
  const ex = main.slice(main.indexOf('async function exchangeLaunch()'), main.indexOf('function adoptGrant('));
  assert.match(ex, /appLaunch\.ticket = '';\s+pairing\.ticket = '';/);
  assert.match(ex, /finally \{\s+appLaunch\.exchanging = false;\s+appLaunch\.settled = true;/);
  assert.doesNotMatch(ex, /shell\.log\([^)]*ticket\b(?! exchanged)/, 'the ticket itself is never logged');
  const adopt = main.slice(main.indexOf('function adoptGrant('), main.indexOf('function forwardGrant('));
  // A handed grant in another session: loads of the old one superseded first,
  // a cube on show opened again there when the launch has none.
  assert.ok(adopt.indexOf("if (handed && before && before !== grant.session) loads.begin('');") < adopt.indexOf('ensureBackend().adoptSession(grant.session)'));
  assert.match(adopt, /showFromDesktop\(\{ seq: 0, type: 'show', dataset_id: '', path: state\.openPath/);
  assert.match(adopt, /loadLinked\(\{ session: grant\.session, dataset: '', path, kind, box, \.\.\.sky \}\)/, 'a point table or a velocity field follows too');
  assert.match(adopt, /if \(!single\.active\) \{ forwardGrant\(\); return; \}/);
  assert.match(adopt, /saveToken\(grant\.token, false\)/);
  assert.match(adopt, /hub\.rehello\(\)/);
  assert.match(main, /launch: \(\) => appLaunch\.id,/);
  assert.match(main, /immersiveApp: \(\) => APP_IMMERSIVE,/);
});

test('main.js: a page that stands down for the app in VR hands its launch over; none ends a session for another', () => {
  const down = main.slice(main.indexOf('function onSingleChange('), main.indexOf('function takeOver()'));
  assert.match(down, /if \(info\.reason === 'vr'\) forwardGrant\(\);/);
  const fwd = main.slice(main.indexOf('function forwardGrant()'), main.indexOf('function onPageMessage('));
  assert.match(fwd, /!by\.pinned\) return;/);
  assert.match(fwd, /single\.send\(by\.id, APP\.grantMessage\(by\.id, g\)\)/);
  const tick = main.slice(main.indexOf('function appTick()'), main.indexOf("$('enter-vr').addEventListener"));
  assert.match(tick, /single\.setPinned\(xr \? PINNED_IN_VR : R\.xrActive \|\| entering \? PINNED_ENTERING : 0\)/);
  // The app's code never ends a session nor asks for another itself.
  const app = main.slice(main.indexOf('// ── The headset app (issue #255)'), main.indexOf("$('enter-vr').addEventListener"));
  assert.doesNotMatch(app, /endXR\(|\.end\(\)|requestSession|enterXR\(/);
});

// ── One page: the app in VR never stands down ───────────────────────────────

/** BroadcastChannel stand-ins delivering at once, in order. */
function bus() {
  const members = new Set();
  return () => {
    const ch = {
      onmessage: null,
      postMessage(m) { for (const o of members) if (o !== ch && o.onmessage) o.onmessage({ data: structuredClone(m) }); },
      close() { members.delete(ch); },
    };
    members.add(ch);
    return ch;
  };
}

test('a pinned page (the app in VR) never stands down; the newer page does, reason vr', () => {
  const make = bus();
  let t = 1000;
  const now = () => t;
  const events = [];
  const old = new SinglePage({ channel: make(), id: 'old', now, content: true, onChange: (a, i) => events.push(['old', a, i]) });
  old.claim();
  old.setPinned(PINNED_IN_VR);
  t = 2000;
  const got = [];
  const fresh = new SinglePage({ channel: make(), id: 'new', now, content: true, onChange: (a, i) => events.push(['new', a, i]) });
  fresh.claim();
  assert.equal(old.active, true, 'the page in VR stays');
  assert.equal(fresh.active, false);
  assert.equal(events.length, 1);
  assert.deepEqual(events[0].slice(0, 2), ['new', false]);
  assert.equal(events[0][2].reason, 'vr');
  assert.equal(fresh.by.pinned, true);
  assert.equal(stoodDownText({ reason: 'vr' }).title, 'VisIVO VR is already in VR');
  // Even the user's "Use this window" does not take the app's VR away.
  fresh.claim({ force: true });
  assert.equal(old.active, true);
  assert.equal(fresh.active, false);
  // The stood-down page's message reaches the page it names, and only it.
  old.onMessage = (m) => got.push(['old', m]);
  const third = new SinglePage({ channel: make(), id: 'third', now, onMessage: (m) => got.push(['third', m]) });
  third.claim();
  fresh.send('old', APP.grantMessage('old', GRANT));
  assert.equal(got.length, 1);
  assert.equal(got[0][0], 'old');
  assert.deepEqual(APP.grantFromMessage(got[0][1], 'old'), GRANT);
  assert.equal(APP.grantFromMessage(got[0][1], 'third'), null);
  assert.equal(APP.grantFromMessage({ ...got[0][1], grant: { token: '' } }, 'old'), null);
  assert.equal(third.active, false, 'a page with nothing stands down for the one in VR');
});

test('a page in VR outranks one entering; two entering pages keep the usual order; one in VR never stands down', () => {
  const inVR = { t: 1, id: 'a', content: true, pinned: PINNED_IN_VR };
  const entering = { t: 2, id: 'b', content: true, pinned: PINNED_ENTERING };
  assert.equal(theyWin(entering, inVR), true, 'the page entering gives way to the one in VR');
  assert.equal(theyWin(inVR, entering), false);
  assert.equal(theyWin(inVR, { ...inVR, t: 5, id: 'z' }), false, 'a page in VR never stands down');
  assert.equal(theyWin({ ...entering, t: 1, id: 'a' }, entering), true, 'between two entering pages the newer wins');
  // Over the channel: a newer page that already asked for its session still gives way.
  const make = bus();
  const old = new SinglePage({ channel: make(), id: 'old', now: () => 1, content: true });
  old.claim();
  old.setPinned(PINNED_IN_VR);
  const fresh = new SinglePage({ channel: make(), id: 'new', now: () => 2, content: true });
  fresh.setPinned(PINNED_ENTERING);
  fresh.claim();
  assert.equal(old.active, true);
  assert.equal(fresh.active, false);
});

test('pinned ranks above content and the user\'s choice; unpinned pages keep the old order', () => {
  assert.equal(theyWin({ t: 2, id: 'b', content: true, held: true, pinned: false }, { t: 1, id: 'a', content: false, pinned: true }), true);
  assert.equal(theyWin({ t: 1, id: 'a', content: false, pinned: true }, { t: 2, id: 'b', content: true, held: true }), false);
  assert.equal(theyWin({ t: 1, id: 'a', content: true }, { t: 2, id: 'b', content: true }), true);
});

// ── The hub's hello names the launch ────────────────────────────────────────

test('the hello names the app\'s launch, and says it again when a handed launch changes it', async () => {
  const calls = [];
  let launch = '';
  const resp = (status, body) => ({ status, text: async () => JSON.stringify(body) });
  const fetch = async (url, init = {}) => {
    const u = new URL(url, 'http://h');
    const body = init.body ? JSON.parse(init.body) : null;
    calls.push({ path: u.pathname, body });
    if (u.pathname === '/v1/vr/headset/hello') return resp(200, { client: body.client, server_time: 1, instance: 'i' });
    if (u.pathname === '/v1/vr/headset/commands') {
      await new Promise((res) => init.signal && init.signal.addEventListener('abort', res));
      throw Object.assign(new Error('aborted'), { name: 'AbortError' });
    }
    return resp(200, {});
  };
  const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((r) => setImmediate(r)); };
  const hub = H.createHub({ client: 'vr-app', transport: 'adb', label: 'Quest 3', auth: () => ({ token: 'tok', session: 's' }),
    launch: () => launch, onCommand: () => {}, log: () => {}, fetch, setTimeout: (f, ms) => setTimeout(f, Math.min(ms, 5)), clearTimeout });
  hub.start();
  await flush();
  const hellos = () => calls.filter((c) => c.path === '/v1/vr/headset/hello');
  assert.deepEqual(hellos()[0].body, { client: 'vr-app', transport: 'adb', label: 'Quest 3' });
  launch = 'Ln_0a1b2c3d4';
  hub.rehello();
  await flush();
  assert.equal(hellos().length, 2);
  assert.equal(hellos()[1].body.launch, 'Ln_0a1b2c3d4');
  launch = 'bad launch';
  hub.rehello();
  await flush();
  assert.equal(hellos()[2].body.launch, undefined, 'a malformed launch is not sent');
  hub.stop();
});

test('main.js: the app quits with its session, the room is said again on entering VR', () => {
  const start = main.slice(main.indexOf('async function startSession('), main.indexOf('function placeInFront('));
  assert.match(start, /if \(APP_IMMERSIVE\) session\.addEventListener\('end', \(\) => setTimeout\(\(\) => \{ try \{ window\.close\(\); \}/);
  // A page that stood down keeps its window: closing it ends the session of the page in VR.
  const fwd = main.slice(main.indexOf('function forwardGrant()'), main.indexOf('function onPageMessage('));
  assert.doesNotMatch(fwd, /window\.close/);
  const tick = main.slice(main.indexOf('function appTick()'), main.indexOf("$('enter-vr').addEventListener"));
  assert.match(tick, /JSON\.stringify\(\[r, !!xr\]\)/, 'the Quit action is offered once in VR');
  // The app counts as content from its start: it never stands down for a page in a browser tab.
  assert.match(main, /return !!\(APP_IMMERSIVE \|\| state\.cube/);
});
