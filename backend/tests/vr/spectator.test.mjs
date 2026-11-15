// The headset's view on the desktop (issue #281): the page's side.
// node --test backend/tests/vr/spectator.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import * as S from '../../app/static/vr/js/spectator.js';
import { flipRows as workerFlip } from '../../app/static/vr/js/spectator-worker.js';

// ── Pure parts ─────────────────────────────────────────────────────────────

test('watch command: bounded fps and width, on only when it says true', () => {
  assert.deepEqual(S.watchParams({ on: true, fps: 24, width: 640 }), { on: true, fps: 24, width: 640 });
  assert.deepEqual(S.watchParams({ on: true }), { on: true, fps: S.DEFAULT_FPS, width: S.DEFAULT_WIDTH });
  assert.deepEqual(S.watchParams({ on: true, fps: 500, width: 10 }), { on: true, fps: S.MAX_FPS, width: S.MIN_WIDTH });
  assert.deepEqual(S.watchParams({ on: 'yes', fps: 'x', width: 1e9 }), { on: false, fps: S.DEFAULT_FPS, width: S.MAX_WIDTH });
  assert.equal(S.watchUsable({ seq: 3, type: 'watch', on: false }), true);
  assert.equal(S.watchUsable({ seq: 3, type: 'watch', on: 1 }), false);
  assert.equal(S.watchUsable({ seq: 0, type: 'watch', on: true }), false);
  assert.equal(S.watchUsable({ seq: 3, type: 'show', on: true }), false);
});

test('capture rect: the crop around the line of sight, inside the eye, scaled to the width asked for', () => {
  // A symmetric frustum: the middle of the viewport.
  const sym = new Float32Array(16);
  let r = S.captureRect({ x: 0, y: 0, width: 1000, height: 800 }, sym, 0.8, 400);
  assert.deepEqual(r.src, [100, 80, 900, 720]);
  assert.deepEqual([r.w, r.h], [400, 320]);
  // The whole eye's view on the copy: the crop (800 wide) fills the 400 px.
  assert.deepEqual(r.panelViewport, [-50, -40, 500, 400]);
  // The right half of a side-by-side framebuffer (the left eye's x offset 0, the right 1000).
  r = S.captureRect({ x: 1000, y: 0, width: 1000, height: 800 }, sym, 0.8, 400);
  assert.deepEqual(r.src, [1100, 80, 1900, 720]);
  assert.deepEqual(r.panelViewport, [-50, -40, 500, 400]);
  // A headset's asymmetric frustum: straight ahead lands at ndc (-P[8], -P[9]).
  const asym = new Float32Array(16);
  asym[8] = -0.1; asym[9] = 0.05;      // the line of sight at ndc (0.1, -0.05)
  r = S.captureRect({ x: 0, y: 0, width: 1000, height: 800 }, asym, 0.8, 400);
  assert.deepEqual(r.src, [150, 60, 950, 700]);
  // Kept inside the viewport however far the axis is.
  asym[8] = -0.9;
  r = S.captureRect({ x: 0, y: 0, width: 1000, height: 800 }, asym, 0.8, 400);
  assert.equal(r.src[2], 1000);
  assert.equal(r.src[0], 200);
  // Never wider than the crop; even sizes; the 2-D page keeps everything (crop 1, no projection).
  r = S.captureRect({ x: 0, y: 0, width: 301, height: 201 }, null, 1, 960);
  assert.deepEqual(r.src, [0, 0, 301, 201]);
  assert.deepEqual([r.w % 2, r.h % 2], [0, 0]);
  assert.ok(r.w <= 301 && Math.abs(r.w / r.h - 301 / 201) < 0.02);
});

test('flipRows: GL\'s bottom row first becomes the image\'s top row; the worker does the same', () => {
  const w = 3, h = 4;
  const src = new Uint8Array(w * h * 4).map((_, i) => i);
  const a = S.flipRows(src, w, h, new Uint8Array(src.length));
  const b = workerFlip(src, w, h, new Uint8Array(src.length));
  assert.deepEqual(Array.from(a.subarray(0, 12)), Array.from(src.subarray(36, 48)));
  assert.deepEqual(Array.from(a.subarray(36, 48)), Array.from(src.subarray(0, 12)));
  assert.deepEqual(a, b);
});

