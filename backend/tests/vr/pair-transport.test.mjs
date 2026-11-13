// The pairing's data-channel transport (issue #213, contract visivo-pair/1):
// node --test backend/tests/vr/pair-transport.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

import {
  CHUNK_BYTES, HIGH_WATER_BYTES, encodeChunk, decodeChunk, bodyChunks, frameId, encodeWsFrame, decodeWsFrame,
  wsMessageFrames, wsControlFrame, WsReassembler, PacedSender, WS_TEXT, WS_BINARY, WS_OPEN, WS_CLOSE,
} from '../../app/static/vr/js/pair-wire.js';
import { ApiClient, ChannelWebSocket, requestHeaders } from '../../app/static/vr/js/pair-transport.js';
import { backendPath, socketRequest, pairedRequested } from '../../app/static/vr/js/transport.js';
import { hookedFetch } from '../../app/static/vr/js/api.js';

/** A fake RTCDataChannel: what is sent is kept; bufferedAmount is set by the test. */
class FakeChannel extends EventTarget {
  constructor() {
    super();
    this.sent = [];
    this.bufferedAmount = 0;
    this.bufferedAmountLowThreshold = 0;
    this.readyState = 'open';
    this.binaryType = 'blob';
  }
  send(d) {
    if (this.readyState !== 'open') throw new Error('closed');
    this.sent.push(d);
  }
  close() { if (this.readyState !== 'closed') { this.readyState = 'closed'; this.dispatchEvent(new Event('close')); } }
  deliver(data) { this.dispatchEvent(new MessageEvent('message', { data })); }
  drain(to = 0) { this.bufferedAmount = to; this.dispatchEvent(new Event('bufferedamountlow')); }
}

const json = (frame) => JSON.parse(frame);
const tick = () => new Promise((r) => setTimeout(r, 0));
const bytes = (n, seed = 1) => Uint8Array.from({ length: n }, (_, i) => (i * 31 + seed) & 255);

// ── Framing ────────────────────────────────────────────────────────────────

test('api chunk: 4-byte big-endian id, a flag byte, at most 16 KiB', () => {
  const f = new Uint8Array(encodeChunk(0x01020304, true, Uint8Array.of(9, 8)));
  assert.deepEqual([...f], [1, 2, 3, 4, 1, 9, 8]);
  const c = decodeChunk(f.buffer);
  assert.equal(c.id, 0x01020304);
  assert.equal(c.last, true);
  assert.deepEqual([...c.payload], [9, 8]);
  assert.equal(decodeChunk(new Uint8Array(4).buffer), null);
  assert.throws(() => encodeChunk(1, false, new Uint8Array(CHUNK_BYTES + 1)), RangeError);
  assert.equal(decodeChunk(encodeChunk(0xffffffff, false, null)).id, 0xffffffff);
});

test('a body: 16 KiB chunks, only the last flagged; an empty body is one empty last chunk', () => {
  const b = bytes(2 * CHUNK_BYTES + 5);
  const chunks = bodyChunks(7, b).map(decodeChunk);
  assert.deepEqual(chunks.map((c) => [c.id, c.last, c.payload.byteLength]),
    [[7, false, CHUNK_BYTES], [7, false, CHUNK_BYTES], [7, true, 5]]);
  const joined = new Uint8Array(chunks.reduce((n, c) => n + c.payload.byteLength, 0));
  let o = 0; for (const c of chunks) { joined.set(c.payload, o); o += c.payload.byteLength; }
  assert.deepEqual(joined, b);
  const empty = bodyChunks(3, new Uint8Array(0)).map(decodeChunk);
  assert.deepEqual(empty.map((c) => [c.id, c.last, c.payload.byteLength]), [[3, true, 0]]);
  assert.equal(bodyChunks(1, bytes(CHUNK_BYTES)).length, 1);
});

test('frameId reads text and binary frames alike', () => {
  assert.equal(frameId(JSON.stringify({ t: 'res', id: 5 })), 5);
  assert.equal(frameId(encodeChunk(6, true, null)), 6);
  assert.equal(frameId('not json'), null);
  assert.equal(frameId(JSON.stringify({ t: 'res' })), null);
});

