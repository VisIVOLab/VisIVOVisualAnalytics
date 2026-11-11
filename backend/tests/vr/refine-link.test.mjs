// The refinement with a cluster backend (issue #187), without GL or a backend:
// the page under a path prefix, the window widened over a far link, the HUD
// for colour-only frames. node --test backend/tests/vr/refine-link.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';

const sockets = [];
class FakeSocket {
  static OPEN = 1;
  constructor(url) { this.url = url; this.readyState = 0; this.sent = []; this.l = {}; sockets.push(this); }
  addEventListener(k, f) { (this.l[k] ||= []).push(f); }
  send(m) { this.sent.push(JSON.parse(m)); }
  close() { this.readyState = 3; for (const f of this.l.close || []) f({ code: 1000 }); }
  open() { this.readyState = 1; for (const f of this.l.open || []) f({}); }
}
// The page as a reverse proxy serves it: under /portal/visivo.
globalThis.window = { location: { protocol: 'http:', host: 'localhost:18900', pathname: '/portal/visivo/vr/' } };
globalThis.WebSocket = FakeSocket;
const deleted = [];
globalThis.fetch = async (url, init) => { if (init && init.method === 'DELETE') deleted.push(url); return { ok: true }; };

const { Refiner } = await import('../../app/static/vr/js/refine-client.js');

const tick = () => new Promise((r) => setTimeout(r, 0));
const backend = {
  token: 't', headers: () => ({}),
  _json: async (method) => (method === 'DELETE' ? {} : { valid: true, session_id: 'sP' }),
};

/** A VXR1 message for `poseId`, colour-only or with a (fake) depth. */
function vxr1(poseId, timing, { depth = false } = {}) {
  const color = new Uint8Array([0xff, 0xd8, 0xff, 0xd9]);
  const header = {
    v: 1, pose_id: poseId, eyes: [{ eye: 'left', x: 0, y: 0, width: 2, height: 2 }, { eye: 'right', x: 2, y: 0, width: 2, height: 2 }],
    color: { format: 'jpeg', width: 4, height: 2, offset: 0, length: color.length },
    depth: depth ? { width: 2, height: 1, offset: color.length, length: 0 } : null,
    reprojection: depth ? 'depth' : 'pose',
    depth_unavailable: depth ? null : 'the volume mapper gave no depth image (empty image)',
    timing_ms: timing,
  };
  const h = new TextEncoder().encode(JSON.stringify(header));
  const out = new Uint8Array(8 + h.length + color.length);
  out.set([0x56, 0x58, 0x52, 0x31]);
  new DataView(out.buffer).setUint32(4, h.length, true);
  out.set(h, 8);
  out.set(color, 8 + h.length);
  return out.buffer;
}

async function connected(opts = {}) {
  sockets.length = 0;
  const r = new Refiner(null, () => backend, { ...opts, draw: { forget() {}, shown: null, upload() { return 0; }, draw() { return false; } } });
  r.setEnabled(true);
  await r.setDataset('D');
  await tick(); await tick();
  const ws = sockets[0];
  ws.open();
  r._onMessage(JSON.stringify({ stream: 'on', window: 2, max_fps: 72 }));
  return { r, ws };
}

test('under a path prefix the page talks to the prefixed API', async () => {
  deleted.length = 0;
  const { r, ws } = await connected();
  assert.equal(ws.url, 'ws://localhost:18900/portal/visivo/v1/render/sP/ws?token=t');
  assert.deepEqual(ws.sent[0], { type: 'stream', window: 2, max_fps: 72 });
  r.setEnabled(false);
  await tick();
  assert.deepEqual(deleted, ['/portal/visivo/v1/render/sP']);
});

