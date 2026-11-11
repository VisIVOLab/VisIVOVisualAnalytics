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
