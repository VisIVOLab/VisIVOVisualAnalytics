// Safe performance defaults and diagnostics (epic #166).
// node --test backend/tests/vr/quality.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import * as Q from '../../app/static/vr/js/quality.js';
import * as D from '../../app/static/vr/js/diagnostics.js';
import { Governor } from '../../app/static/vr/js/volume.js';
import { sanitizeSettings } from '../../app/static/vr/js/settings.js';

const MiB = 1024 * 1024;

test('eye buffer: lowered only with samples at the floor and frames missed, one step per window', () => {
  const g = new Q.EyeBufferGovernor({ start: 1, holdMs: 1000 });
  const target = 1000 / 90;
  // Missing every frame but samples not at the floor: nothing.
  for (let i = 0; i < 200; i++) g.update(25, target, false);
  assert.equal(g.scale, 1);
  // At the floor: one step after one window (40 frames of 25 ms), then the next.
  for (let i = 0; i < 40; i++) g.update(25, target, true);
  assert.equal(g.scale, 0.85);
  for (let i = 0; i < 400; i++) g.update(25, target, true);
  assert.equal(g.scale, Q.EYE_SCALE_FLOOR, 'never below the floor');
  assert.equal(Q.EYE_SCALE_FLOOR, 0.85, 'issue #226: never below 0.85×');
  // Good frames never raise it again within the session.
  for (let i = 0; i < 2000; i++) g.update(target, target, true);
  assert.equal(g.scale, 0.85);
  g.reset(1.25);
  assert.equal(g.scale, 1.25);
  for (let i = 0; i < 40; i++) g.update(25, target, true);
  assert.equal(g.scale, 1);
  // A few missed frames (under 10 %) are not enough.
  const h = new Q.EyeBufferGovernor({ start: 1, holdMs: 1000 });
  for (let i = 0; i < 90; i++) h.update(i % 20 === 0 ? 25 : target, target, true);
  assert.equal(h.scale, 1);
  // Long pauses (a hitch, a dialog) are not frames.
  for (let i = 0; i < 100; i++) h.update(400, target, true);
  assert.equal(h.scale, 1);
});

test('the frame-rate governor is on by default and its first stage drops samples', () => {
  const g = new Governor({ start: 1, max: 1 });
  for (let i = 0; i < 30; i++) g.update(30, 1000 / 90);
  assert.ok(g.value <= 0.5, `samples/voxel ${g.value}`);
  // The page's default: hold the frame rate (index.html checkbox, main.js state).
  const html = new URL('../../app/static/vr/index.html', import.meta.url);
  return import('node:fs').then(({ readFileSync }) => {
    assert.match(readFileSync(html, 'utf8'), /id="governor" type="checkbox" checked/);
    const main = readFileSync(new URL('../../app/static/vr/js/main.js', import.meta.url), 'utf8');
    assert.match(main, /governorOn: true/);
  });
});

test('the panel says when and how quality was lowered', () => {
  assert.equal(Q.qualityNote({ governorOn: true, spv: 1, spvChosen: 1, eyeScale: 1, eyeChosen: 1, targetHz: 90 }), '');
  assert.equal(Q.qualityNote({ governorOn: false, spv: 0.3, spvChosen: 1, targetHz: 90 }), '');
  assert.equal(Q.qualityNote({ governorOn: true, spv: 0.52, spvChosen: 1, eyeScale: 0.85, eyeChosen: 1, targetHz: 90 }),
    'Quality lowered to hold 90 Hz: 0.52 samples/voxel (of 1), eye buffer 0.85× (of 1×)');
  assert.match(Q.qualityNote({ governorOn: true, spv: 0.5, spvChosen: 1, targetHz: 0 }), /hold the frame rate/);
});

