// Fewer Enter VR presses (issue #237).
// node --test backend/tests/vr/xr-entry.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import * as XE from '../../app/static/vr/js/xr-entry.js';
import { appQuery, appAddress, launchMode } from '../../../web/vr-loader/js/loader-core.js';

const media = (modes) => (q) => ({ matches: modes.some((m) => q === `(display-mode: ${m})`) });

test('launched as an app: the loader says so, or the page is shown standalone', () => {
  assert.equal(XE.launchedAsApp('?transport=pair&launch=app', media([])), true);
  assert.equal(XE.launchedAsApp('?debug', media(['standalone'])), true);
  assert.equal(XE.launchedAsApp('?debug', media(['fullscreen'])), true);
  assert.equal(XE.launchedAsApp('?debug', media(['browser'])), false);
  assert.equal(XE.launchedAsApp('', null), false);
  assert.equal(XE.launchedAsApp('?launch=page', () => { throw new Error('no media queries'); }), false);
});

test('the loader passes the launch on to its iframe, and only it decides', () => {
  assert.equal(launchMode(media(['standalone'])), 'app');
  assert.equal(launchMode(media(['browser'])), '');
  assert.equal(launchMode(undefined), '');
  assert.equal(appQuery('?debug', 'app'), '?transport=pair&debug&launch=app');
  // A launch in the loader's own address is not passed on as it is.
  assert.equal(appQuery('?launch=app&debug'), '?transport=pair&debug');
  assert.equal(appAddress('k3j4l5m6', '', { session: 's' }, 'app'), 'app/k3j4l5m6/vr/?transport=pair&launch=app#session=s');
  const loader = readFileSync(new URL('../../../web/vr-loader/js/loader.js', import.meta.url), 'utf8');
  assert.match(loader, /launch: launchMode\([^\n]*window\.location\.search\)/);
  assert.match(loader, /appAddress\(state\.tag, window\.location\.search, welcome, state\.launch\)/);
});

test('the headset app opens the loader with ?launch=immersive: the page enters VR at load (#383)', () => {
  // The app's own address wins over the display mode (a TWA may also report standalone).
  assert.equal(launchMode(media(['standalone']), '?launch=immersive'), 'immersive');
  assert.equal(launchMode(undefined, '?launch=immersive'), 'immersive');
  assert.equal(launchMode(media(['browser']), '?launch=page'), '');
  assert.equal(appQuery('?launch=immersive&debug', 'immersive'), '?transport=pair&debug&launch=immersive');
  assert.equal(appAddress('k3j4l5m6', '?launch=immersive', { session: 's' }, 'immersive'),
    'app/k3j4l5m6/vr/?transport=pair&launch=immersive#session=s');
  // The page reads it as the immersive app: it enters VR before its cube, with no ticket to wait for.
  assert.equal(XE.launchedAsApp('?transport=pair&launch=immersive', null), true);
  assert.equal(XE.shouldAutoEnter({ launched: true, tried: false, vrSupported: true, active: true, inXR: false,
    cubeReady: false, immersive: true, settled: true }), true);
});

test('auto-enter: once, launched as an app, with a cube on show, on the page in use, outside VR', () => {
  const base = { launched: true, tried: false, vrSupported: true, active: true, inXR: false, cubeReady: true };
  assert.equal(XE.shouldAutoEnter(base), true);
  for (const k of ['launched', 'vrSupported', 'active', 'cubeReady']) assert.equal(XE.shouldAutoEnter({ ...base, [k]: false }), false, k);
  for (const k of ['tried', 'inXR', 'busy']) assert.equal(XE.shouldAutoEnter({ ...base, [k]: true }), false, k);
});

test('a refusal for want of a gesture is the fallback, not an error', () => {
  assert.equal(XE.isActivationRefusal({ name: 'SecurityError', message: 'x' }), true);
  assert.equal(XE.isActivationRefusal({ name: 'NotAllowedError', message: 'x' }), true);
  assert.equal(XE.isActivationRefusal(new Error('requestSession requires user activation')), true);
  assert.equal(XE.isActivationRefusal({ name: 'NotSupportedError', message: 'immersive-vr not supported' }), false);
  assert.equal(XE.isActivationRefusal(null), false);
});