test('pacer, ring, rate, query', () => {
  const p = new S.Pacer(15);
  assert.ok(p.due(0));
  p.mark(0);
  // 72 Hz frames: the first at or after 66.7 ms (2 ms slack).
  assert.equal(p.due(55.6), false);
  assert.equal(p.due(69.4), true);
  const r = new S.Ring(3);
  for (const v of [1, 2, 3, 10, NaN]) r.push(v);
  assert.equal(r.mean, 5);
  assert.equal(r.max, 10);
  assert.equal(S.rateOf([0, 100, 200, 300], 300).toFixed(1), '10.0');
  assert.equal(S.rateOf([0], 10), 0);
  assert.equal(S.rateOf([0, 100], 5000), 0, 'old frames do not count');
  assert.equal(S.frameQuery('vr-1', 'left', 0.4567).toString(), 'client=vr-1&eye=left&cost_ms=0.457');
  assert.equal(S.frameQuery('vr-1', 'bogus').toString(), 'client=vr-1&eye=left');
  assert.equal(S.frameQuery('vr-1', 'mono', 5000).get('cost_ms'), '1000.000');
});

test('badge model: head pose times the offset (upper left, in front)', () => {
  const head = new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 2, 1.6, -1, 1]);
  const m = S.badgeModel(head, [-0.1, 0.1, -0.5]);
  assert.deepEqual(Array.from(m.subarray(12, 15)).map((v) => +v.toFixed(5)), [1.9, 1.7, -1.5]);
  // Turned 90° about y: "in front" is along the head's -z (world -x).
  const turned = new Float32Array([0, 0, -1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1]);
  const t = S.badgeModel(turned, [0, 0, -0.5]);
  assert.deepEqual(Array.from(t.subarray(12, 15)).map((v) => +v.toFixed(5)), [-0.5, 0, 0]);
});

// ── The controller, with a fake GL ─────────────────────────────────────────

function fakeGL() {
  const calls = [];
  const C = { NO_ERROR: 0, ALREADY_SIGNALED: 0x911A, CONDITION_SATISFIED: 0x911C, TIMEOUT_EXPIRED: 0x911B, WAIT_FAILED: 0x911D,
    FRAMEBUFFER: 1, READ_FRAMEBUFFER: 2, DRAW_FRAMEBUFFER: 3, RENDERBUFFER: 4, RGBA8: 5, PIXEL_PACK_BUFFER: 6, STREAM_READ: 7,
    COLOR_ATTACHMENT0: 8, COLOR_BUFFER_BIT: 9, LINEAR: 10, RGBA: 11, UNSIGNED_BYTE: 12, PACK_ALIGNMENT: 13, SYNC_GPU_COMMANDS_COMPLETE: 14 };
  const gl = { ...C, signal: C.TIMEOUT_EXPIRED, error: 0, calls };
  const rec = (name, ret) => (...a) => { calls.push([name, ...a]); return typeof ret === 'function' ? ret(...a) : ret; };
  let ids = 0;
  Object.assign(gl, {
    createFramebuffer: rec('createFramebuffer', () => ({ fb: ++ids })),
    createRenderbuffer: rec('createRenderbuffer', () => ({ rb: ++ids })),
    createBuffer: rec('createBuffer', () => ({ buf: ++ids })),
    bindRenderbuffer: rec('bindRenderbuffer'), renderbufferStorage: rec('renderbufferStorage'),
    bindFramebuffer: rec('bindFramebuffer'), framebufferRenderbuffer: rec('framebufferRenderbuffer'),
    bindBuffer: rec('bindBuffer'), bufferData: rec('bufferData'), blitFramebuffer: rec('blitFramebuffer'),
    viewport: rec('viewport'), pixelStorei: rec('pixelStorei'), readPixels: rec('readPixels'),
    fenceSync: rec('fenceSync', () => ({ fence: ++ids })), flush: rec('flush'),
    clientWaitSync: rec('clientWaitSync', () => gl.signal), deleteSync: rec('deleteSync'),
    getBufferSubData: rec('getBufferSubData', (_t, src, dst, dstOff = 0, len = dst.length - dstOff) => { dst.fill(7, dstOff, dstOff + len); }),
    getError: rec('getError', () => { const e = gl.error; gl.error = 0; return e; }),
    deleteFramebuffer: rec('deleteFramebuffer'), deleteRenderbuffer: rec('deleteRenderbuffer'), deleteBuffer: rec('deleteBuffer'),
  });
  gl.names = () => calls.map((c) => c[0]);
  gl.reset = () => { calls.length = 0; };
  return gl;
}

