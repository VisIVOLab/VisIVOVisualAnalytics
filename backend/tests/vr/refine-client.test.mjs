// The refinement's session lifecycle (issue #170), without GL or a backend:
// node --test backend/tests/vr/refine-client.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';

// The page's globals, as stand-ins: the address, the socket, fetch (DELETE).
const sockets = [];
class FakeSocket {
  static OPEN = 1;
  constructor(url, protocols) { this.url = url; this.protocols = protocols; this.readyState = 0; this.sent = []; this.l = {}; sockets.push(this); }
  addEventListener(k, f) { (this.l[k] ||= []).push(f); }
  send(m) { this.sent.push(JSON.parse(m)); }
  close() { this.readyState = 3; for (const f of this.l.close || []) f({ code: 1000 }); }
  open() { this.readyState = 1; for (const f of this.l.open || []) f({}); }
}
globalThis.window = { location: { protocol: 'http:', host: 'localhost:8000' } };
globalThis.WebSocket = FakeSocket;
const deleted = [];
globalThis.fetch = async (url, init) => { if (init && init.method === 'DELETE') deleted.push(url); return { ok: true }; };

const { Refiner } = await import('../../app/static/vr/js/refine-client.js');

const tick = () => new Promise((r) => setTimeout(r, 0));
const stubDraw = { forget() {}, shown: null, upload() { return 0; }, draw() { return false; } };

/** A backend whose create_session answers when the test says so. */
function slowBackend() {
  const pending = [];
  return {
    token: 't', headers: () => ({}), pending,
    _json(method, path, body) {
      if (method === 'DELETE') return Promise.resolve({ cancelled: true });
      return new Promise((resolve) => pending.push({ body, resolve }));
    },
  };
}

test('a cube change while its session is being created connects the new cube', async () => {
  sockets.length = 0; deleted.length = 0;
  const b = slowBackend();
  const r = new Refiner(null, () => b, { draw: stubDraw });
  r.setEnabled(true);
  await r.setDataset('A');
  await tick();
  assert.equal(b.pending.length, 1);
  assert.equal(b.pending[0].body.dataset_id, 'A');
  // B arrives while A's create_session is still running.
  await r.setDataset('B');
  await tick();
  b.pending[0].resolve({ valid: true, session_id: 'sA' });
  await tick(); await tick();
  // A's session is not wanted any more: deleted; B's is asked for.
  assert.ok(deleted.includes('/v1/render/sA'), JSON.stringify(deleted));
  assert.equal(b.pending.length, 2);
  assert.equal(b.pending[1].body.dataset_id, 'B');
  b.pending[1].resolve({ valid: true, session_id: 'sB' });
  await tick(); await tick();
  assert.equal(sockets.length, 1);
  // The token rides as a subprotocol, never in the address (access logs).
  assert.match(sockets[0].url, /\/v1\/render\/sB\/ws$/);
  assert.deepEqual(sockets[0].protocols, ["visivo.v1", "visivo-token.t"]);
});

test('turning refinement off frees the render slot; on again makes a new session', async () => {
  sockets.length = 0; deleted.length = 0;
  const b = slowBackend();
  const r = new Refiner(null, () => b, { draw: stubDraw });
  r.setEnabled(true);
  await r.setDataset('A');
  await tick();
  b.pending[0].resolve({ valid: true, session_id: 's1' });
  await tick(); await tick();
  sockets[0].open();
  r.lastSentA = new Float32Array(16);
  r.setEnabled(false);
  await tick();
  assert.ok(deleted.includes('/v1/render/s1'));
  assert.equal(sockets[0].readyState, 3);
  assert.equal(sockets.length, 1);                 // no reconnect after the close
  r.setEnabled(true);
  assert.equal(r.lastSentA, null);                 // a still view asks for a frame again
  await tick();
  assert.equal(b.pending.length, 2);
});

test('a cube drawn out of core asks for no session at all', async () => {
  const b = slowBackend();
  const r = new Refiner(null, () => b, { draw: stubDraw });
  r.setEnabled(true);
  await r.setDataset('big', { outOfCore: true });
  await tick();
  assert.equal(b.pending.length, 0);
  assert.match(r.hudLines()[0], /out of core/);
});

test('a backend that starts preparing a pyramid is not followed', async () => {
  const b = slowBackend();
  const r = new Refiner(null, () => b, { draw: stubDraw });
  r.setEnabled(true);
  await r.setDataset('big');
  await tick();
  b.pending[0].resolve({ valid: false, preparing: true, progress: 0 });
  await tick(); await tick();
  assert.match(r.unavailable, /out of core/);
  await new Promise((res) => setTimeout(res, 20));
  assert.equal(b.pending.length, 1);                // not asked again
});