test('ws frames: type byte, flag byte, fragments of 16 KiB reassembled (amendment 1)', () => {
  const f = new Uint8Array(encodeWsFrame(WS_BINARY, true, Uint8Array.of(1, 2)));
  assert.deepEqual([...f], [2, 1, 1, 2]);
  assert.deepEqual(decodeWsFrame(f).type, WS_BINARY);
  const big = bytes(3 * CHUNK_BYTES + 100, 3);
  const frames = wsMessageFrames(big);
  assert.equal(frames.length, 4);
  assert.deepEqual(frames.map((x) => new Uint8Array(x)[1]), [0, 0, 0, 1]);
  assert.ok(frames.every((x) => x.byteLength <= CHUNK_BYTES + 2));
  const rx = new WsReassembler();
  const out = frames.map((x) => rx.push(x));
  assert.deepEqual(out.slice(0, 3), [null, null, null]);
  assert.equal(out[3].kind, 'binary');
  assert.deepEqual(new Uint8Array(out[3].data), big);
  // Text, split across a multi-byte character.
  const text = 'é'.repeat(CHUNK_BYTES);       // 2 bytes each: two full fragments
  const tf = wsMessageFrames(text);
  assert.equal(new Uint8Array(tf[0])[0], WS_TEXT);
  let r = null; for (const x of tf) r = rx.push(x);
  assert.deepEqual(r, { kind: 'text', data: text });
  // An empty message is one frame flagged last.
  assert.deepEqual(rx.push(wsMessageFrames('')[0]), { kind: 'text', data: '' });
  assert.equal(new Uint8Array(wsMessageFrames(new ArrayBuffer(0))[0]).byteLength, 2);
});

test('ws control frames, and frames that break the framing', () => {
  const rx = new WsReassembler();
  assert.deepEqual(rx.push(wsControlFrame(WS_OPEN, { protocol: 'visivo.v1' })), { kind: 'open', protocol: 'visivo.v1' });
  assert.deepEqual(rx.push(wsControlFrame(WS_CLOSE, { code: 1013, reason: 'busy' })), { kind: 'close', code: 1013, reason: 'busy' });
  assert.equal(rx.push(new Uint8Array([1]).buffer).kind, 'error');
  assert.equal(rx.push(encodeWsFrame(9, true, null)).kind, 'error');
  rx.push(encodeWsFrame(WS_TEXT, false, Uint8Array.of(65)));
  assert.equal(rx.push(encodeWsFrame(WS_BINARY, true, Uint8Array.of(66))).kind, 'error');
  const rx2 = new WsReassembler();
  rx2.push(encodeWsFrame(WS_TEXT, false, Uint8Array.of(65)));
  assert.equal(rx2.push(wsControlFrame(WS_CLOSE, { code: 1000 })).kind, 'error');
});

// ── Backpressure ───────────────────────────────────────────────────────────

test('the sender pauses above 1 MiB buffered and resumes on bufferedamountlow, in order', async () => {
  const ch = new FakeChannel();
  const s = new PacedSender(ch, { pollMs: 10000 });
  assert.equal(ch.bufferedAmountLowThreshold, HIGH_WATER_BYTES);
  s.send('a');
  ch.bufferedAmount = HIGH_WATER_BYTES + 1;
  s.send('b'); s.send('c');
  assert.deepEqual(ch.sent, ['a']);
  assert.equal(s.queued, 2);
  assert.equal(s.pauses, 1);
  ch.drain(HIGH_WATER_BYTES);            // at the mark: sending again
  assert.deepEqual(ch.sent, ['a', 'b', 'c']);
  s.close();
});

test('the sender also resumes by polling a channel that never fires the event', async () => {
  const ch = new FakeChannel();
  const s = new PacedSender(ch, { pollMs: 5 });
  ch.bufferedAmount = 2 * HIGH_WATER_BYTES;
  s.send('x');
  assert.deepEqual(ch.sent, []);
  ch.bufferedAmount = 0;                 // no event
  await new Promise((r) => setTimeout(r, 30));
  assert.deepEqual(ch.sent, ['x']);
  s.close();
});

test('a large upload goes out paced: at most 1 MiB plus one chunk ahead of the channel', async () => {
  const ch = new FakeChannel();
  const s = new PacedSender(ch, { pollMs: 10000 });
  const client = new ApiClient((f) => s.send(f));
  // The channel's buffer grows with every send, as a real one would.
  const send = ch.send.bind(ch);
  ch.send = (d) => { send(d); ch.bufferedAmount += typeof d === 'string' ? d.length : d.byteLength; };
  const body = bytes(3 * 1024 * 1024);
  const p = client.fetch('/v1/cube/mask_stats_bin?x=1', { method: 'POST', body });
  assert.ok(ch.bufferedAmount <= HIGH_WATER_BYTES + CHUNK_BYTES + 5 + 512, `${ch.bufferedAmount}`);
  const before = ch.sent.length;
  assert.ok(s.queued > 0);
  ch.drain(0);
  assert.ok(ch.sent.length > before);
  while (s.queued) ch.drain(0);
  const chunks = ch.sent.slice(1).map(decodeChunk);
  assert.equal(chunks.reduce((n, c) => n + c.payload.byteLength, 0), body.byteLength);
  assert.ok(chunks.at(-1).last && chunks.slice(0, -1).every((c) => !c.last));
  client.receive(JSON.stringify({ t: 'res', id: 1, status: 200, headers: {} }));
  client.receive(encodeChunk(1, true, null));
  assert.equal((await p).status, 200);
  s.close();
});

