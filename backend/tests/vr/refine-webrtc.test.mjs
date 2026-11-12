// Refined frames as WebRTC video (issue #189): the pose id stripe, the
// pairing of headers with video frames, and when the video is chosen.
// node --test backend/tests/vr/refine-webrtc.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import {
  STRIPE, crc8, stripeBits, stripeCell, readStripe, grayRow, videoKey,
  FramePairer, TransportChooser, RtcStereo, grayFromCopy,
} from '../../app/static/vr/js/refine-webrtc.js';

// The same check values as backend/tests/test_render_stereo_webrtc.py: the
// page reads what the backend draws.
const ID = 0x12345678;
const BITS_OF_ID = [1, 1, 0, 1,
  0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 1, 0, 1, 0, 0, 0, 1, 0, 1, 0, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 0,
  0, 0, 0, 1, 1, 1, 0, 0,            // CRC-8 of 12 34 56 78: 0x1c
  0, 1, 0, 0];

/** A rendered stripe row as the backend draws it: 255 / 0 cells, black after. */
function drawRow(id, width) {
  const cell = stripeCell(width);
  const row = new Float32Array(width);
  stripeBits(id).forEach((b, i) => { if (b) row.fill(255, i * cell, (i + 1) * cell); });
  return row;
}

/** A row through a lossy codec, roughly: blurred edges, noise, a level offset. */
function degrade(row, seed, { blur = 3, noise = 25, offset = 0 } = {}) {
  let s = seed;
  const rnd = () => { s = (s * 1103515245 + 12345) % 2147483648; return s / 2147483648 - 0.5; };
  const out = new Float32Array(row.length);
  for (let x = 0; x < row.length; x++) {
    let a = 0, n = 0;
    for (let k = -blur; k <= blur; k++) { const j = x + k; if (j >= 0 && j < row.length) { a += row[j]; n++; } }
    out[x] = Math.min(255, Math.max(0, a / n + offset + noise * rnd()));
  }
  return out;
}

test('CRC-8/SMBUS has its check value', () => {
  assert.equal(crc8([...'123456789'].map((c) => c.charCodeAt(0))), 0xf4);
});

test('the stripe layout is the backend\'s: start, 32 id bits, CRC-8, end', () => {
  assert.deepEqual(stripeBits(ID), BITS_OF_ID);
  assert.equal(STRIPE.cells, 48);
  assert.equal(stripeCell(2048), 42);
  assert.equal(stripeCell(383), 0);           // eyes under 192 pixels: no video
  assert.equal(stripeCell(384), 8);
});

test('a stripe reads back through blur, noise and level shifts, at every width', () => {
  for (const width of [384, 1000, 2048, 3360, 5120]) {
    for (let i = 0; i < 200; i++) {
      const id = (i * 2654435761) >>> 0;
      const row = degrade(drawRow(id, width), i + width, { offset: (i % 5) * 8 - 16 });
      assert.equal(readStripe(row), id, `width ${width}, id ${id}`);
    }
  }
});

test('the pose id is its low 32 bits', () => {
  assert.equal(videoKey(2 ** 40 + 7), 7);
  assert.equal(readStripe(drawRow(2 ** 33 + 99, 2048)), 99);
});

test('a damaged stripe reads as nothing, never as another id', () => {
  const width = 2048, cell = stripeCell(width);
  let misread = 0, refused = 0;
  for (let i = 0; i < 2000; i++) {
    const id = (i * 40503 + 17) >>> 0;
    const row = drawRow(id, width);
    // Flip one cell (any of the 48), or two id cells.
    const flip = (k) => { const v = row[k * cell + cell / 2] > 127 ? 0 : 255; row.fill(v, k * cell, (k + 1) * cell); };
    flip(i % 48);
    if (i % 3 === 0) {
      let k2 = 4 + ((i * 7) % 32);                  // a second, different id cell
      if (k2 === i % 48) k2 = k2 === 4 ? 5 : 4;
      flip(k2);
    }
    const got = readStripe(row);
    if (got === null) refused++; else if (got !== id) misread++;
  }
  assert.equal(misread, 0);
  assert.equal(refused, 2000);
  // A frame of black, of white, mirrored, or with no stripe at all.
  assert.equal(readStripe(new Float32Array(2048)), null);
  assert.equal(readStripe(new Float32Array(2048).fill(255)), null);
  assert.equal(readStripe(drawRow(ID, 2048).reverse()), null);
});