test('an out-of-core refusal on the socket closes the session', async () => {
  sockets.length = 0; deleted.length = 0;
  const b = slowBackend();
  const r = new Refiner(null, () => b, { draw: stubDraw });
  r.setEnabled(true);
  await r.setDataset('A');
  await tick();
  b.pending[0].resolve({ valid: true, session_id: 's9' });
  await tick(); await tick();
  sockets[0].open();
  r._onMessage(JSON.stringify({ type: 'xr_error', pose_id: 1, error: 'Stereo frames are not available yet for a cube drawn out of core.' }));
  await tick();
  assert.ok(deleted.includes('/v1/render/s9'));
  assert.equal(sockets[0].readyState, 3);
  assert.equal(sockets.length, 1);
});

test('suspended (passthrough) closes the render session; resuming makes a new one', async () => {
  sockets.length = 0; deleted.length = 0;
  const b = slowBackend();
  const r = new Refiner(null, () => b, { draw: stubDraw });
  r.setEnabled(true);
  await r.setDataset('A');
  await tick();
  b.pending[0].resolve({ valid: true, session_id: 'sP' });
  await tick(); await tick();
  sockets[0].open();
  r.setSuspended('in passthrough');
  r.setSuspended('in passthrough');                // every frame: one close only
  await tick();
  assert.deepEqual(deleted, ['/v1/render/sP']);
  assert.equal(sockets[0].readyState, 3);
  await new Promise((res) => setTimeout(res, 20));
  assert.equal(b.pending.length, 1);               // nothing asked while suspended
  assert.equal(sockets.length, 1);                 // and no reconnect
  assert.match(r.hudLines()[0], /passthrough/);
  r.setSuspended('');
  await tick();
  assert.equal(b.pending.length, 2);
  assert.equal(b.pending[1].body.dataset_id, 'A');
});

test('a stale failed attempt does not write the error of the session on show', async () => {
  const b = slowBackend();
  const r = new Refiner(null, () => b, { draw: stubDraw });
  r.setEnabled(true);
  await r.setDataset('A');
  await tick();
  const first = b.pending[0];
  await r.setDataset('B');                         // A's attempt is now stale
  await tick();
  first.resolve(Promise.reject(new Error('500: boom')));   // A fails late
  await tick(); await tick();
  assert.equal(r.error, '');
  assert.equal(r.unavailable, '');
});

test('a session that gives no stereo frames is told by its code, not by a word', async () => {
  const { stereoRefused } = await import('../../app/static/vr/js/refine-client.js');
  assert.equal(stereoRefused({ code: 'out_of_core', error: 'x' }), true);
  assert.equal(stereoRefused({ code: 'unsupported', error: 'x' }), true);
  assert.equal(stereoRefused({ error: 'Nothing to draw: every point is filtered out.' }), false);
  assert.equal(stereoRefused({ code: 'other', error: 'drawn out of core' }), false);
  assert.equal(stereoRefused({ error: 'Stereo frames are not available yet for a cube drawn out of core' }), true);
  assert.equal(stereoRefused({ error: 'Stereo frames are drawn for cubes only.' }), true);
});

test('a frame decoded after a cube change is never shown on the new cube', async () => {
  sockets.length = 0; deleted.length = 0;
  const b = slowBackend();
  const r = new Refiner(null, () => b, { draw: stubDraw });
  r.setEnabled(true);
  await r.setDataset('A');
  await tick();
  b.pending[0].resolve({ valid: true, session_id: 'sA' });
  await tick(); await tick();
  sockets[0].open();
  // A pose asked of A's session; its frame is being decoded when the cube changes.
  const id = r.book.add({ tSent: 0, version: r.version, eyes: [], model: new Float32Array(16),
    aCentre: new Float32Array(16), gen: r.gen, datasetId: r.datasetId, sessionId: r.sessionId });
  const rec = r.book.take(id);
  let release;
  const closed = [];
  globalThis.createImageBitmap = () => new Promise((res) => { release = () => res({ close() { closed.push(1); } }); });
  const decoding = r._decode({ header: {}, color: new Uint8Array(4), depth: null }, rec, 0);
  await tick();
  r.setDataset('B');                    // same view, same look version: only the cube changed
  release();
  await decoding;
  assert.equal(r.incoming, null, 'not queued for upload on B');
  assert.equal(closed.length, 1, 'the decoded image is released');
  // The same frame for the cube still on show is kept.
  const id2 = r.book.add({ tSent: 0, version: r.version, eyes: [], model: new Float32Array(16),
    aCentre: new Float32Array(16), gen: r.gen, datasetId: r.datasetId, sessionId: r.sessionId });
  const rec2 = r.book.take(id2);
  globalThis.createImageBitmap = async () => ({ close() {} });
  await r._decode({ header: {}, color: new Uint8Array(4), depth: null }, rec2, 0);
  assert.ok(r.incoming && r.incoming.rec.id === id2);
  delete globalThis.createImageBitmap;
  r.setEnabled(false);                  // no keepalive timer left behind
});