async function flush() { for (let i = 0; i < 20; i++) await new Promise((r) => setImmediate(r)); }

/** Frames (14 ms apart) until the capture under way reaches the encoder, at most `max`. */
async function untilEncoded(h, src, max = 12) {
  const n0 = h.encodes.length;
  for (let i = 0; i < max && h.encodes.length === n0 && h.spectator.watching; i++) { h.t += 14; h.spectator.frame(src); }
  await flush();
}

function harness({ ack = { watched: true, stored: true, seq: 1, fps: 15, width: 512 }, status = 200, readAt = 'frame', hang = false } = {}) {
  const gl = fakeGL();
  const panels = [];
  const renderer = {
    drawPanel: (view, model, handle, size) => panels.push({ view, model, handle, size }),
    createPanel: () => ({ tex: { t: 1 }, version: 0 }), updatePanel: (h, c) => { h.canvas = c; h.version++; }, releasePanel: () => {},
  };
  const posts = [];
  const h = { gl, panels, posts, logs: [], status, ack, t: 1000, workers: [], released: [] };
  renderer.releasePanel = (handle) => h.released.push(handle);
  const encodes = [];
  h.spectator = S.createSpectator({
    gl, renderer, client: () => 'vr-test', auth: () => ({ token: 'tok', session: 'sess-1' }), readAt,
    log: (level, event, message) => h.logs.push([level, event, message]),
    now: () => h.t,
    // A document with a canvas for the badge (no body: no 2-D page line in node).
    document: { createElement: () => ({ width: 0, height: 0, getContext: () => new Proxy({}, { get: () => () => {} }) }) },
    // `hang`: the worker takes frames and never answers (a convertToBlob that hangs).
    makeWorker: () => {
      const w = {
        terminated: false,
        postMessage(m) { encodes.push(m); if (!h.hang) queueMicrotask(() => this.onmessage({ data: { id: m.id, jpeg: new Uint8Array([0xff, 0xd8, 0xff, 1]).buffer, buf: m.buf, ms: 3 } })); },
        terminate() { this.terminated = true; },
      };
      h.workers.push(w);
      return w;
    },
    fetch: async (url, init) => {
      posts.push({ url, init });
      return { status: h.status, json: async () => h.ack };
    },
  });
  h.encodes = encodes;
  h.hang = hang;
  return h;
}

const SRC = { fbo: { xr: 1 }, vp: { x: 0, y: 0, width: 1000, height: 800 }, proj: new Float32Array(16), eye: 'left',
  view: { proj: 'P', view: 'V' } };

test('not watched: a frame costs nothing, not even a GL call', () => {
  const h = harness();
  h.spectator.frame(SRC);
  assert.deepEqual(h.gl.calls, []);
  assert.equal(h.spectator.badge(new Float32Array(16)), null);
});

