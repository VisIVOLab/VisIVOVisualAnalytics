// The desktop's pairing link (issue #172): node --test backend/tests/vr/pairing.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import { parsePairing, strippedAddress, pairingFallback, PAIRING_KEYS } from '../../app/static/vr/js/pairing.js';

// What VrLink::encodeValue writes: every byte but RFC 3986's unreserved ones.
const enc = (s) => encodeURIComponent(s).replace(/[!'()*]/g, (c) => `%${c.charCodeAt(0).toString(16).toUpperCase()}`);

test('the desktop link decodes to its four values', () => {
  const token = 'aB3_-.~x+y/z=&#%';
  const path = '/Users/me/Data Cubes/HCG 44 (v2)&co.fits';
  const hash = `#token=${enc(token)}&session=${enc('5f0c1a2b-3c4d-4e5f-8a9b-0c1d2e3f4a5b')}&dataset=${enc('ds_12')}&path=${enc(path)}`;
  const p = parsePairing(hash);
  assert.equal(p.token, token);
  assert.equal(p.session, '5f0c1a2b-3c4d-4e5f-8a9b-0c1d2e3f4a5b');
  assert.equal(p.dataset, 'ds_12');
  assert.equal(p.path, path);
  assert.deepEqual(p.errors, []);
  assert.equal(p.rest, '');
  assert.equal(p.present, true);
});

test('order does not matter, and "+" stays a plus (decoded once)', () => {
  const p = parsePairing('path=%2Fa%2Bb.fits&dataset=d&token=t%2Bu&session=s');
  assert.equal(p.path, '/a+b.fits');
  assert.equal(p.token, 't+u');
  assert.equal(p.dataset, 'd');
  assert.equal(p.session, 's');
  // Decoded once only: an encoded escape stays an escape.
  assert.equal(parsePairing('#path=%252F').path, '%2F');
  assert.equal(parsePairing('#path=a+b').path, 'a+b');
});

test('missing keys are empty; a token alone is the old link', () => {
  const p = parsePairing('#token=abc');
  assert.deepEqual([p.token, p.session, p.dataset, p.path], ['abc', '', '', '']);
  assert.equal(p.present, true);
  const q = parsePairing('#session=s&path=%2Fx.fits');
  assert.deepEqual([q.token, q.session, q.dataset, q.path], ['', 's', '', '/x.fits']);
  assert.equal(parsePairing('').present, false);
  assert.equal(parsePairing('#').present, false);
  assert.equal(parsePairing(undefined).present, false);
  assert.equal(parsePairing('#token=').token, '');
  assert.equal(parsePairing('#token').present, true);
});

test('a malformed escape empties that key only and is reported by name, not value', () => {
  const p = parsePairing('#token=ab%ZZ&session=s&dataset=d%E0%A4&path=%2Fok');
  assert.equal(p.token, '');
  assert.equal(p.dataset, '');
  assert.equal(p.session, 's');
  assert.equal(p.path, '/ok');
  assert.deepEqual(p.errors.sort(), ['dataset', 'token']);
  assert.ok(!JSON.stringify(p.errors).includes('ZZ'));
});

test('extra keys are kept for the address; pairing keys are removed', () => {
  const p = parsePairing('#view=top&token=t&x&session=s&dataset=d&path=p&tokenx=1');
  assert.equal(p.rest, 'view=top&x&tokenx=1');
  assert.equal(p.token, 't');
  assert.equal(strippedAddress('/vr/', '?debug', '#view=top&token=t&session=s'), '/vr/?debug#view=top');
  assert.equal(strippedAddress('/vr/', '', '#token=t&session=s&dataset=d&path=p'), '/vr/');
  assert.equal(strippedAddress('/vr/', '', ''), '/vr/');
  for (const k of PAIRING_KEYS) assert.ok(!strippedAddress('/vr/', '', `#${k}=v&a=1`).includes(k));
});

test('a key given twice keeps its first value; empty parts are ignored', () => {
  const p = parsePairing('#&&token=first&token=second&&dataset=a&dataset=b&');
  assert.equal(p.token, 'first');
  assert.equal(p.dataset, 'a');
  assert.equal(p.rest, '');
});

test('fallback: dataset used, re-opened in the session, or a new session', () => {
  assert.equal(pairingFallback(null, true), 'use');
  assert.equal(pairingFallback(new Error("404: Unknown dataset_id 'd'. Open the dataset first via /v1/datasets/open."), true), 'open');
  assert.equal(pairingFallback(new Error('404: Session not found.'), true), 'new-session');
  assert.equal(pairingFallback(new Error('400: X-Visivo-Session must be a high-entropy id (>= 32 URL-safe chars), e.g. a UUID.'), true), 'new-session');
  assert.equal(pairingFallback(new Error('404: Session not found.'), false), 'fail');
  assert.equal(pairingFallback(new Error('401: Invalid token'), true), 'fail');
  assert.equal(pairingFallback(new Error('422: This endpoint requires a spectral cube dataset.'), true), 'fail');
  assert.equal(pairingFallback(new Error('network down'), true), 'fail');
});

test('a read that fails at the network is sent once more; an answer or another error is not', async () => {
  const { retryOnNetworkError } = await import('../../app/static/vr/js/api.js');
  let n = 0;
  assert.equal(await retryOnNetworkError(async () => { if (n++ === 0) throw new TypeError('Failed to fetch'); return 'ok'; }, { waitMs: 1 }), 'ok');
  assert.equal(n, 2);
  n = 0;
  await assert.rejects(retryOnNetworkError(async () => { n++; throw new TypeError('Failed to fetch'); }, { waitMs: 1 }), TypeError);
  assert.equal(n, 2);                               // once more, not forever
  n = 0;
  await assert.rejects(retryOnNetworkError(async () => { n++; throw new Error('500: boom'); }, { waitMs: 1 }), /500/);
  assert.equal(n, 1);
});

test('the reduced cube is resent only when its request failed within 2 s of being sent', async () => {
  const { retryOnNetworkError, RESEND_WITHIN_MS } = await import('../../app/static/vr/js/api.js');
  assert.equal(RESEND_WITHIN_MS, 2000);
  let clock = 0, n = 0;
  const now = () => clock;
  // Failed at once (before or while sending): sent again.
  const quick = async () => { n++; if (n === 1) { clock += 50; throw new TypeError('Failed to fetch'); } return 'ok'; };
  assert.equal(await retryOnNetworkError(quick, { waitMs: 1, withinMs: RESEND_WITHIN_MS, now }), 'ok');
  assert.equal(n, 2);
  // The connection lost after 30 s of waiting (the backend still computing): not sent again.
  n = 0;
  const late = async () => { n++; clock += 30000; throw new TypeError('Failed to fetch'); };
  await assert.rejects(retryOnNetworkError(late, { waitMs: 1, withinMs: RESEND_WITHIN_MS, now }), TypeError);
  assert.equal(n, 1);
});

test('fallback: a 404 during a pyramid build, or a superseded load, does not reopen the path', () => {
  const during = Object.assign(new Error("404: Unknown dataset_id 'd'."), { duringBuild: true });
  assert.equal(pairingFallback(during, true), 'fail');
  const replaced = Object.assign(new Error('another cube was loaded meanwhile'), { superseded: true });
  assert.equal(pairingFallback(replaced, true), 'fail');
});
