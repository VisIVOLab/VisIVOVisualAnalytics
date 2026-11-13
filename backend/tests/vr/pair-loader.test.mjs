// The public loader's pure decisions (issue #213):
// node --test backend/tests/vr/pair-loader.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';

import {
  serviceUrl, serviceHost, normaliseCode, pairFromHash, appQuery, appFragment, appAddress, failureKind,
  resumeDelayMs, keptResume, linkFigures, isLoopbackHost, AppIdMap,
} from '../../../web/vr-loader/js/loader-core.js';
import { encodeChunk, decodeChunk } from '../../../web/vr-loader/js/pair-wire.js';

test('page ids: a reloaded page reusing id 1 never gets the old document\'s answer', () => {
  const ids = new AppIdMap();
  const oldDoc = {}, newDoc = {};
  const a = ids.outgoing(oldDoc, JSON.stringify({ t: 'req', id: 1, method: 'GET', path: '/old', body: false }));
  const b = ids.outgoing(newDoc, JSON.stringify({ t: 'req', id: 1, method: 'GET', path: '/v1/vr/info', body: false }));
  assert.notEqual(a.wire, b.wire);
  assert.ok(a.wire % 2 === 1 && b.wire % 2 === 1);
  // The old document's answer goes to the old document (the loader drops it: not current).
  const late = ids.incoming(JSON.stringify({ t: 'res', id: a.wire, status: 200, headers: {} }));
  assert.equal(late.doc, oldDoc);
  // Forgetting the old document lists its open request, to cancel.
  assert.deepEqual(ids.forget(oldDoc), [a.wire]);
  assert.equal(ids.incoming(encodeChunk(a.wire, true, null)), null);
  // The new document's answer, with its own id back.
  const res = ids.incoming(JSON.stringify({ t: 'res', id: b.wire, status: 200, headers: {} }));
  assert.equal(res.doc, newDoc);
  assert.equal(JSON.parse(res.frame).id, 1);
  const chunk = ids.incoming(encodeChunk(b.wire, true, Uint8Array.of(7)));
  assert.equal(decodeChunk(chunk.frame).id, 1);
  assert.deepEqual([...decodeChunk(chunk.frame).payload], [7]);
  assert.equal(ids.open, 0, 'the last chunk ends it');
});

test('page ids: body chunks and cancels are mapped; unknown ids are dropped', () => {
  const ids = new AppIdMap();
  const doc = {};
  const req = ids.outgoing(doc, JSON.stringify({ t: 'req', id: 5, method: 'POST', path: '/x', body: true }));
  const c = ids.outgoing(doc, encodeChunk(5, true, Uint8Array.of(1, 2)));
  assert.equal(decodeChunk(c.frame).id, req.wire);
  assert.deepEqual([...decodeChunk(c.frame).payload], [1, 2]);
  const cancel = ids.outgoing(doc, JSON.stringify({ t: 'cancel', id: 5 }));
  assert.equal(cancel.cancel, true);
  assert.deepEqual(JSON.parse(cancel.frame), { t: 'cancel', id: req.wire });
  assert.equal(ids.open, 0);
  assert.equal(ids.outgoing(doc, encodeChunk(5, true, null)), null);
  assert.equal(ids.outgoing(doc, JSON.stringify({ t: 'cancel', id: 9 })), null);
  assert.equal(ids.outgoing(doc, 'garbage'), null);
  const r2 = ids.outgoing(doc, JSON.stringify({ t: 'req', id: 6, method: 'GET', path: '/y', body: false }));
  assert.ok(ids.incoming(JSON.stringify({ t: 'err', id: r2.wire, message: 'm' })));
  assert.equal(ids.open, 0, 'an err ends it');
});

const pub = { hostname: 'vr.example.org', href: 'https://vr.example.org/x/vr-loader/' };
const local = { hostname: 'localhost', href: 'http://localhost:8821/' };

test('the service address: from config.json, relative to it, never plain ws on a public loader', () => {
  const cfg = 'https://vr.example.org/x/vr-loader/config.json';
  assert.equal(serviceUrl('wss://pair.example.org/svc/', '', pub, cfg), 'wss://pair.example.org/svc');
  assert.equal(serviceUrl('https://pair.example.org', '', pub, cfg), 'wss://pair.example.org');
  assert.equal(serviceUrl('../pair', '', pub, cfg), 'wss://vr.example.org/x/pair');
  assert.equal(serviceUrl('ws://pair.example.org', '', pub, cfg), '');
  assert.equal(serviceUrl('', '', pub, cfg), '');
  assert.equal(serviceUrl(undefined, '', pub, cfg), '');
  assert.equal(serviceUrl('ftp://x', '', pub, cfg), '');
});