test('grey levels of an RGBA row', () => {
  assert.deepEqual([...grayRow(new Uint8ClampedArray([255, 255, 255, 255, 0, 30, 60, 255]), 2)], [255, 30]);
});

// ── Pairing ─────────────────────────────────────────────────────────────────

const frameOf = (key) => ({ key, closed: false, close() { this.closed = true; } });

test('a header and its frame pair in either order', () => {
  const p = new FramePairer();
  assert.equal(p.addHeader({ key: 1, h: 'one' }, 0), null);
  const a = p.addVideo(1, frameOf(1), 1);
  assert.equal(a.entry.h, 'one');
  assert.equal(a.frame.key, 1);
  assert.equal(p.addVideo(2, frameOf(2), 2), null);         // the frame first
  const b = p.addHeader({ key: 2, h: 'two' }, 3);
  assert.equal(b.entry.h, 'two');
  assert.equal(b.frame.key, 2);
  assert.equal(p.stats.paired, 2);
});

test('headers whose frames were merged away are dropped when a newer frame pairs', () => {
  const p = new FramePairer();
  for (const k of [5, 6, 7]) p.addHeader({ key: k }, 0);
  const pair = p.addVideo(7, frameOf(7), 1);
  assert.equal(pair.entry.key, 7);
  assert.equal(p.waiting, 0);
  assert.equal(p.stats.headersDropped, 2);
});

test('a resent keyframe of the pose on show is a repeat; unreadable frames are closed', () => {
  const p = new FramePairer();
  p.addHeader({ key: 3 }, 0);
  p.addVideo(3, frameOf(3), 0);
  const again = frameOf(3);
  assert.equal(p.addVideo(3, again, 5), null);
  assert.ok(again.closed);
  assert.equal(p.stats.repeats, 1);
  const bad = frameOf(null);
  assert.equal(p.addVideo(null, bad, 6), null);
  assert.ok(bad.closed);
  assert.equal(p.stats.unreadable, 1);
});

test('a frame waits for its header only so long, and only the newest waits', () => {
  const p = new FramePairer({ holdMs: 100 });
  const f1 = frameOf(10), f2 = frameOf(11);
  p.addVideo(10, f1, 0);
  p.addVideo(11, f2, 10);
  assert.ok(f1.closed && !f2.closed);
  p.addHeader({ key: 99 }, 500);                // f2 is too old by now
  assert.ok(f2.closed);
  assert.equal(p.addHeader({ key: 11 }, 501), null);
});

test('synthetic stream: drops, merges, repeats, reordering and damage never mismatch', () => {
  // The backend renders poses 1..N; the track merges some (their headers
  // went out, their frames never do); video and headers race; some frames
  // are resent (keyframes) or come out unreadable. Every pair handed out
  // must be the header of the very pose drawn in its frame.
  let seed = 7;
  const rnd = () => { seed = (seed * 1103515245 + 12345) % 2147483648; return seed / 2147483648; };
  const p = new FramePairer({ maxHeaders: 8, holdMs: 400 });
  const events = [];
  // Each stream keeps its own order (TCP; RTP frames decoded in order), but
  // which of a header and its frame comes first is left to chance.
  let t = 0, th = 0, tvLast = 0;
  for (let id = 1; id <= 20000; id++) {
    t += 5 + 20 * rnd();
    th = Math.max(th, t + 30 * rnd());
    events.push({ t: th, kind: 'h', id });
    const fate = rnd();
    if (fate < 0.15) continue;                           // merged in the encoder: no frame
    const tv = tvLast = Math.max(tvLast, t + 40 * rnd());
    const width = 2048;
    let row = degrade(drawRow(id, width), id);
    if (fate > 0.97) row = new Float32Array(width).fill(128);      // damaged
    events.push({ t: tv, kind: 'v', id, row });
    if (fate > 0.9 && fate <= 0.97) events.push({ t: tv + 50, kind: 'v', id, row });   // a resend
  }
  events.sort((a, b) => a.t - b.t);
  let mismatches = 0, paired = 0;
  for (const e of events) {
    const pair = e.kind === 'h' ? p.addHeader({ key: e.id, id: e.id }, e.t)
      : p.addVideo(readStripe(e.row), { id: e.id, close() {} }, e.t);
    if (pair) { paired++; if (pair.entry.id !== pair.frame.id) mismatches++; }
  }
  assert.equal(mismatches, 0);
  assert.ok(paired > 15000, `paired ${paired}`);
});