test('a late close of the old socket does not stop the new stream', async () => {
  sockets.length = 0; deleted.length = 0;
  const b = slowBackend();
  const r = new Refiner(null, () => b, { draw: stubDraw });
  r.setEnabled(true);
  await r.setDataset('A');
  await tick();
  b.pending[0].resolve({ valid: true, session_id: 'sA' });
  await tick(); await tick();
  const old = sockets[0];
  old.open();
  // The old socket will report its close only later.
  old.close = function () { this.readyState = 2; };
  r.setDataset('B');
  await tick(); await tick();
  b.pending[1].resolve({ valid: true, session_id: 'sB' });
  await tick(); await tick();
  const fresh = sockets[1];
  fresh.open();
  for (const f of fresh.l.message || []) f({ data: JSON.stringify({ stream: 'on' }) });
  assert.equal(r.streaming, true);
  r.book.add({ tSent: 1e12, version: r.version, gen: r.gen });
  // Now the old socket's events arrive: close, and a stray message.
  for (const f of old.l.message || []) f({ data: JSON.stringify({ stream: 'on' }) });
  for (const f of old.l.close || []) f({ code: 1006 });
  assert.equal(r.ws, fresh, 'the new socket is kept');
  assert.equal(r.streaming, true, 'still streaming');
  assert.equal(r.book.outstanding(0), 1, 'the new stream\'s poses are kept');
  assert.notEqual(r.status, 'reconnecting');
  r.setEnabled(false);                  // no keepalive timer left behind
});

/** A VXR1 message carrying only a header whose colour comes as video. */
function videoHeader(id) {
  const header = { v: 1, pose_id: id, depth: null, timing_ms: { total: 4 }, video: { id },
    eyes: [], color: { format: 'h264-webrtc', width: 128, height: 64, offset: 0, length: 0,
      stripe: { y: 64, rows: 16, cells: 48, cell: 2, id_bits: 32 } } };
  const json = new TextEncoder().encode(JSON.stringify(header));
  const buf = new Uint8Array(8 + json.length);
  buf.set([0x56, 0x58, 0x52, 0x31]);
  new DataView(buf.buffer).setUint32(4, json.length, true);
  buf.set(json, 8);
  return buf.buffer;
}

/** A refiner streaming over WebRTC video, with a still view to drive. */
async function stillOverVideo() {
  sockets.length = 0; deleted.length = 0;
  const b = slowBackend();
  const r = new Refiner(null, () => b, { draw: stubDraw, transport: 'webrtc' });
  r.setEnabled(true);
  await r.setDataset('A');
  await tick();
  b.pending[0].resolve({ valid: true, session_id: 'sA' });
  await tick(); await tick();
  const ws = sockets[0];
  ws.open();
  for (const f of ws.l.message || []) f({ data: JSON.stringify({ stream: 'on' }) });
  r.rtc.state = 'video';                         // the link is up
  r.rtc.tick = () => {};                         // its watchdogs are driven by hand here
  const ident = () => new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -0.75, 1]);
  const eye = (e) => ({ eye: e, view: ident(), proj: ident(), width: 64, height: 64 });
  const ctx = (t) => ({ t, eyes: [eye('left'), eye('right')], aCentre: ident(), model: ident(), zStretch: 1,
    lookKey: 'k', look: { tf: { type: 'set_transfer_functions' }, crop: { type: 'set_crop' } } });
  let t = 1000;
  return {
    r, ws,
    poses: () => ws.sent.filter((m) => m.type === 'xr_pose'),
    still: (n) => { for (let i = 0; i < n; i++, t += 16) r.frame(ctx(t)); },
  };
}

test('a video given up asks a still view for a fresh pose, as JPEG', async () => {
  const { r, ws, poses, still } = await stillOverVideo();
  still(120);                                    // two seconds still: one pose asked
  assert.equal(poses().length, 1);
  // Its header comes as video: the record is taken, the header waits for the frame.
  const id = poses()[0].pose_id;
  r._onMessage(videoHeader(id));
  assert.equal(r.book.take(id), null, 'the pose record went with its header');
  still(30);
  assert.equal(poses().length, 1, 'a still view asks nothing more while the video may come');
  // The video never decodes: given up, back to JPEG. The JPEG of that same
  // pose would find no record; the still view must ask again.
  r.rtc._fail('no video frame decoded for 3 s');
  assert.ok(ws.sent.some((m) => m.type === 'xr_rtc_stop'));
  still(30);
  assert.equal(poses().length, 2, 'a fresh pose is asked after the fallback');
  r.setEnabled(false);
});

test('a video header that arrives after the video was given up asks again too', async () => {
  const { r, poses, still } = await stillOverVideo();
  still(120);
  assert.equal(poses().length, 1);
  const id = poses()[0].pose_id;
  r.rtc._fail('WebRTC failed');
  still(30);
  const asked = poses().length;                  // asked again at the failure
  assert.equal(asked, 2);
  // The header of the pose in flight at the failure comes late: the video
  // no longer takes it, and that pose is never shown.
  r._onMessage(videoHeader(id));
  assert.equal(r.book.take(id), null);
  still(30);
  assert.equal(poses().length, asked + 1, 'the still view asks again');
  r.setEnabled(false);
});