test('a cancelled upload above the high-water mark: its queued frames go back, the cancel goes first', async () => {
  const ch = new FakeChannel();
  const s = new PacedSender(ch, { pollMs: 10000 });
  const client = new ApiClient((f) => s.send(f), { sender: s });
  const other = client.fetch('/v1/other');              // id 1, goes out at once
  ch.bufferedAmount = HIGH_WATER_BYTES + 1;
  const ac = new AbortController();
  const p = client.fetch('/v1/share/publish', { method: 'POST', body: new Uint8Array(8 * CHUNK_BYTES), signal: ac.signal });
  assert.equal(s.queued, 1 + 8);                          // req + 8 chunks, held
  ac.abort();
  await assert.rejects(p, (e) => e.name === 'AbortError');
  assert.equal(s.queued, 1, 'only the cancel is left');
  ch.drain(0);
  const after = ch.sent.slice(1);
  assert.deepEqual(after.map((f) => (typeof f === 'string' ? json(f) : 'chunk')), [{ t: 'cancel', id: 2 }]);
  // Another request's frames are kept, behind the cancel.
  ch.bufferedAmount = HIGH_WATER_BYTES + 1;
  const ac2 = new AbortController();
  const q = client.fetch('/v1/a', { method: 'POST', body: 'x', signal: ac2.signal });   // id 3
  const r = client.fetch('/v1/b', { method: 'POST', body: 'y' });                       // id 4
  ac2.abort();
  await assert.rejects(q, (e) => e.name === 'AbortError');
  ch.drain(0);
  assert.deepEqual(ch.sent.slice(2).map((f) => (typeof f === 'string' ? json(f).t + json(f).id : `chunk${decodeChunk(f).id}`)),
    ['cancel3', 'req4', 'chunk4']);
  client.failAll();
  await assert.rejects(r, TypeError);
  await assert.rejects(other, TypeError);
  s.close();
});

// ── fetch over the api channel ─────────────────────────────────────────────

test('a request: req frame, body chunks, then a streamed Response', async () => {
  const ch = new FakeChannel();
  const client = new ApiClient((f) => ch.send(f));
  const p = client.fetch('/v1/cube/reduced_bin', {
    method: 'post', headers: { 'Content-Type': 'application/json', 'X-Visivo-Token': 'secret-token', 'X-Visivo-Session': 's1' },
    body: JSON.stringify({ dataset_id: 'd' }),
  });
  const req = json(ch.sent[0]);
  assert.deepEqual(req, { t: 'req', id: 1, method: 'POST', path: '/v1/cube/reduced_bin',
    headers: { 'content-type': 'application/json', 'x-visivo-session': 's1' }, body: true });
  assert.ok(!JSON.stringify(ch.sent[0]).includes('secret-token'), 'the token never goes over the pairing');
  const c = decodeChunk(ch.sent[1]);
  assert.equal(c.last, true);
  assert.equal(new TextDecoder().decode(c.payload), '{"dataset_id":"d"}');
  const payload = bytes(40000, 7);
  client.receive(JSON.stringify({ t: 'res', id: 1, status: 200, headers: { 'Content-Length': String(payload.byteLength), 'X-Visivo-Width': '4' } }));
  const resp = await p;
  assert.equal(resp.ok, true);
  assert.equal(resp.headers.get('content-length'), '40000');
  assert.equal(resp.headers.get('x-visivo-width'), '4');
  const reader = resp.body.getReader();
  for (const f of bodyChunks(1, payload)) client.receive(f);
  const got = [];
  for (;;) { const { done, value } = await reader.read(); if (done) break; got.push(...value); }
  assert.deepEqual(Uint8Array.from(got), payload);
  assert.equal(client.pending.size, 0);
});