test('one capture: blit, panels, readPixels into a PBO and a fence; read back only once the fence passed; posted as JPEG', async () => {
  const h = harness();
  h.spectator.command({ on: true, fps: 15, width: 400 });
  const overlays = [{ id: 'menu', model: 'M', handle: { tex: {} }, size: [0.4, 0.3] }, { id: 'x', model: 'X', handle: { tex: null }, size: [1, 1] }];
  h.spectator.frame({ ...SRC, overlays });
  const n = h.gl.names();
  for (const name of ['renderbufferStorage', 'bufferData', 'blitFramebuffer', 'readPixels', 'fenceSync', 'flush']) assert.ok(n.includes(name), name);
  assert.ok(n.indexOf('blitFramebuffer') < n.indexOf('readPixels') && n.indexOf('readPixels') < n.indexOf('fenceSync'));
  const blit = h.gl.calls.find((c) => c[0] === 'blitFramebuffer');
  assert.deepEqual(blit.slice(1, 9), [100, 80, 900, 720, 0, 0, 400, 320]);
  assert.equal(blit[10], h.gl.LINEAR);
  // readPixels into the bound PBO (offset 0), never into client memory.
  const rp = h.gl.calls.find((c) => c[0] === 'readPixels');
  assert.deepEqual(rp.slice(1), [0, 0, 400, 320, h.gl.RGBA, h.gl.UNSIGNED_BYTE, 0]);
  // The panels with a texture, over the scene, with the whole-eye viewport; the frame's framebuffer back.
  assert.deepEqual(h.panels.map((p) => p.model), ['M']);
  assert.ok(h.gl.calls.some((c) => c[0] === 'viewport' && c[1] === -50 && c[3] === 500));
  const binds = h.gl.calls.filter((c) => c[0] === 'bindFramebuffer');
  assert.deepEqual(binds[binds.length - 1], ['bindFramebuffer', h.gl.FRAMEBUFFER, SRC.fbo]);
  assert.equal(h.spectator.st.stage, 'gpu');
  assert.ok(n.includes('getError'), 'the first capture of a watch is checked');
  // The fence has not passed: the next frames only ask, never wait, never read.
  h.gl.reset();
  h.t += 14;
  h.spectator.frame(SRC);
  assert.deepEqual(h.gl.names(), ['clientWaitSync']);
  assert.deepEqual(h.gl.calls[0].slice(2), [0, 0], 'a zero timeout');
  // Passed: the bytes out of the PBO in one copy (readAt 'frame' here), to the encoder, then posted.
  h.gl.signal = h.gl.ALREADY_SIGNALED;
  h.gl.reset();
  h.t += 14;
  h.spectator.frame(SRC);
  assert.deepEqual(h.gl.names().filter((x) => x !== 'bindBuffer'), ['clientWaitSync', 'deleteSync', 'getBufferSubData']);
  assert.equal(h.encodes.length, 1);
  assert.deepEqual([h.encodes[0].w, h.encodes[0].h, h.encodes[0].buf.byteLength], [400, 320, 400 * 320 * 4]);
  assert.ok(new Uint8Array(h.encodes[0].buf).every((v) => v === 7), 'every byte read');
  await flush();
  assert.equal(h.posts.length, 1);
  const { url, init } = h.posts[0];
  assert.match(url, /\/v1\/vr\/headset\/spectator\/frame\?client=vr-test&eye=left&cost_ms=/);
  assert.equal(init.method, 'POST');
  assert.equal(init.headers['Content-Type'], 'image/jpeg');
  assert.equal(init.headers['X-Visivo-Session'], 'sess-1');
  assert.ok(init.body instanceof ArrayBuffer);
  assert.equal(h.spectator.st.stage, '');
  assert.equal(h.spectator.st.stored, 1);
  assert.equal(h.spectator.stats().size.join('x'), '400x320');
});

test('by default the bytes are read in a task of their own after the frame, not inside it', async () => {
  const h = harness({ readAt: 'idle' });
  h.spectator.command({ on: true, width: 320 });
  h.gl.signal = h.gl.ALREADY_SIGNALED;
  h.spectator.frame(SRC);            // capture
  h.gl.reset();
  h.t += 14;
  h.spectator.frame(SRC);            // the fence has passed
  assert.deepEqual(h.gl.names(), ['clientWaitSync', 'deleteSync'], 'nothing read inside the frame');
  assert.equal(h.spectator.st.stage, 'read');
  h.spectator.frame(SRC);
  assert.ok(!h.gl.names().includes('getBufferSubData'));
  await new Promise((r) => setTimeout(r, 5));
  assert.ok(h.gl.names().includes('getBufferSubData'));
  assert.equal(h.encodes.length, 1);
  await flush();
  assert.equal(h.posts.length, 1);
  // 'start': read at the next frame's start, before it draws.
  const g = harness({ readAt: 'start' });
  g.spectator.command({ on: true, width: 320 });
  g.gl.signal = g.gl.ALREADY_SIGNALED;
  g.spectator.frame(SRC);
  g.t += 14;
  g.spectator.frame(SRC);
  assert.equal(g.encodes.length, 0);
  g.spectator.frameStart();
  assert.equal(g.encodes.length, 1);
  assert.equal(g.spectator.stats().readAt, 'start');
});

test('one capture in flight: frames due meanwhile are skipped, the pace is kept', async () => {
  const h = harness();
  h.spectator.command({ on: true, fps: 30, width: 320 });
  h.spectator.frame(SRC);
  for (let i = 0; i < 10; i++) { h.t += 40; h.spectator.frame(SRC); }
  assert.equal(h.gl.calls.filter((c) => c[0] === 'blitFramebuffer').length, 1, 'the fence never passed: no second capture');
  h.gl.signal = h.gl.CONDITION_SATISFIED;
  await untilEncoded(h, SRC);         // read back, encoded, posted
  h.t += 1;
  h.spectator.frame(SRC);            // 400+ ms after the last capture: due
  assert.equal(h.gl.calls.filter((c) => c[0] === 'blitFramebuffer').length, 2);
});