// ── Choosing the transport ──────────────────────────────────────────────────

test('auto: the video once the JPEG frames are slow; jpeg never; webrtc at once', () => {
  const fast = new TransportChooser('auto');
  for (let i = 0; i < 20; i++) fast.observe(6);
  assert.equal(fast.wantVideo(0), false);
  const slow = new TransportChooser('auto');
  for (let i = 0; i < 7; i++) slow.observe(60);
  assert.equal(slow.wantVideo(0), false);          // not enough frames yet
  slow.observe(60);
  assert.equal(slow.wantVideo(0), true);
  const never = new TransportChooser('jpeg');
  for (let i = 0; i < 20; i++) never.observe(500);
  assert.equal(never.wantVideo(0), false);
  assert.equal(new TransportChooser('webrtc').wantVideo(0), true);
  assert.equal(new TransportChooser('nonsense').mode, 'auto');
});

test('after a failure: JPEG for a minute (auto), or until a new session (webrtc)', () => {
  const c = new TransportChooser('auto');
  for (let i = 0; i < 8; i++) c.observe(80);
  c.failed(1000, 'no H.264');
  for (let i = 0; i < 8; i++) c.observe(80);
  assert.equal(c.wantVideo(2000), false);
  assert.equal(c.wantVideo(1000 + 60001), true);
  const w = new TransportChooser('webrtc');
  w.failed(0, 'x');
  assert.equal(w.wantVideo(1e9), false);
  w.reset();
  assert.equal(w.wantVideo(1e9), true);
});

test('without WebRTC in the browser the link fails at once, back to JPEG', async () => {
  const sent = [];
  const r = new RtcStereo({ send: (c) => sent.push(c), onFrame: () => {}, mode: 'webrtc', now: () => 0 });
  r.connected();
  await new Promise((res) => setTimeout(res, 0));
  assert.equal(r.state, 'failed');
  assert.match(r.chooser.reason, /no WebRTC/);
  assert.deepEqual(sent, [{ type: 'xr_rtc_stop' }]);
  assert.match(r.hudLine(), /via JPEG/);
});

test('a refusal by the backend falls back to JPEG', async () => {
  const sent = [];
  const r = new RtcStereo({ send: (c) => sent.push(c), onFrame: () => {}, mode: 'auto', now: () => 0 });
  r.state = 'connecting';
  await r.onMessage({ type: 'xr_rtc_error', error: 'WebRTC transport not built in this backend.' });
  assert.equal(r.state, 'failed');
  assert.match(r.chooser.reason, /not built/);
});

// ── The refiner's side ──────────────────────────────────────────────────────

test('the refiner sends a video header to the pairing and shows the frame of its pose', async () => {
  globalThis.window = { location: { protocol: 'http:', host: 'localhost:8000' } };
  globalThis.createImageBitmap = async (src, x, y, w, h) => ({ width: w ?? src.width, height: h ?? src.height, src, close() {} });
  const { Refiner } = await import('../../app/static/vr/js/refine-client.js');
  const stubDraw = { forget() {}, shown: null, upload() { return 0; }, draw() { return false; } };
  const r = new Refiner(null, () => null, { draw: stubDraw, transport: 'webrtc' });
  r.ws = { readyState: 1, send() {} };
  r.rtc.state = 'video';                           // headers count only while the link is up
  const ids = [1, 2, 3].map(() => r.book.add({ tSent: performance.now(), version: r.version }));
  const message = (id) => {
    const header = { v: 1, pose_id: id, depth: null, timing_ms: { total: 4 }, video: { id },
      eyes: [], color: { format: 'h264-webrtc', width: 768, height: 256, offset: 0, length: 0,
        stripe: { y: 256, rows: 16, cells: 48, cell: 16, id_bits: 32 } } };
    const json = new TextEncoder().encode(JSON.stringify(header));
    const buf = new Uint8Array(8 + json.length);
    buf.set([0x56, 0x58, 0x52, 0x31]);
    new DataView(buf.buffer).setUint32(4, json.length, true);
    buf.set(json, 8);
    return buf.buffer;
  };
  for (const id of ids) r._onMessage(message(id));
  assert.equal(r.rtc.pairer.waiting, 3);
  assert.equal(r.incoming, null);
  // The frame of pose 2 is decoded (pose 1's never comes: merged).
  const frame = { width: 768, height: 272, closed: false, close() { this.closed = true; } };
  const pair = r.rtc.pairer.addVideo(ids[1], frame, performance.now());
  await r.rtc._deliver(pair);
  await new Promise((res) => setTimeout(res, 0));
  assert.equal(r.incoming.header.pose_id, ids[1]);
  assert.equal(r.incoming.rec.id, ids[1]);
  assert.equal(r.incoming.bitmap, frame);               // the frame itself, nothing copied
  assert.equal(r.incoming.vScale, 256 / 272);          // the texture shows the eyes' rows only
  assert.ok(!frame.closed);                             // held for the upload
  assert.equal(r.rtc.pairer.waiting, 1);                // pose 3 still waits
  // A frame whose size is not its header's is not shown.
  const wrong = { width: 700, height: 272, close() {} };
  const before = r.incoming;
  await r.rtc._deliver(r.rtc.pairer.addVideo(ids[2], wrong, performance.now()));
  assert.equal(r.incoming, before);
});