test('GET without a body: body:false and no chunk; header lists of pairs are accepted', async () => {
  const ch = new FakeChannel();
  const client = new ApiClient((f) => ch.send(f), { idStart: 2, idStep: 2 });
  const p1 = client.fetch('/vr/b/abc123/js/main.js');
  const p2 = client.fetch('/v1/vr/info', { headers: new Headers({ 'X-Visivo-Token': 't0k3n-long' }) });
  assert.equal(ch.sent.length, 2);
  assert.deepEqual([json(ch.sent[0]).id, json(ch.sent[1]).id], [2, 4]);
  assert.equal(json(ch.sent[0]).body, false);
  assert.deepEqual(json(ch.sent[1]).headers, {});
  client.receive(JSON.stringify({ t: 'res', id: 2, status: 200, headers: [['Content-Type', 'text/javascript']] }));
  client.receive(encodeChunk(2, false, new TextEncoder().encode('export ')));
  client.receive(encodeChunk(2, true, new TextEncoder().encode('{};')));
  const r1 = await p1;
  assert.equal(r1.headers.get('content-type'), 'text/javascript');
  assert.equal(await r1.text(), 'export {};');
  client.receive(JSON.stringify({ t: 'res', id: 4, status: 204, headers: {} }));
  client.receive(encodeChunk(4, true, null));
  const r2 = await p2;
  assert.equal(r2.status, 204);
  assert.equal(r2.body, null);
});

test('HTTP errors come back as Responses (statuses kept), not as rejections', async () => {
  const ch = new FakeChannel();
  const client = new ApiClient((f) => ch.send(f));
  const p = client.fetch('/v1/datasets/x');
  client.receive(JSON.stringify({ t: 'res', id: 1, status: 404, headers: { 'content-type': 'application/json' } }));
  client.receive(encodeChunk(1, true, new TextEncoder().encode('{"detail":"dataset not found"}')));
  const r = await p;
  assert.equal(r.status, 404);
  assert.equal(r.ok, false);
  assert.deepEqual(await r.json(), { detail: 'dataset not found' });
});

test('abort before the answer: AbortError, and a cancel frame', async () => {
  const ch = new FakeChannel();
  const client = new ApiClient((f) => ch.send(f));
  const ac = new AbortController();
  const p = client.fetch('/v1/spectral/probe', { method: 'POST', body: '{}', signal: ac.signal });
  ac.abort();
  await assert.rejects(p, (e) => e.name === 'AbortError');
  assert.deepEqual(json(ch.sent.at(-1)), { t: 'cancel', id: 1 });
  assert.equal(client.stats.cancels, 1);
  // Late frames for it are dropped.
  client.receive(JSON.stringify({ t: 'res', id: 1, status: 200, headers: {} }));
  client.receive(encodeChunk(1, true, null));
  assert.equal(client.pending.size, 0);
  // Already aborted: nothing sent at all.
  const n = ch.sent.length;
  await assert.rejects(client.fetch('/v1/ping', { signal: AbortSignal.abort() }), (e) => e.name === 'AbortError');
  assert.equal(ch.sent.length, n);
});

test('abort while the body streams: the stream errors, a cancel frame goes', async () => {
  const ch = new FakeChannel();
  const client = new ApiClient((f) => ch.send(f));
  const ac = new AbortController();
  const p = client.fetch('/v1/cube/reduced_bin', { method: 'POST', body: '{}', signal: ac.signal });
  client.receive(JSON.stringify({ t: 'res', id: 1, status: 200, headers: {} }));
  const r = await p;
  client.receive(encodeChunk(1, false, bytes(10)));
  const reader = r.body.getReader();
  assert.equal((await reader.read()).value.byteLength, 10);
  ac.abort('timeout');
  await assert.rejects(reader.read());
  assert.deepEqual(json(ch.sent.at(-1)), { t: 'cancel', id: 1 });
});

test('the reader cancelling the body sends a cancel', async () => {
  const ch = new FakeChannel();
  const client = new ApiClient((f) => ch.send(f));
  const p = client.fetch('/v1/x');
  client.receive(JSON.stringify({ t: 'res', id: 1, status: 200, headers: {} }));
  await (await p).body.cancel();
  assert.deepEqual(json(ch.sent.at(-1)), { t: 'cancel', id: 1 });
});