test('the backend says nobody watches: capture stops and the badge goes', async () => {
  const h = harness({ ack: { watched: false, stored: false, seq: 0, fps: 15, width: 512 } });
  h.spectator.command({ on: true });
  assert.ok(h.spectator.badge(new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1])), 'a badge while watched');
  h.gl.signal = h.gl.ALREADY_SIGNALED;
  h.spectator.frame(SRC);
  await untilEncoded(h, SRC);
  assert.equal(h.spectator.watching, false);
  assert.equal(h.spectator.badge(new Float32Array(16)), null);
  assert.ok(h.logs.some(([, ev]) => ev === 'spectator-off'));
  // Its resources are freed.
  assert.ok(h.gl.names().includes('deleteBuffer'));
});

test('404 stops (the hub says hello again), 429 drops the frame, failures in a row pause', async () => {
  for (const [status, watching] of [[404, false], [429, true]]) {
    const h = harness({ status });
    h.spectator.command({ on: true });
    h.gl.signal = h.gl.ALREADY_SIGNALED;
    h.spectator.frame(SRC);
    await untilEncoded(h, SRC);
    assert.equal(h.posts.length, 1);
    assert.equal(h.spectator.watching, watching, String(status));
  }
  const h = harness({ status: 500 });
  h.spectator.command({ on: true, fps: 30 });
  h.gl.signal = h.gl.ALREADY_SIGNALED;
  for (let i = 0; i < 6 * S.MAX_FAILURES && !h.spectator.st.pauses; i++) {
    h.t += 40;
    h.spectator.frame(SRC);
    await flush();
  }
  // Still watched (a link down for a while must not end it), but quiet for PAUSE_MS.
  assert.equal(h.spectator.watching, true);
  assert.equal(h.spectator.st.pauses, 1);
  assert.ok(h.logs.some(([lvl, ev]) => lvl === 'warning' && ev === 'spectator-paused'));
  const blits = () => h.gl.calls.filter((c) => c[0] === 'blitFramebuffer').length;
  const before = blits();
  for (let i = 0; i < 10; i++) { h.t += 400; h.spectator.frame(SRC); }
  assert.equal(blits(), before, 'no capture while paused');
  h.t += S.PAUSE_MS;
  h.spectator.frame(SRC);
  assert.equal(blits(), before + 1, 'captures again after the pause');
});

test('a GL error on the first capture stops the watch instead of sending black frames', () => {
  const h = harness();
  h.spectator.command({ on: true });
  const realBlit = h.gl.blitFramebuffer;
  h.gl.blitFramebuffer = (...a) => { realBlit(...a); h.gl.error = 0x502; };
  h.spectator.frame(SRC);
  assert.equal(h.spectator.watching, false);
  assert.ok(h.logs.some(([lvl, ev]) => lvl === 'warning' && ev === 'spectator-capture-failed'));
});

test('stop by command while a frame is on its way: its post is dropped, no stale state', async () => {
  const h = harness();
  h.spectator.command({ on: true });
  h.gl.signal = h.gl.ALREADY_SIGNALED;
  h.spectator.frame(SRC);
  for (let i = 0; i < 12 && !h.encodes.length; i++) { h.t += 14; h.spectator.frame(SRC); }   // encoding
  h.spectator.command({ on: false });
  await flush();
  assert.equal(h.spectator.watching, false);
  assert.equal(h.spectator.st.stored, 0);
  h.spectator.command({ on: true });
  assert.equal(h.spectator.st.stage, '');
});

test('a WebGL context loss: the capture under way is dropped, new GL objects on the restored context, still watched', async () => {
  const h = harness();
  h.spectator.command({ on: true, width: 320 });
  const head = new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
  const badge = h.spectator.badge(head);
  h.spectator.frame(SRC);
  assert.equal(h.spectator.st.stage, 'gpu');
  h.spectator.contextLost();
  assert.equal(h.spectator.st.stage, '');
  assert.equal(h.spectator.watching, true);
  assert.deepEqual(h.released, [badge.handle], 'the badge\'s panel was the old context\'s');
  assert.notEqual(h.spectator.badge(head).handle, badge.handle, 'a new one');
  // The next capture makes its objects again, and is checked again for a GL error.
  h.gl.reset();
  h.t += 100;
  h.spectator.frame(SRC);
  assert.ok(h.gl.names().includes('createFramebuffer') && h.gl.names().includes('createBuffer'));
  assert.ok(h.gl.names().includes('getError'));
  assert.equal(h.spectator.st.stage, 'gpu');
  assert.equal(h.spectator.stats().losses, 1);
  // A frame that was on its way to the encoder before the loss is never posted.
  h.gl.signal = h.gl.ALREADY_SIGNALED;
  h.t += 14;
  h.spectator.frame(SRC);            // read, to the encoder
  h.spectator.contextLost();
  await flush();
  assert.equal(h.posts.length, 0);
  // fenceSync gives null (the context is lost): no stage that never ends.
  const g = harness();
  g.spectator.command({ on: true });
  g.gl.fenceSync = () => null;
  g.spectator.frame(SRC);
  assert.equal(g.spectator.st.stage, '');
  assert.equal(g.spectator.st.failures, 1);
  assert.match(g.spectator.st.lastError, /no fence/);
});