test('?pair_service= is honoured on a loopback loader only', () => {
  assert.equal(serviceUrl('wss://a.example.org', 'ws://localhost:8822', local, 'http://localhost:8821/config.json'), 'ws://localhost:8822');
  assert.equal(serviceUrl('wss://a.example.org', 'wss://evil.example.com', pub, 'https://vr.example.org/config.json'), 'wss://a.example.org');
  assert.equal(serviceHost('wss://pair.example.org:8443/svc'), 'pair.example.org:8443');
  assert.ok(isLoopbackHost('127.0.0.1') && isLoopbackHost('[::1]') && !isLoopbackHost('vr.example.org'));
});

test('codes: from the address, typed with spaces or dashes', () => {
  assert.deepEqual(pairFromHash('#pair=abc123'), { code: 'ABC123', rest: '' });
  assert.deepEqual(pairFromHash('#x=1&pair=ab-c1%2023&y'), { code: 'ABC123', rest: 'x=1&y' });
  assert.deepEqual(pairFromHash(''), { code: '', rest: '' });
  assert.deepEqual(pairFromHash('#pair=%E0'), { code: '', rest: '' });
  assert.equal(normaliseCode(' abc 123 '), 'ABC123');
  const resume = 'Xy_z-0123456789abcdefghijk';
  assert.equal(normaliseCode(resume), resume.replace(/-/g, ''));
});

test('the app\'s address: its own directory, the loader\'s query, the welcome as the fragment (no token)', () => {
  assert.equal(appQuery('?debug&panel3d&pair_service=ws%3A%2F%2Flocalhost%3A1'), '?transport=pair&debug&panel3d');
  assert.equal(appQuery(''), '?transport=pair');
  const w = { t: 'welcome', session: 's 1', dataset: 'd1', path: '/data/cube.fits',
    params: { kind: 'points', box: '100', token: 'never' }, page_build: 'abc' };
  assert.equal(appFragment(w), 'kind=points&box=100&session=s%201&dataset=d1&path=%2Fdata%2Fcube.fits');
  assert.equal(appFragment({ params: { session: 'a', 'bad key': 'x' } }), 'session=a');
  assert.equal(appFragment(null), '');
  assert.equal(appAddress('k3j4l5m6', '?debug', { session: 's' }), 'app/k3j4l5m6/vr/?transport=pair&debug#session=s');
  assert.equal(appAddress('k3j4l5m6', '', {}), 'app/k3j4l5m6/vr/?transport=pair');
});

test('what a failure means: a new code, or resume later', () => {
  for (const r of ['unknown-code', 'expired', 'used', 'rejected', 'bye']) assert.equal(failureKind(r), 'lost', r);
  for (const r of ['host-gone', 'rate-limited', 'timeout', 'ice-failed', 'service-unreachable', '']) assert.equal(failureKind(r), 'retry', r);
  assert.deepEqual([1, 2, 3, 4, 5, 9].map((n) => resumeDelayMs(n)), [1000, 2000, 4000, 8000, 15000, 15000]);
  assert.equal(resumeDelayMs(1, 'rate-limited'), 30000);
});

test('a kept resume code: only while it is valid', () => {
  const now = 1_000_000;
  assert.deepEqual(keptResume(JSON.stringify({ code: 'r', expiresAt: now + 60000 }), now), { code: 'r', expiresAt: now + 60000 });
  assert.equal(keptResume(JSON.stringify({ code: 'r', expiresAt: now + 1000 }), now), null);
  assert.equal(keptResume('garbage', now), null);
  assert.equal(keptResume(null, now), null);
});

test('the link\'s figures from getStats(): the selected pair, its candidate types and round trip', () => {
  const reports = [
    { id: 'T', type: 'transport', selectedCandidatePairId: 'P' },
    { id: 'P', type: 'candidate-pair', localCandidateId: 'L', remoteCandidateId: 'R', currentRoundTripTime: 0.012, state: 'succeeded' },
    { id: 'L', type: 'local-candidate', candidateType: 'srflx', protocol: 'udp' },
    { id: 'R', type: 'remote-candidate', candidateType: 'host' },
  ];
  assert.deepEqual(linkFigures(reports), { candidates: 'srflx/host udp', rttMs: 12 });
  assert.deepEqual(linkFigures([]), {});
});