test('err, a cancel from the backend and a dropped link reject as a fetch with nothing answering (TypeError)', async () => {
  const ch = new FakeChannel();
  let open = true;
  const client = new ApiClient((f) => ch.send(f), { isOpen: () => open });
  const a = client.fetch('/v1/a');
  client.receive(JSON.stringify({ t: 'err', id: 1, message: 'backend unreachable' }));
  await assert.rejects(a, (e) => e instanceof TypeError && /backend unreachable/.test(e.message));
  const b = client.fetch('/v1/b');
  client.receive(JSON.stringify({ t: 'cancel', id: 2 }));
  await assert.rejects(b, TypeError);
  const c = client.fetch('/v1/c');
  client.receive(JSON.stringify({ t: 'res', id: 3, status: 200, headers: {} }));
  const rc = await c;
  const d = client.fetch('/v1/d');
  client.failAll();
  await assert.rejects(d, TypeError);
  await assert.rejects(rc.arrayBuffer(), TypeError);
  open = false;
  await assert.rejects(client.fetch('/v1/e'), TypeError);
});

test('a drop then a resume: GET and HEAD in flight are sent again, a POST fails as network', async () => {
  const ch = new FakeChannel();
  const client = new ApiClient((f) => ch.send(f), { idStart: 1, idStep: 2 });
  const get = client.fetch('/vr/b/abc123/js/main.js');                       // id 1, no answer yet
  const post = client.fetch('/v1/cube/reduced_bin', { method: 'POST', body: '{}' });   // id 3
  const head = client.fetch('/v1/ping', { method: 'HEAD' });                 // id 5
  client.hold();
  await assert.rejects(post, (e) => e instanceof TypeError);
  assert.equal(client.held.length, 2);
  assert.equal(client.pending.size, 0);
  // Frames for the old ids (a late answer of the old link) are ignored.
  client.receive(JSON.stringify({ t: 'res', id: 1, status: 200, headers: {} }));
  const n = ch.sent.length;
  client.resend();
  const again = ch.sent.slice(n).map(json);
  assert.deepEqual(again.map((m) => [m.t, m.method, m.path, m.body]),
    [['req', 'GET', '/vr/b/abc123/js/main.js', false], ['req', 'HEAD', '/v1/ping', false]]);
  assert.ok(again.every((m) => m.id > 5), 'new ids');
  client.receive(JSON.stringify({ t: 'res', id: again[0].id, status: 200, headers: { 'content-type': 'text/javascript' } }));
  client.receive(encodeChunk(again[0].id, true, new TextEncoder().encode('export {};')));
  assert.equal(await (await get).text(), 'export {};');
  client.receive(JSON.stringify({ t: 'res', id: again[1].id, status: 200, headers: {} }));
  client.receive(encodeChunk(again[1].id, true, null));
  assert.equal((await head).status, 200);
  assert.equal(client.stats.resent, 2);
});

test('a POST marked idempotent (the reduced cube) is held and sent again, body and all', async () => {
  const ch = new FakeChannel();
  const client = new ApiClient((f) => ch.send(f));
  const p = client.fetch('/v1/cube/reduced_bin', { method: 'POST', body: '{"dataset_id":"d"}', idempotent: true });
  const q = client.fetch('/v1/cube/mask_stats_bin', { method: 'POST', body: 'x' });
  client.hold();
  await assert.rejects(q, TypeError);
  assert.equal(client.held.length, 1);
  const n = ch.sent.length;
  client.resend();
  const req = json(ch.sent[n]);
  assert.deepEqual([req.method, req.path, req.body], ['POST', '/v1/cube/reduced_bin', true]);
  assert.equal(new TextDecoder().decode(decodeChunk(ch.sent[n + 1]).payload), '{"dataset_id":"d"}');
  client.receive(JSON.stringify({ t: 'res', id: req.id, status: 200, headers: {} }));
  client.receive(encodeChunk(req.id, true, Uint8Array.of(5)));
  assert.deepEqual([...new Uint8Array(await (await p).arrayBuffer())], [5]);
});

test('a GET whose body had begun goes on where it stopped after the resume', async () => {
  const ch = new FakeChannel();
  const client = new ApiClient((f) => ch.send(f));
  const payload = bytes(50000, 9);
  const p = client.fetch('/v1/big');
  client.receive(JSON.stringify({ t: 'res', id: 1, status: 200, headers: { 'content-length': '50000', etag: '"v1"' } }));
  const resp = await p;
  client.receive(encodeChunk(1, false, payload.subarray(0, CHUNK_BYTES)));
  client.receive(encodeChunk(1, false, payload.subarray(CHUNK_BYTES, 2 * CHUNK_BYTES)));
  client.hold();
  client.resend();
  const id = json(ch.sent.at(-1)).id;
  client.receive(JSON.stringify({ t: 'res', id, status: 200, headers: { 'content-length': '50000', etag: '"v1"' } }));
  for (const f of bodyChunks(id, payload)) client.receive(f);     // the whole body again
  assert.deepEqual(new Uint8Array(await resp.arrayBuffer()), payload);
});

