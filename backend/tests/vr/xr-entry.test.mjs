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
  assert.match(loader, /appAddress\(state\.tag, window\.location\.search, welcome,\s*launchMode\(/);
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

test('offerSession: used only where it exists, once at a time', () => {
  assert.equal(XE.canOffer({ offerSession() {} }), true);
  assert.equal(XE.canOffer({ requestSession() {} }), false);
  assert.equal(XE.canOffer(undefined), false);
  const base = { offerable: true, vrSupported: true, active: true, inXR: false, offered: false };
  assert.equal(XE.shouldOffer(base), true);
  for (const k of ['offerable', 'vrSupported', 'active']) assert.equal(XE.shouldOffer({ ...base, [k]: false }), false, k);
  for (const k of ['inXR', 'offered']) assert.equal(XE.shouldOffer({ ...base, [k]: true }), false, k);
});

test('the page wires them: the offered session goes through enterXR, the capacity is set before the offer', () => {
  const main = readFileSync(new URL('../../app/static/vr/js/main.js', import.meta.url), 'utf8');
  assert.match(main, /XE\.canOffer\(navigator\.xr\)/);
  assert.match(main, /navigator\.xr\.offerSession\('immersive-vr'/);
  assert.match(main, /enterXR\('immersive-vr', false, session\)/);
  const offer = main.slice(main.indexOf('function maybeOffer()'), main.indexOf('navigator.xr.offerSession('));
  assert.match(offer, /ensureCapacity\(state\.cube, \{ page: true \}\)/, 'no 3-D allocation once a session may start (#229)');
  // A session the browser made is ended when the page cannot use it, and
  // no 3-D texture is allocated until it has ended.
  const refuse = main.slice(main.indexOf('const refuse = (msg) => {'), main.indexOf('throw new Error(msg);'));
  assert.ok(refuse.length > 0 && refuse.indexOf('R.xrActive = true') >= 0
    && refuse.indexOf('R.xrActive = true') < refuse.indexOf('given.end()'), refuse);
});