test('over a far link the window widens (and the eye is capped when asked)', async () => {
  const { r, ws } = await connected({ link: { remoteMaxEyePx: 768 } });
  const eyes = [{ eye: 'left', width: 2064, height: 2208 }, { eye: 'right', width: 2064, height: 2208 }];
  assert.deepEqual(r._eyeSize(eyes), [2064, 2208]);
  const t0 = 1000;
  for (let i = 0; i < 12; i++) {
    const id = r.book.add({ tSent: t0, version: r.version, eyes: [], model: null, aCentre: null });
    // performance.now() - tSent ~ the measured 122 ms at 512^2 on PLEIADI.
    const realNow = performance.now;
    performance.now = () => t0 + 122;
    try { r._onMessage(vxr1(id, { render: 28, depth: 2.2, encode: 4.5, total: 41 })); } finally { performance.now = realNow; }
  }
  await tick();
  const reconf = ws.sent.filter((m) => m.type === 'stream');
  assert.equal(reconf.length, 2, JSON.stringify(reconf));          // the first, then one change
  assert.deepEqual(reconf[1], { type: 'stream', window: 4, max_fps: 72 });
  assert.equal(r._window(), 4);
  const [w, h] = r._eyeSize(eyes);
  assert.ok(Math.max(w, h) <= 768 && Math.abs(w / h - 2064 / 2208) < 0.01, `${w}x${h}`);
  assert.equal(r.summary().link.remote, true);
  assert.ok(r.hudLines().some((l) => /link far rtt \d+ ms {2}window 4 {2}eye <= 768 px/.test(l)));
  r.setEnabled(false);
});

test('by default the eye keeps its size over a far link', async () => {
  const { r } = await connected();
  for (let i = 0; i < 12; i++) {
    const id = r.book.add({ tSent: 1000, version: r.version, eyes: [], model: null, aCentre: null });
    const realNow = performance.now;
    performance.now = () => 1122;
    try { r._onMessage(vxr1(id, { render: 28, depth: 2.2, encode: 4.5, total: 41 })); } finally { performance.now = realNow; }
  }
  assert.equal(r.linkState.remote, true);
  assert.equal(r._window(), 4);
  assert.deepEqual(r._eyeSize([{ width: 2064, height: 2208 }]), [2064, 2208]);
  r.setEnabled(false);
});

test('a colour-only frame on show is named in the HUD', async () => {
  const { r } = await connected();
  r.status = 'refined';
  r.draw.shown = { header: JSON.parse(new TextDecoder().decode(new Uint8Array(vxr1(1, {})).slice(8, -4))), rec: { tSent: 0, version: r.version } };
  const hud = r.hudLines();
  const i = hud.indexOf('  reprojection: pose only (no depth)');
  assert.ok(i > 0, JSON.stringify(hud));
  assert.match(hud[i + 1], /^ {4}why: the volume mapper gave no depth image/);
  assert.equal(r.summary().reprojection.mode, 'pose');
  r.setEnabled(false);
});

test('adaptation can be turned off', async () => {
  const r = new Refiner(null, () => backend, { adaptLink: false, draw: { forget() {}, shown: null } });
  r.linkState = { window: 6, maxEyePx: 512 };
  assert.equal(r._window(), 2);
  assert.deepEqual(r._eyeSize([{ width: 1000, height: 1000 }]), [1000, 1000]);
});

test('a new session measures the link afresh', async () => {
  const { r } = await connected();
  r.linkState = { window: 5, maxEyePx: Infinity, remote: true };
  r.link.window = 5; r.link.remote = true; r.link.transport.push(90);
  await r.setDataset('E');
  assert.equal(r.link.transport.length, 0);
  assert.equal(r._window(), 2);
  assert.equal(r.linkState.remote, false);
  r.setEnabled(false);
});

test('the refinement holds a frame without depth to the 1 cm gate', async () => {
  const { r } = await connected();
  const ident = () => new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -0.75, 1]);
  const ctx = (t, dx) => {
    const a = ident(); a[12] = dx;
    return { t, eyes: [], aCentre: a, model: ident(), zStretch: 1, lookKey: 'k',
      look: { tf: { type: 'set_transfer_functions' }, crop: { type: 'set_crop' } } };
  };
  r.paused = true;                                      // no poses: only the gate is under test
  const run = (header, dx) => {
    r.frame(ctx(0, 0));                                 // the look's version
    r.weight = 0;
    r.draw.shown = { header, rec: { tSent: 0, version: r.version, aCentre: ident() } };
    let t = 16;
    for (let i = 0; i < 60; i++, t += 16) r.frame(ctx(t + 1000 * run.n, dx));
    run.n++;
    return r.weight;
  };
  run.n = 1;
  const withDepth = { reprojection: 'depth', depth: { width: 2, height: 1 } };
  const colourOnly = { reprojection: 'pose', depth: null, depth_unavailable: 'x' };
  // 2 cm from the frame's pose: shown with depth, not without; 5 mm: both.
  assert.ok(run(withDepth, 0.02) > 0.5);
  assert.equal(run(colourOnly, 0.02), 0);
  assert.ok(run(colourOnly, 0.005) > 0.5);
  r.setEnabled(false);
});