test('a resent HEAD with a length and no body is whole', async () => {
  const ch = new FakeChannel();
  const client = new ApiClient((f) => ch.send(f));
  const p = client.fetch('/file', { method: 'HEAD' });
  client.hold();
  client.resend();
  const id = json(ch.sent.at(-1)).id;
  client.receive(JSON.stringify({ t: 'res', id, status: 200, headers: { 'content-length': '100', etag: '"a"' } }));
  const r = await p;
  client.receive(encodeChunk(id, true, null));
  assert.equal(r.headers.get('content-length'), '100');
  assert.equal(await r.text(), '');
  assert.equal(client.pending.size, 0);
});

test('a body that had begun is never spliced from another answer, nor ends short', async () => {
  const run = async (first, second, firstBytes, secondBody) => {
    const ch = new FakeChannel();
    const client = new ApiClient((f) => ch.send(f));
    const p = client.fetch('/download');
    client.receive(JSON.stringify({ t: 'res', id: 1, status: 200, headers: first }));
    const resp = await p;
    client.receive(encodeChunk(1, false, Uint8Array.from(firstBytes)));
    client.hold();
    client.resend();
    const id = json(ch.sent.at(-1)).id;
    client.receive(JSON.stringify({ t: 'res', id, status: 200, headers: second }));
    client.receive(encodeChunk(id, true, Uint8Array.from(secondBody)));
    try { return [...new Uint8Array(await resp.arrayBuffer())]; } catch (e) { return e.message; }
  };
  // Another ETag, same length: not continued.
  assert.match(await run({ 'content-length': '4', etag: '"a"' }, { 'content-length': '4', etag: '"b"' }, [1, 2], [8, 9, 10, 11]), /changed/);
  // No ETag, or a weak one: not continued.
  assert.match(await run({ 'content-length': '4' }, { 'content-length': '4' }, [1, 2], [1, 2, 3, 4]), /changed/);
  assert.match(await run({ 'content-length': '4', etag: 'W/"a"' }, { 'content-length': '4', etag: 'W/"a"' }, [1, 2], [1, 2, 3, 4]), /changed/);
  // The length gone: not continued.
  assert.match(await run({ 'content-length': '4', etag: '"a"' }, { etag: '"a"' }, [1, 2, 3], [8]), /changed/);
  // Same ETag and length, but the second body ends short: an error, not a short success.
  assert.match(await run({ 'content-length': '4', etag: '"a"' }, { 'content-length': '4', etag: '"a"' }, [1, 2, 3], [1, 2]), /ended short/);
  // Same ETag and length, whole: continued.
  assert.deepEqual(await run({ 'content-length': '4', etag: '"a"' }, { 'content-length': '4', etag: '"a"' }, [1, 2], [1, 2, 3, 4]), [1, 2, 3, 4]);
});

test('a resent GET whose answer changed fails; a lost pairing fails what was held; abort while held', async () => {
  const ch = new FakeChannel();
  const client = new ApiClient((f) => ch.send(f));
  const p = client.fetch('/v1/x');
  client.receive(JSON.stringify({ t: 'res', id: 1, status: 200, headers: { 'content-length': '10' } }));
  const resp = await p;
  client.hold();
  client.resend();
  const id = json(ch.sent.at(-1)).id;
  client.receive(JSON.stringify({ t: 'res', id, status: 404, headers: {} }));
  await assert.rejects(resp.arrayBuffer(), /answer changed/);
  assert.deepEqual(json(ch.sent.at(-1)), { t: 'cancel', id });
  const q = client.fetch('/v1/y');
  client.hold();
  client.failAll('the pairing with the computer is lost');
  await assert.rejects(q, /pairing with the computer is lost/);
  assert.equal(client.held.length, 0);
  const ac = new AbortController();
  const r = client.fetch('/v1/z', { signal: ac.signal });
  client.hold();
  const sent = ch.sent.length;
  ac.abort();
  await assert.rejects(r, (e) => e.name === 'AbortError');
  assert.equal(ch.sent.length, sent, 'no cancel for a request that is not on the wire');
  client.resend();
  assert.equal(ch.sent.length, sent, 'nothing sent again');
});