test('the video is given up only when a header waits 3 s with no frame, not after a long still view', () => {
  let now = 0;
  const sent = [];
  const r = new RtcStereo({ send: (c) => sent.push(c), onFrame: () => {}, mode: 'webrtc', now: () => now });
  r.state = 'video';
  r.lastVideoAt = 0;
  now = 60000;                                   // a minute still: no frame needed
  r.tick(now);
  assert.equal(r.state, 'video');
  r.onHeader({ header: { pose_id: 5, video: { id: 5 } }, depth: null }, {}, now);
  r.tick(now + 2000);
  assert.equal(r.state, 'video');                // its frame may still come
  r.tick(now + 3001);
  assert.equal(r.state, 'failed');
  assert.match(r.chooser.reason, /no video frame/);
  assert.deepEqual(sent, [{ type: 'xr_rtc_stop' }]);
});

// ── Review follow-ups: realistic damage, the copyTo row, the watchdogs ─────

/** A stripe row through a rough model of a lossy codec at a low rate:
 *  contrast loss, a level offset, 16-pixel blocks with a ramp across each
 *  (deblocking), ringing at the edges, a wide blur and strong noise. */
function codecLike(row, seed) {
  let s = seed;
  const rnd = () => { s = (s * 1103515245 + 12345) % 2147483648; return s / 2147483648 - 0.5; };
  const n = row.length, out = new Float32Array(n);
  const gain = 0.6 + 0.3 * (rnd() + 0.5), offset = 40 * rnd();
  for (let x = 0; x < n; x++) {
    let a = 0, k = 0;
    for (let d = -6; d <= 6; d++) { const j = x + d; if (j >= 0 && j < n) { a += row[j]; k++; } }
    const ring = x > 0 && row[x] !== row[x - 1] ? 50 * Math.sign(row[x] - row[x - 1]) : 0;
    const block = 12 * ((x % 16) / 16 - 0.5);
    out[x] = Math.min(255, Math.max(0, 128 + gain * (a / k - 128) + offset + ring + block + 40 * rnd()));
  }
  return out;
}

test('a stripe through realistic codec damage reads its id or nothing, never another', () => {
  let ok = 0, refused = 0, misread = 0;
  for (const width of [384, 768, 2048, 3400]) {
    for (let i = 0; i < 500; i++) {
      const id = (i * 2654435761 + width) >>> 0;
      const got = readStripe(codecLike(drawRow(id, width), i * 7 + width));
      if (got === id) ok++; else if (got === null) refused++; else misread++;
    }
  }
  assert.equal(misread, 0);
  // The narrowest stripe (8-pixel cells, 13-pixel blur) loses most of the refusals.
  assert.ok(ok >= 1800, `read ${ok}, refused ${refused}`);
});