test('a stage that never ends (a worker that hangs) is given up after STALL_MS, its worker replaced', async () => {
  const h = harness({ hang: true });
  h.spectator.command({ on: true, width: 320 });
  h.gl.signal = h.gl.ALREADY_SIGNALED;
  h.spectator.frame(SRC);
  h.t += 14;
  h.spectator.frame(SRC);            // read, to the (hung) encoder
  assert.equal(h.spectator.st.stage, 'encode');
  h.t += S.STALL_MS - 100;
  h.spectator.frame(SRC);
  assert.equal(h.spectator.st.stage, 'encode', 'not yet');
  h.t += 200;
  h.spectator.frame(SRC);
  assert.equal(h.spectator.st.stage, '');
  assert.equal(h.spectator.stats().stalls, 1);
  assert.ok(h.logs.some(([lvl, ev]) => lvl === 'warning' && ev === 'spectator-stalled'));
  assert.equal(h.workers.length, 2, 'a new encoder');
  assert.ok(h.workers[0].terminated);
  // The next capture goes, on the new worker.
  h.hang = false;
  h.t += 100;
  h.spectator.frame(SRC);
  h.t += 14;
  h.spectator.frame(SRC);
  await flush();
  assert.equal(h.posts.length, 1);
  assert.equal(h.encodes.length, 2);
  // The desktop's watch again unsticks it at once, without waiting for a frame.
  const g = harness({ hang: true });
  g.spectator.command({ on: true });
  g.gl.signal = g.gl.ALREADY_SIGNALED;
  g.spectator.frame(SRC);
  g.t += 14;
  g.spectator.frame(SRC);
  g.t += S.STALL_MS + 1;
  g.spectator.command({ on: true });
  assert.equal(g.spectator.st.stage, '');
  // A message the worker cannot read: what waits on it fails, the page encodes from then on.
  const k = harness({ hang: true });
  k.spectator.command({ on: true });
  k.gl.signal = k.gl.ALREADY_SIGNALED;
  k.spectator.frame(SRC);
  k.t += 14;
  k.spectator.frame(SRC);
  k.workers[0].onmessageerror();
  await flush();
  assert.equal(k.spectator.st.stage, '');
  assert.equal(k.spectator.st.failures, 1);
  assert.equal(k.spectator.stats().encoder, 'page');
});

test('the page hooks it up: hub onWatch, the left eye after the scene, the badge on top, the 2-D page', () => {
  const main = readFileSync(new URL('../../app/static/vr/js/main.js', import.meta.url), 'utf8');
  assert.match(main, /onWatch: \(cmd\) => spectator\.command\(cmd\)/);
  assert.match(main, /spectator\.badge\(pose\.transform\.matrix\)/);
  assert.match(main, /spectator\.frame\(\{ fbo: layer\.framebuffer/);
  assert.match(main, /spectator\.frame\(\{ fbo: null/);
  // Both context handlers drop its GL objects.
  const lost = main.slice(main.indexOf("addEventListener('webglcontextlost'"));
  assert.ok(lost.indexOf('spectator.contextLost()') < lost.indexOf('});'));
  const rebuild = main.slice(main.indexOf('function rebuildGraphics'));
  assert.ok(rebuild.indexOf('spectator.contextLost()') < rebuild.indexOf('R.restore()'));
  // Captured before the frame's GPU timer ends, so its GPU time is counted.
  const xrPart = main.slice(main.indexOf('function onXRFrame'));
  assert.ok(xrPart.indexOf('spectator.frame(') < xrPart.indexOf('R.endFrame()'));
});