test('hookedFetch keeps its error shapes over the pairing: network, aborted, timedOut', async () => {
  const realFetch = globalThis.fetch;
  const ch = new FakeChannel();
  const client = new ApiClient((f) => ch.send(f));
  globalThis.fetch = (url, init) => client.fetch(url, init);
  try {
    const p = hookedFetch('/v1/x', {});
    client.failAll();
    await assert.rejects(p, (e) => e.network && !e.aborted && !e.timedOut && e.cause instanceof TypeError);
    const ac = new AbortController();
    const q = hookedFetch('/v1/y', { signal: ac.signal });
    ac.abort();
    await assert.rejects(q, (e) => e.network && e.aborted);
    const tc = new AbortController();
    const t = hookedFetch('/v1/z', { signal: tc.signal });
    tc.abort('timeout');
    await assert.rejects(t, (e) => e.network && e.timedOut && !e.aborted);
  } finally {
    globalThis.fetch = realFetch;
  }
});

test('request headers: lower-case, token dropped, from objects, Headers and pairs', () => {
  assert.deepEqual(requestHeaders({ 'X-Visivo-Token': 'x', A: '1' }), { a: '1' });
  assert.deepEqual(requestHeaders(new Headers({ B: '2', 'x-visivo-token': 'y' })), { b: '2' });
  assert.deepEqual(requestHeaders([['C', '3']]), { c: '3' });
  assert.deepEqual(requestHeaders(undefined), {});
});

// ── WebSocket over a ws channel ────────────────────────────────────────────

test('ws: opens on the backend\'s open frame, sends and receives fragmented messages', async () => {
  const ch = new FakeChannel();
  const ws = new ChannelWebSocket('wss://h/v1/render/s/ws', ch);
  ws.binaryType = 'arraybuffer';
  const events = [];
  ws.onopen = () => events.push('open');
  ws.addEventListener('message', (e) => events.push(e.data));
  assert.equal(ws.readyState, ChannelWebSocket.CONNECTING);
  assert.throws(() => ws.send('x'), /CONNECTING/);
  ch.deliver(wsControlFrame(WS_OPEN, { protocol: 'visivo.v1' }));
  assert.equal(ws.readyState, 1);
  assert.equal(ws.protocol, 'visivo.v1');
  ws.send(JSON.stringify({ type: 'stream' }));
  const sent = decodeWsFrame(ch.sent[0]);
  assert.equal(sent.type, WS_TEXT);
  assert.equal(sent.last, true);
  const frame = bytes(300 * 1024, 5);            // a VXR1 frame larger than 256 KiB
  for (const f of wsMessageFrames(frame)) ch.deliver(f);
  ch.deliver(wsMessageFrames('{"stream":"on"}')[0]);
  assert.equal(events[0], 'open');
  assert.deepEqual(new Uint8Array(events[1]), frame);
  assert.equal(events[2], '{"stream":"on"}');
  ws.send(bytes(40000));
  assert.equal(ch.sent.length, 1 + 3);
  assert.ok(ch.sent.every((f) => f.byteLength <= CHUNK_BYTES + 2));
});

test('ws: binaryType blob gives Blobs', async () => {
  const ch = new FakeChannel();
  const ws = new ChannelWebSocket('u', ch);
  let got = null;
  ws.onmessage = (e) => { got = e.data; };
  ch.deliver(wsControlFrame(WS_OPEN, { protocol: '' }));
  ch.deliver(wsMessageFrames(Uint8Array.of(1, 2, 3))[0]);
  assert.ok(got instanceof Blob);
  assert.deepEqual([...new Uint8Array(await got.arrayBuffer())], [1, 2, 3]);
});

test('ws: the backend closes with a code; the page echoes it once', async () => {
  const ch = new FakeChannel();
  const ws = new ChannelWebSocket('u', ch);
  let closed = null;
  let errors = 0;
  ws.onerror = () => errors++;
  ws.addEventListener('close', (e) => { closed = e; });
  ch.deliver(wsControlFrame(WS_OPEN, { protocol: 'visivo.v1' }));
  ch.deliver(wsControlFrame(WS_CLOSE, { code: 1013, reason: 'in use' }));
  assert.equal(ws.readyState, 3);
  assert.deepEqual([closed.code, closed.reason, closed.wasClean], [1013, 'in use', true]);
  assert.equal(errors, 0);
  const echo = decodeWsFrame(ch.sent.at(-1));
  assert.equal(echo.type, WS_CLOSE);
  assert.deepEqual(JSON.parse(new TextDecoder().decode(echo.payload)), { code: 1013, reason: 'in use' });
  ws.send('ignored after close');
  assert.equal(ch.sent.length, 1);
  await tick();
  assert.equal(ch.readyState, 'closed');
});