test('bursts of damaged cells and rows of garbage never read as another id', () => {
  const width = 2048, cell = stripeCell(width);
  let seed = 3;
  const rnd = () => { seed = (seed * 1103515245 + 12345) % 2147483648; return seed / 2147483648; };
  let misread = 0;
  for (let i = 0; i < 3000; i++) {
    const id = (i * 40503 + 99) >>> 0;
    const row = drawRow(id, width);
    // A burst of 1 to 8 neighbouring cells set at random (a lost slice).
    const len = 1 + Math.floor(rnd() * 8), at = Math.floor(rnd() * (48 - len));
    for (let k = at; k < at + len; k++) row.fill(rnd() < 0.5 ? 0 : 255, k * cell, (k + 1) * cell);
    const got = readStripe(row);
    if (got !== null && got !== id) misread++;
  }
  // Rows of noise (a corrupt decode): the patterns and the CRC must refuse them.
  for (let i = 0; i < 3000; i++) {
    const row = new Float32Array(width).map(() => 255 * rnd());
    if (readStripe(row) !== null) misread++;
  }
  assert.equal(misread, 0);
});

test('the stripe row from a VideoFrame copy: Y plane (limited range) or RGB', () => {
  const width = 768, id = 0xabcdef01;
  const gray = drawRow(id, width);
  const y = new Uint8Array(4 + 2 * width);
  for (let x = 0; x < width; x++) y[4 + x] = 16 + Math.round(gray[x] * 219 / 255);
  assert.equal(readStripe(grayFromCopy(y, 'NV12', [{ offset: 4, stride: width }], width)), id);
  assert.equal(readStripe(grayFromCopy(y, 'I420', [{ offset: 4, stride: width }], width)), id);
  const rgba = new Uint8Array(4 * width * 2);
  for (let x = 0; x < width; x++) rgba.fill(gray[x], 4 * x, 4 * x + 3);
  assert.equal(readStripe(grayFromCopy(rgba, 'BGRA', [{ offset: 0, stride: 4 * width }], width)), id);
  assert.equal(grayFromCopy(y, 'I420P10', [{ offset: 0, stride: width }], width), null);
  assert.equal(grayFromCopy(y, 'whatever', [{ offset: 0, stride: width }], width), null);
});

test('headers count only while the video is up', () => {
  const r = new RtcStereo({ send: () => {}, onFrame: () => {}, mode: 'webrtc', now: () => 0 });
  r.state = 'connecting';
  r.onHeader({ header: { pose_id: 1, video: { id: 1 } }, depth: null }, {}, 0);
  assert.equal(r.pairer.waiting, 0);
  r.state = 'video';
  r.onHeader({ header: { pose_id: 2, video: { id: 2 } }, depth: null }, {}, 0);
  assert.equal(r.pairer.waiting, 1);
});

test('frames that arrive but never pair give the video up after 3 s', async () => {
  let now = 1000;
  const sent = [];
  const r = new RtcStereo({ send: (c) => sent.push(c), onFrame: () => {}, mode: 'webrtc', now: () => now });
  r.state = 'video';
  r.onHeader({ header: { pose_id: 7, video: { id: 7 } }, depth: null }, {}, now);
  // Video keeps coming, but its stripe does not read (all grey).
  for (let i = 0; i < 5; i++) {
    now += 500;
    await r._handle({ close() {} }, new Float32Array(1024).fill(128), r.gen);
    r.tick(now);
  }
  assert.equal(r.state, 'video');                 // 2.5 s: not yet
  now += 600;
  await r._handle({ close() {} }, new Float32Array(1024).fill(128), r.gen);
  r.tick(now);
  assert.equal(r.state, 'failed');
  assert.match(r.chooser.reason, /no video frame paired/);
});

test('ten unreadable frames in a row give the video up at once; a pair resets the count', async () => {
  const r = new RtcStereo({ send: () => {}, onFrame: () => {}, mode: 'webrtc', now: () => 0 });
  r.state = 'video';
  const grey = new Float32Array(1024).fill(128);
  for (let i = 0; i < 9; i++) await r._handle({ close() {} }, grey, r.gen);
  assert.equal(r.state, 'video');
  // A good pair in between starts the count again.
  r.onHeader({ header: { pose_id: 3, video: { id: 3 }, color: { width: 1024, height: 100, stripe: { rows: 16 } } }, depth: null }, {}, 0);
  await r._handle({ width: 1024, height: 116, close() {} }, drawRow(3, 1024), r.gen);
  assert.equal(r.badInARow, 0);
  for (let i = 0; i < 9; i++) await r._handle({ close() {} }, grey, r.gen);
  assert.equal(r.state, 'video');
  await r._handle({ close() {} }, grey, r.gen);
  assert.equal(r.state, 'failed');
  assert.match(r.chooser.reason, /10 video frames in a row without a readable pose id/);
});