test('offerSession: used only where it exists, asked for (?offer, #291), once at a time', () => {
  assert.equal(XE.canOffer({ offerSession() {} }), true);
  assert.equal(XE.canOffer({ requestSession() {} }), false);
  assert.equal(XE.canOffer(undefined), false);
  assert.equal(XE.offerWanted('?debug&offer'), true);
  assert.equal(XE.offerWanted('?offer=1'), true);
  assert.equal(XE.offerWanted('?debug'), false);
  assert.equal(XE.offerWanted('?offers'), false);
  assert.equal(XE.offerWanted(''), false);
  const base = { offerable: true, wanted: true, vrSupported: true, active: true, inXR: false, offered: false };
  assert.equal(XE.shouldOffer(base), true);
  for (const k of ['offerable', 'wanted', 'vrSupported', 'active']) assert.equal(XE.shouldOffer({ ...base, [k]: false }), false, k);
  for (const k of ['inXR', 'offered', 'stopped']) assert.equal(XE.shouldOffer({ ...base, [k]: true }), false, k);
  // The page shows its own Enter VR: without ?offer it never offers.
  const { wanted, ...unasked } = base;
  assert.equal(wanted, true);
  assert.equal(XE.shouldOffer(unasked), false);
});

test('Enter VR refused while the browser holds an offer (#291): said, with what to do', () => {
  const quest = Object.assign(new Error('Failed to execute \'requestSession\' on \'XRSystem\': There is already an active, immersive XRSession.'), { name: 'InvalidStateError' });
  assert.equal(XE.isOfferConflict(quest, { offerPending: true, inXR: false }), true);
  assert.equal(XE.isOfferConflict(quest, { offerPending: false, inXR: false }), false);
  assert.equal(XE.isOfferConflict(quest, { offerPending: true, inXR: true }), false);
  assert.equal(XE.isOfferConflict({ name: 'NotAllowedError' }, { offerPending: true, inXR: false }), false);
  const c = XE.enterFailure(quest, { offerPending: true });
  assert.equal(c.kind, 'offer-conflict');
  assert.match(c.text, /press the browser's Enter VR/);
  assert.match(c.text, /reload the page/);
  // The heuristic cannot tell the offer from another open session: both said.
  assert.match(c.text, /or another VR session is open/);
  assert.match(c.text, /leave the other VR session/);
  // The browser's Enter VR answered while the page's request was under way.
  const raced = XE.enterFailure(quest, { offerPending: false, offerRaced: true });
  assert.equal(raced.kind, 'offer-raced');
  assert.match(raced.text, /press Enter VR again/);
  assert.equal(XE.enterFailure({ name: 'NotAllowedError', message: 'x' }, { offerRaced: true }).kind, 'activation');
  // Every failure says what to do: a verb of action in each text.
  const cases = [
    [quest, {}, 'busy', /leave it, then press Enter VR/],
    [{ name: 'NotAllowedError', message: 'requires user activation' }, {}, 'activation', /press Enter VR again/],
    [{ name: 'NotAllowedError', message: 'Permission denied' }, {}, 'permission', /allow it/],
    [{ name: 'NotSupportedError', message: 'immersive-vr' }, {}, 'unsupported', /open the page/],
    [new Error('this page is paused: the page in use is in another window'), {}, 'paused', /Use this window/],
    [new Error('a VR session is already running'), {}, 'busy', /leave VR first/],
    [new Error('a session the browser started for this page has not closed yet'), {}, 'stuck', /reload the page/],
    [new Error('the benchmark\'s cubes are not prepared: press Prepare the headset benchmark first'), {}, 'failed', /press Prepare/],
    [{ name: 'OperationError', message: 'XR runtime gone' }, {}, 'failed', /reload the page and press Enter VR/],
  ];
  for (const [err, ctx, kind, re] of cases) {
    const f = XE.enterFailure(err, ctx);
    assert.equal(f.kind, kind, `${err.name}: ${err.message}`);
    assert.match(f.text, re, f.text);
  }
});

test('the XR flag belongs to its session (#305): a refused offer ending late never clears it', () => {
  const seen = [];
  const flag = new XE.SessionFlag((active, owner) => seen.push([active, owner]));
  const offered = { id: 'offered' }, mine = { id: 'mine' };
  assert.equal(flag.active, false);
  // The offer answers while the page's own request is under way: refused, held.
  flag.holdRefused(offered);
  assert.equal(flag.active, true);
  assert.equal(flag.owner, null, 'a refused session never owns the flag');
  // The page's own session starts, then the refused one's end comes.
  flag.own(mine);
  assert.equal(flag.ended(offered), 'refused');
  assert.equal(flag.active, true, 'the page\'s session still runs: no 3-D allocation (#229)');
  assert.equal(flag.owner, mine);
  assert.equal(flag.ended(offered), '', 'an end already seen changes nothing');
  assert.equal(flag.ended({ id: 'stranger' }), '');
  assert.equal(flag.ended(mine), 'owner');
  assert.equal(flag.active, false);
  assert.equal(flag.owner, null);
  assert.deepEqual(seen.at(-1), [false, null]);
  // A refused session alone holds the flag until its end.
  flag.holdRefused(offered);
  assert.equal(flag.active, true);
  assert.equal(flag.ended(offered), 'refused');
  assert.equal(flag.active, false);
  // The owner is never held as refused.
  flag.own(mine);
  flag.holdRefused(mine);
  assert.equal(flag.ended(mine), 'owner');
  assert.equal(flag.active, false);
});

test('the page wires them: ?offer only, the offered session goes through enterXR, the capacity is set before the offer', () => {
  const main = readFileSync(new URL('../../app/static/vr/js/main.js', import.meta.url), 'utf8');
  assert.match(main, /XE\.canOffer\(navigator\.xr\)/);
  assert.match(main, /offerWanted: XE\.offerWanted\(window\.location\.search\)/);
  assert.match(main, /wanted: entry\.offerWanted/);
  assert.match(main, /navigator\.xr\.offerSession\('immersive-vr'/);
  assert.match(main, /enterXR\('immersive-vr', false, session\)/);
  const offer = main.slice(main.indexOf('function maybeOffer()'), main.indexOf('navigator.xr.offerSession('));
  assert.match(offer, /ensureCapacity\(state\.cube, \{ page: true \}\)/, 'no 3-D allocation once a session may start (#229)');
  // A session the browser made is ended when the page cannot use it, and no
  // 3-D texture is allocated until it has ended: held as refused (#305).
  const refuse = main.slice(main.indexOf('const refuse = (msg) => {'), main.indexOf('throw new Error(msg);'));
  assert.ok(refuse.length > 0 && refuse.indexOf('xrFlag.holdRefused(given)') >= 0
    && refuse.indexOf('xrFlag.holdRefused(given)') < refuse.indexOf('given.end()'), refuse);
  assert.doesNotMatch(refuse, /xrFlag\.own\(|R\.xrActive = /, 'a refused session never owns the flag');
  // Only the flag writes R.xrActive; the page's own session owns it.
  assert.equal((main.match(/R\.xrActive = /g) || []).length, 1);
  assert.match(main, /new XE\.SessionFlag\(\(active, owner\) => \{ R\.xrActive = active; R\.xrSession = owner; \}\)/);
  assert.match(main, /xrFlag\.own\(session\);\n\s*session\.addEventListener\('end', \(\) => \{ if \(xrFlag\.ended\(session\)\)/);
  // Every Enter VR failure says what to do (#291).
  for (const id of ['enter-vr', 'enter-ar']) assert.match(main, new RegExp(`\\$\\('${id}'\\)\\.addEventListener\\('click', \\(\\) => enterXR\\([^)]*\\)\\.catch\\(enterFailed\\)\\)`));
  assert.doesNotMatch(main, /setStatus\(`XR: /);
  // Nothing waiting for a session's end runs while the page's own request is
  // under way; a failed request runs it then (#305).
  const after = main.slice(main.indexOf('function afterXREnd('), main.indexOf('const afterXR = {'));
  assert.match(after, /if \(R\.xrActive \|\| entering\) \{ afterXR\.deferred\+\+; return; \}/);
  assert.ok(after.indexOf('if (wasBench) afterXR.benchEnded = true') < after.indexOf('if (R.xrActive || entering)'), 'a benchmark end is noted even when deferred');
  const request = main.slice(main.indexOf('session = await navigator.xr.requestSession('), main.indexOf('finally { entering = false; }'));
  assert.match(request, /catch \(err\) \{[\s\S]*setTimeout\(\(\) => afterXREnd\(false\), 0\);[\s\S]*throw err;/);
  // Auto-enter: quiet for a refusal for want of a press, said otherwise.
  const auto = main.slice(main.indexOf('function maybeAutoEnter()'), main.indexOf('function maybeOffer()'));
  assert.match(auto, /if \(refused\)[^\n]*press Enter VR`;\n\s*else enterFailed\(err\);/);
  // A refused session that never ends: said (reload), never released by a timer.
  assert.match(main, /if \(R\.xrActive && !R\.xrSession && !entering\) refuse\('a session the browser started for this page has not closed yet'\)/);
});