test('the budget refuses a cube before anything is allocated', () => {
  const ok = Q.cubeBudget({ dims: [256, 256, 158], encoding: 'uint8' }, { headset: true, deviceMemoryGB: 8 });
  assert.ok(ok.ok);
  assert.equal(ok.textureBytes, 256 * 256 * 158);
  // The selection mask's storage (one byte a voxel) is allocated with the
  // cube (issue #229) and counted: 8-bit 640x640x200 = 78 MiB of texture,
  // 157 MiB on the GPU: fits a headset with 8 GB, not one with 4.
  const mid = Q.cubeBudget({ dims: [640, 640, 200], encoding: 'uint8' }, { headset: true, deviceMemoryGB: 8 });
  assert.ok(mid.ok);
  assert.equal(mid.gpuBytes, 2 * 640 * 640 * 200 + 80 * 80 * 25 * 4);
  const small = Q.cubeBudget({ dims: [640, 640, 200], encoding: 'uint8' }, { headset: true, deviceMemoryGB: 4 });
  assert.ok(!small.ok);
  assert.match(small.reason, /headset allows 128 MiB/);
  assert.match(small.reason, /for the selection mask/);
  // 512^3 8-bit: 128 MiB of texture, but 257 MiB with its mask.
  assert.ok(!Q.cubeBudget({ dims: [512, 512, 512], encoding: 'uint8' }, { headset: true, deviceMemoryGB: 8 }).ok);
  // 16-bit doubles the texture, not the mask.
  const f16 = Q.cubeBudget({ dims: [640, 640, 300], encoding: 'float16' }, { headset: true, deviceMemoryGB: 8 });
  assert.ok(!f16.ok);
  assert.equal(f16.textureBytes, 2 * 640 * 640 * 300);
  assert.equal(f16.gpuBytes, 3 * 640 * 640 * 300 + 80 * 80 * 38 * 4);
  // A computer's browser allows more.
  assert.ok(Q.cubeBudget({ dims: [640, 640, 640], encoding: 'float16' }, { headset: false, deviceMemoryGB: 8 }).ok);
  // The 3-D texture side is checked first.
  const side = Q.cubeBudget({ dims: [4096, 16, 16], encoding: 'uint8' }, { maxTex3D: 2048 });
  assert.ok(!side.ok);
  assert.match(side.reason, /3-D texture limit \(2048/);
  // Unknown device memory: the headset's 8 GB share.
  assert.equal(Q.textureLimitBytes({ headset: true }), 256 * MiB);
  assert.ok(ok.peakBytes > 2 * ok.textureBytes);
});

// ── Diagnostics ───────────────────────────────────────────────────────────

test('scrubbing removes tokens and directories, keeps hosts and file names', () => {
  const tok = 'Zx81_secret-token-value';
  const s = D.scrubText(`GET ws://localhost:8000/v1/render/abc/ws?token=${tok}&q=1 failed; X-Visivo-Token: ${tok}; `
    + `open /Users/alice/proj/cube.fits and C:\\Users\\bob\\m33.fits, session=deadbeef12 ${tok}`, [tok]);
  assert.ok(!s.includes(tok), s);
  assert.ok(!s.includes('alice') && !s.includes('bob') && !s.includes('deadbeef12'), s);
  assert.match(s, /localhost:8000/);
  assert.match(s, /…\/cube\.fits/);
  assert.match(s, /m33\.fits/);
  assert.equal(D.scrubText('Mozilla/5.0 (X11; Linux x86_64)'), 'Mozilla/5.0 (X11; Linux x86_64)');
  assert.equal(D.scrubText('at main.js:12:5'), 'at main.js:12:5');
  // The backend's own routes are not paths on disk: kept.
  assert.equal(D.scrubText('POST /v1/cube/subvolume_bin failed'), 'POST /v1/cube/subvolume_bin failed');
  assert.equal(D.scrubText('GET /vr/b/abc/js/main.js'), 'GET /vr/b/abc/js/main.js');
});

test('the log ring counts repeats and keeps the last events', () => {
  let t = 0;
  const log = new D.DiagLog({ max: 3, now: () => ++t, secrets: () => ['secret-123'] });
  log.add('error', 'page-error', 'boom secret-123');
  log.add('error', 'page-error', 'boom secret-123');
  assert.equal(log.items.length, 1);
  assert.equal(log.items[0].count, 2);
  assert.equal(log.items[0].message, 'boom <token>');
  for (let i = 0; i < 5; i++) log.add('info', `e${i}`);
  assert.equal(log.items.length, 3);
  assert.equal(log.items[0].event, 'e2');
  log.add('warning', 'w');
  assert.deepEqual(log.errors().map((e) => e.event), ['w']);
});

function fakeTimers() {
  const q = [];
  return { q, setTimeout(f, ms) { const h = { f, ms }; q.push(h); return h; }, clearTimeout(h) { const i = q.indexOf(h); if (i >= 0) q.splice(i, 1); } };
}

test('the sender batches, honours 429 and the per-minute cap, stops on 401, keeps events when down', async () => {
  let t = 0;
  const posts = [];
  let status = 200;
  const timers = fakeTimers();
  const s = new D.LogSender({ post: async (b) => { posts.push(b.events.length); return { status, retryAfterS: 7 }; },
    now: () => t, timers, maxPerPost: 5, maxPerMinute: 8, intervalMs: 3000 });
  for (let i = 0; i < 12; i++) s.enqueue({ level: 'info', event: `e${i}`, message: '', count: 1, t });
  s.enqueue({ level: 'debug', event: 'local-only', message: '', count: 1, t });
  assert.equal(s.queue.length, 12, 'debug events stay local');
  assert.equal(timers.q.length, 1);
  await s.flush();
  assert.deepEqual(posts, [5]);
  await s.flush();
  assert.deepEqual(posts, [5, 3], 'the per-minute cap: 8');
  await s.flush();
  assert.deepEqual(posts, [5, 3]);
  t += 61000;
  status = 429;
  await s.flush();
  assert.equal(s.queue.length, 4, '429: kept');
  await s.flush();
  assert.deepEqual(posts, [5, 3, 4], 'blocked for Retry-After');
  t += 8000;
  status = 0;                                       // the backend is down
  await s.flush();
  assert.equal(s.queue.length, 4);
  t += 11000;
  status = 401;
  await s.flush();
  assert.ok(s.stopped);
  const n = posts.length;
  await s.flush();
  assert.equal(posts.length, n, 'stopped until resumed');
  status = 200;
  s.resume();
  await s.flush();
  assert.equal(s.queue.length, 0);
  assert.equal(s.sent, 12);
});

test('the diagnostics text holds versions, limits, settings and errors, no secrets', () => {
  const txt = D.diagnosticsReport({
    time: '2026-11-11T20:00:00Z', page: { build: 'abc123abc123', api: 1 },
    backend: { backend_version: '1.0.12', build: 'abc123abc123', vr_api: 1, vr_api_min: 1, instance: 'i1' },
    lifecycle: 'ok', address: 'http://localhost:8000/vr/', userAgent: 'UA',
    gpu: { renderer: 'Adreno 740', maxTex3D: 2048, maxTexture: 16384, timerQuery: true, deviceMemoryGB: 8 },
    xr: 'immersive-vr', cube: '234×234×158', fps: '72 fps', quality: 'as chosen', refine: 'refined',
    settings: sanitizeSettings({ colormap: 'magma' }),
    errors: [{ t: 0, level: 'error', event: 'page-error', message: 'x token=SECRETSECRET', count: 2 }],
    secrets: ['SECRETSECRET'],
  });
  for (const want of ['page build: abc123abc123', 'version 1.0.12', 'Adreno 740', '3-D texture 2048', '"colormap":"magma"', 'page-error ×2']) {
    assert.ok(txt.includes(want), want);
  }
  assert.ok(!txt.includes('SECRETSECRET'));
  assert.match(D.fpsSummary([11.1, 11.1, 11.2, 30], 90), /fps mean over 4 frames.*missed 25\.0 %/);
  assert.equal(D.fpsSummary([], 90), 'no frames');
});

test('the eye-buffer stage: at the real floor of the governor, and enabled again each session', () => {
  // Issue #226: samples per voxel are given up first, all the way down.
  assert.ok(!Q.spvAtFloor({ value: 0.5, min: 0.25, max: 0.5 }), 'chosen 0.5, not lowered yet');
  assert.ok(!Q.spvAtFloor({ value: 0.42, min: 0.25, max: 0.5 }), 'lowered, not at the floor yet');
  assert.ok(!Q.spvAtFloor({ value: 0.5, min: 0.25, max: 1 }));
  assert.ok(!Q.spvAtFloor({ value: 0.3, min: 0.25, max: 1 }));
  assert.ok(Q.spvAtFloor({ value: 0.25, min: 0.25, max: 1 }), 'at the floor');
  assert.ok(Q.spvAtFloor({ value: 0.25, min: 0.25, max: 0.25 }), 'at its own floor');
  const g = new Q.EyeBufferGovernor({ start: 1, holdMs: 1000 });
  g.disabled = true;
  for (let i = 0; i < 100; i++) g.update(25, 1000 / 90, true);
  assert.equal(g.scale, 1);
  g.reset(1);
  for (let i = 0; i < 40; i++) g.update(25, 1000 / 90, true);
  assert.equal(g.scale, 0.85);
});

test('issue #226: 72 Hz by default, and the policy is said in the diagnostics', async () => {
  assert.equal(Q.DEFAULT_RATE_HZ, 72);
  const { readFileSync } = await import('node:fs');
  const html = readFileSync(new URL('../../app/static/vr/index.html', import.meta.url), 'utf8');
  assert.match(html, /<option selected>72<\/option>/);
  const p = Q.qualityPolicy({ targetHz: 72, spvMin: 0.25, spvChosen: 1, eyeChosen: 1 });
  assert.match(p, /^72 Hz; samples\/voxel first \(1 down to 0\.25\), then the eye buffer as a last resort \(1× down to 0\.85× at most\)$/);
  // A smaller eye buffer chosen on the page is never lowered further.
  assert.match(Q.qualityPolicy({ targetHz: 90, eyeChosen: 0.75 }), /0\.75× down to 0\.75×/);
  // The governors together: samples per voxel go all the way down before the eye buffer moves.
  const spv = new Governor({ start: 1, max: 1 });
  const eye = new Q.EyeBufferGovernor({ start: 1, holdMs: 1000 });
  const target = 1000 / 72;
  let firstEyeDropAt = null;
  for (let i = 0; i < 2000; i++) {
    spv.update(30, target);
    const s = eye.update(30, target, Q.spvAtFloor(spv));
    if (s < 1 && firstEyeDropAt === null) firstEyeDropAt = spv.value;
  }
  assert.ok(firstEyeDropAt !== null && firstEyeDropAt <= 0.25 * 1.02, `eye buffer lowered at ${firstEyeDropAt} samples/voxel`);
  assert.equal(eye.scale, 0.85);
});

test('the diagnostics text keeps one line per item, all of it, scrubbed line by line', () => {
  const errors = Array.from({ length: 25 }, (_, i) => ({ t: i * 1000, level: 'warning', event: `event-${i}`, message: `x /Users/alice/c${i}.fits`, count: 1 }));
  const txt = D.diagnosticsReport({ time: 't', page: { build: 'b', api: 1 }, gpu: {}, settings: { a: 1 }, errors, secrets: [] });
  const lines = txt.split('\n');
  assert.ok(lines.length > 30, `${lines.length} lines`);
  assert.ok(txt.includes('event-0') && txt.includes('event-24'), 'the first and the last event are both there');
  assert.ok(!txt.includes('alice'));
  assert.ok(lines.every((l) => l.length <= 1200));
});

test('the diagnostics name the debug cuts of the address, only when there are some (issue #229)', () => {
  const base = { time: 't', page: { build: 'b', api: 1 }, gpu: {}, settings: {}, errors: [], secrets: [] };
  assert.ok(D.diagnosticsReport({ ...base, cuts: 'nohover nohud' }).split('\n').includes('debug cuts: nohover nohud'));
  assert.ok(!D.diagnosticsReport({ ...base, cuts: '' }).includes('debug cuts'));
  assert.ok(!D.diagnosticsReport(base).includes('debug cuts'));
});