test('frames that keep throwing give the video up', () => {
  const r = new RtcStereo({ send: () => {}, onFrame: () => {}, mode: 'webrtc', now: () => 0 });
  r.state = 'video';
  for (let i = 0; i < 4; i++) r._error(new Error('copyTo failed'));
  assert.equal(r.state, 'video');
  r._error(new Error('copyTo failed'));
  assert.equal(r.state, 'failed');
  assert.match(r.chooser.reason, /copyTo failed/);
});

/** A decoded NV12 VideoFrame as a hardware decoder gives it: the coded size
 *  rounded up to 16 (rows of garbage under the visible picture), the
 *  visible rectangle the encoded frame. `copyTo` copies the Y rows asked for. */
function fakeVideoFrame(id, width, height, { format = 'NV12', throws = false } = {}) {
  const codedW = Math.ceil(width / 16) * 16, codedH = Math.ceil(height / 16) * 16;
  const y = new Uint8Array(codedW * codedH).fill(200);           // garbage: bright
  const stripe = drawRow(id, width);
  for (let r = height - 16; r < height; r++) {
    for (let x = 0; x < width; x++) y[r * codedW + x] = 16 + Math.round(stripe[x] * 219 / 255);
  }
  return {
    format, codedWidth: codedW, codedHeight: codedH, displayWidth: width, displayHeight: height,
    visibleRect: { x: 0, y: 0, width, height }, rects: [],
    allocationSize({ rect }) { if (throws) throw new Error('not copyable'); return rect.width * rect.height; },
    async copyTo(buf, { rect }) {
      if (throws) throw new Error('not copyable');
      this.rects.push(rect);
      for (let r = 0; r < rect.height; r++) {
        buf.set(y.subarray((rect.y + r) * codedW + rect.x, (rect.y + r) * codedW + rect.x + rect.width), r * rect.width);
      }
      return [{ offset: 0, stride: rect.width }];
    },
    close() {},
  };
}

test('the stripe is read in the visible rectangle, not the coded size', async () => {
  const r = new RtcStereo({ send: () => {}, onFrame: () => {}, now: () => 0 });
  const frame = fakeVideoFrame(4242, 1700 * 2, 1813 + 17);     // coded 3408 x 1840
  assert.notEqual(frame.codedHeight, frame.displayHeight);
  const gray = await r._stripeOfVideoFrame(frame);
  assert.equal(gray.length, 3400);
  assert.equal(readStripe(gray), 4242);
  assert.deepEqual(frame.rects[0], { x: 0, y: (1830 - 8) & ~1, width: 3400, height: 2 });
  assert.ok(!r.stripeFallback);
});

test('a frame that cannot be copied (or has no format) is read through an image, from then on', async () => {
  const r = new RtcStereo({ send: () => {}, onFrame: () => {}, now: () => 0 });
  const viaImage = [];
  globalThis.createImageBitmap = async (src) => ({ src, close() {} });
  r._stripeOfBitmap = (bm) => { viaImage.push(bm.src); return drawRow(77, 768); };
  const broken = fakeVideoFrame(77, 768, 272, { throws: true });
  assert.equal(readStripe(await r._stripeOfVideoFrame(broken)), 77);
  assert.ok(r.stripeFallback && r._copyFails);
  // Remembered: the next frame is not even tried with copyTo.
  const good = fakeVideoFrame(78, 768, 272);
  await r._stripeOfVideoFrame(good);
  assert.equal(good.rects.length, 0);
  assert.equal(viaImage.length, 2);
  // A frame with no format (opaque hardware memory) goes the same way.
  const r2 = new RtcStereo({ send: () => {}, onFrame: () => {}, now: () => 0 });
  r2._stripeOfBitmap = () => drawRow(9, 768);
  const opaque = fakeVideoFrame(9, 768, 272, { format: null });
  assert.equal(readStripe(await r2._stripeOfVideoFrame(opaque)), 9);
  assert.equal(opaque.rects.length, 0);
});