test('ws: the page closes; closed when the backend echoes', async () => {
  const ch = new FakeChannel();
  const ws = new ChannelWebSocket('u', ch);
  let closed = null;
  ws.onclose = (e) => { closed = e; };
  ch.deliver(wsControlFrame(WS_OPEN, { protocol: 'visivo.v1' }));
  ws.close(1000, 'bye');
  assert.equal(ws.readyState, 2);
  const f = decodeWsFrame(ch.sent.at(-1));
  assert.deepEqual([f.type, f.last], [WS_CLOSE, true]);
  ch.deliver(wsControlFrame(WS_CLOSE, { code: 1000, reason: 'bye' }));
  assert.equal(ws.readyState, 3);
  assert.equal(closed.code, 1000);
  assert.equal(ch.sent.length, 1, 'no second close frame');
});

test('ws: a channel that closes without a close frame is an abnormal closure (1006, error first)', () => {
  const ch = new FakeChannel();
  const ws = new ChannelWebSocket('u', ch);
  const order = [];
  ws.onerror = () => order.push('error');
  ws.onclose = (e) => order.push(`close ${e.code} ${e.wasClean}`);
  ch.close();       // the handshake refused, or the link gone
  assert.deepEqual(order, ['error', 'close 1006 false']);
  assert.equal(ws.readyState, 3);
});

test('ws: a broken frame fails the socket', () => {
  const ch = new FakeChannel();
  const ws = new ChannelWebSocket('u', ch);
  let code = 0;
  ws.onclose = (e) => { code = e.code; };
  ch.deliver(wsControlFrame(WS_OPEN, { protocol: '' }));
  ch.deliver(new Uint8Array([7]).buffer);
  assert.equal(code, 1006);
  assert.equal(ch.readyState, 'closed');
});

test('ws: sends are paced like the api channel', () => {
  const ch = new FakeChannel();
  const ws = new ChannelWebSocket('u', ch);
  ch.deliver(wsControlFrame(WS_OPEN, { protocol: '' }));
  ch.bufferedAmount = HIGH_WATER_BYTES + 10;
  ws.send(bytes(50000));
  assert.equal(ch.sent.length, 0);
  assert.ok(ws.bufferedAmount >= 50000);
  ch.drain(0);
  assert.equal(ch.sent.length, 4);
  ws.sender.close();
});

// ── The page's side: addresses ─────────────────────────────────────────────

test('paired mode: the app directory stands for the backend root', () => {
  const loc = { href: 'https://vr.example.org/tools/vr-loader/app/k3j4l5m6/vr/?transport=pair', host: 'vr.example.org',
    pathname: '/tools/vr-loader/app/k3j4l5m6/vr/' };
  assert.equal(backendPath('/tools/vr-loader/app/k3j4l5m6/v1/vr/info', loc), '/v1/vr/info');
  assert.equal(backendPath('/tools/vr-loader/app/k3j4l5m6/v1/files/list?path=%2Fa', loc), '/v1/files/list?path=%2Fa');
  assert.equal(backendPath('wss://vr.example.org/tools/vr-loader/app/k3j4l5m6/v1/render/s/ws', loc), '/v1/render/s/ws');
  assert.equal(backendPath('https://elsewhere.org/tools/vr-loader/app/k3j4l5m6/v1/x', loc), null);
  assert.equal(backendPath('/tools/other/v1/x', loc), null);
  assert.equal(pairedRequested('?debug&transport=pair'), true);
  assert.equal(pairedRequested('?transport=direct'), false);
  assert.equal(pairedRequested(''), false);
});

test('paired sockets: no token in the query, no token subprotocol', () => {
  assert.deepEqual(socketRequest('/v1/render/s/ws?token=abc&x=1', undefined), { path: '/v1/render/s/ws?x=1', protocols: [] });
  assert.deepEqual(socketRequest('/v1/render/s/ws', ['visivo.v1', 'visivo-token.secret']), { path: '/v1/render/s/ws', protocols: ['visivo.v1'] });
});

test('the loader ships the same transport files as the page', () => {
  for (const f of ['pair-wire.js', 'pair-transport.js']) {
    const page = readFileSync(fileURLToPath(new URL(`../../app/static/vr/js/${f}`, import.meta.url)), 'utf8');
    const loader = readFileSync(fileURLToPath(new URL(`../../../web/vr-loader/js/${f}`, import.meta.url)), 'utf8');
    assert.equal(loader, page, `web/vr-loader/js/${f} differs from the page's: copy it again`);
  }
});
