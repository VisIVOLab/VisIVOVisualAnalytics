// Measurement records and the guided run (issue #224): the sample builder,
// the sender's bounds and failures, the protocol's steps, prompts, pulses,
// skips and cancel, and the report fixture's figures as the page computes them.
// node --test backend/tests/vr/measure.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { FrameStats } from '../../app/static/vr/js/volume.js';
import { Rolling } from '../../app/static/vr/js/refine.js';
import {
  MEASURE_FORMAT, UNITS, DEFINITIONS, recordId, RECORD_ID, linkKind, shortHash, frameFigures, newValues,
  MeasureSender, MeasureRecorder, buildSample, refineFigures, videoFigures, pairFigures, MAX_INTERVALS,
} from '../../app/static/vr/js/measure.js';
import {
  CONDITIONS, PHASES, protocolPlan, planSeconds, promptNotice, MeasureProtocol, STOP_CYCLE,
} from '../../app/static/vr/js/measure-protocol.js';

// ── Small pieces ────────────────────────────────────────────────────────────

test('record id: UTC date, time and 8 hex digits', () => {
  const id = recordId(new Date(Date.UTC(2026, 10, 13, 9, 5, 7)), 'A1b2C3d4ff');
  assert.equal(id, '20261113-090507-a1b2c3d4');
  assert.match(id, RECORD_ID);
  assert.match(recordId(new Date(0), 'zz'), RECORD_ID);
});

test('link kind: by class only, never the host name', () => {
  assert.deepEqual(linkKind({ hostname: 'localhost' }), { kind: 'direct', host: 'loopback' });
  assert.deepEqual(linkKind({ hostname: '127.0.0.1' }), { kind: 'direct', host: 'loopback' });
  assert.deepEqual(linkKind({ hostname: '[::1]' }), { kind: 'direct', host: 'loopback' });
  assert.deepEqual(linkKind({ hostname: '192.168.1.20' }), { kind: 'direct', host: 'lan' });
  assert.deepEqual(linkKind({ hostname: 'my-mac.local' }), { kind: 'direct', host: 'lan' });
  assert.deepEqual(linkKind({ hostname: 'portal.example.org' }), { kind: 'direct', host: 'remote' });
  assert.deepEqual(linkKind({ paired: true, hostname: 'x' }), { kind: 'paired', host: 'pairing service' });
});

test('short hash: stable, 8 hex digits, differs for different looks', () => {
  assert.equal(shortHash('abc'), shortHash('abc'));
  assert.match(shortHash('abc'), /^[0-9a-f]{8}$/);
  assert.notEqual(shortHash('[0.1,1]'), shortHash('[0.1,0.9]'));
});

test('frame figures are FrameStats\' own (the wrist\'s code), missed % only with a display rate', () => {
  const fs = new FrameStats(1000);
  const iv = [11.1, 11.2, 11.0, 25.0, 11.1, 11.3, 10.9, 11.1];
  iv.forEach((v, i) => fs.push(v, 2 + i * 0.1, i % 2 ? 4 : -1));
  const f = frameFigures(fs, 90);
  const s = fs.summary(1000 / 90);
  assert.equal(f.n, 8);
  assert.equal(f.fps, Math.round(s.fps * 100) / 100);
  assert.equal(f.p95, Math.round(s.interval.p95 * 100) / 100);
  assert.equal(f.missed_pct, 12.5);          // 25 ms > 1.5 × 11.1 ms
  assert.equal(f.gpu_mean, 4);               // only the frames with a timer result
  assert.equal(f.gpu_n, 4);
  assert.equal(f.cpu_n, 8);
  assert.equal(f.target_hz, 90);
  assert.equal(frameFigures(fs, 0).missed_pct, null);
});

test('new pose→frame values: only those pushed since the last look, at most what the ring holds', () => {
  const r = new Rolling(4);
  [1, 2, 3].forEach((v) => r.push(v));
  assert.deepEqual(newValues(r, r.total, 0), [1, 2, 3]);
  const seen = r.total;
  [4, 5, 6, 7, 8, 9].forEach((v) => r.push(v));
  assert.deepEqual(newValues(r, r.total, seen), [6, 7, 8, 9]);      // two fell out of the ring
  assert.deepEqual(newValues(r, r.total, r.total), []);
});

test('refine, video and pair figures', () => {
  const rf = refineFigures({ enabled: true, suspended: '', status: 'refined', motion: 'still', weight: 1, transport: { state: 'video' },
    mode: 'webrtc', scale: 1, quality: 95, depth: true, latency: { n: 3, p50: 81.234, p95: 140 }, remoteFps: 11.55,
    lastStopFirst: 120.04, lastStopFull: NaN, lastServer: { total: 30.2, render: 20.1 }, lastKB: 210.33, window: 2 }, [80.04, 90]);
  assert.equal(rf.via, 'webrtc');
  assert.deepEqual(rf.pose_frame, { n: 3, p50: 81.2, p95: 140 });
  assert.deepEqual(rf.pf, [80, 90]);
  assert.equal(rf.stop_full, null);          // NaN is null, never NaN in JSON
  assert.equal(refineFigures({ enabled: false, transport: { state: 'off' } }).via, null);
  assert.equal(refineFigures({ enabled: true, transport: { state: 'off' } }).via, 'jpeg');
  assert.equal(videoFigures({ state: 'off' }), null);
  assert.deepEqual(videoFigures({ state: 'video', bitrate: { kbps: 9000.04, decodeMs: 2.345, rttMs: 3.1, ice: 'host/host', bytes: 10 } }),
    { kbps: 9000, decode_ms: 2.35, jitter_ms: null, frames_decoded: null, frames_dropped: null, bytes: 10, rtt_ms: 3.1, ice: 'host/host' });
  assert.equal(pairFigures({ paired: false }), null);
  assert.equal(pairFigures({ paired: true, state: 'up', rttMs: 12.34, candidates: 'host/srflx udp' }).rtt_ms, 12.3);
});

test('a sample: every figure documented, JSON-safe, the raw intervals kept', () => {
  const fs = new FrameStats(1000);
  for (let i = 0; i < 72; i++) fs.push(13.9, 2, 5);
  const s = buildSample({ t: 1000.04, dur: 1000, window: fs, iv: Array(72).fill(13.9), proto: { condition: 'jpeg', phase: 'still', part: 'measure' },
    buildMs: 0.123, xr: 'vr', mode: 'explore', tool: null, targetHz: 72, wrist: fs.summary(1000 / 72),
    display: { display_hz: 72 }, render: { spv: 1 }, refine: null, cube: { dataset: 'abc', factors: [2, 2, 1] }, look: 'deadbeef', ui: { menu: false } });
  assert.equal(s.k, 's');
  assert.equal(s.frames.n, 72);
  assert.equal(s.iv.length, 72);
  assert.equal(s.wrist.fps, s.frames.fps);
  assert.equal(s.proto.phase, 'still');
  assert.equal(s.rec_ms, 0.12);
  const text = JSON.stringify(s);
  assert.ok(!/NaN|Infinity/.test(text));
  for (const k of ['t', 'dur', 'iv', 'look', 'rec_ms']) assert.ok(UNITS[k], k);
  assert.ok(DEFINITIONS.quantiles_frames && DEFINITIONS.missed);
});

// ── The recorder ────────────────────────────────────────────────────────────

function fakeSender() {
  return { lines: [], start(id) { this.id = id; }, push(l) { this.lines.push(l); }, async drain() {} };
}

test('recorder: header first, one sample a second, events with their own time, cut at a phase', async () => {
  let t = 0;
  const snd = fakeSender();
  const rec = new MeasureRecorder({ sender: snd, now: () => t, wall: () => new Date(Date.UTC(2026, 10, 13)),
    snapshot: () => ({ xr: null, mode: 'explore', targetHz: 0, wrist: null }) });
  rec.start({ page: { build: 'b' } }, '20261113-000000-00000000');
  assert.equal(snd.lines[0].k, 'h');
  assert.equal(snd.lines[0].format, MEASURE_FORMAT);
  assert.ok(snd.lines[0].units && snd.lines[0].definitions);
  for (let i = 0; i < 130; i++) { t += 16.7; rec.frame(16.7, 1, -1); rec.tick(); }
  // 130 frames × 16.7 ms = 2171 ms: two full windows, the third open.
  assert.equal(snd.lines.filter((l) => l.k === 's').length, 2);
  rec.event('refined-first', { ms: 120 });
  rec.setProto({ condition: 'local', phase: 'still', part: 'measure' });
  rec.cut();
  const samples = snd.lines.filter((l) => l.k === 's');
  assert.equal(samples.length, 3);
  assert.ok(samples[2].dur < 1000);
  const ev = snd.lines.find((l) => l.name === 'refined-first');
  assert.equal(ev.t, 2171);
  rec.event('phase', {});
  assert.equal(snd.lines.at(-1).proto.condition, 'local');
  await rec.stop('test');
  assert.equal(snd.lines.at(-1).name, 'record-stop');
  // Off: nothing more.
  const n = snd.lines.length;
  rec.frame(10, 1, 1); rec.tick(); rec.event('x');
  assert.equal(snd.lines.length, n);
});

test('recorder: a snapshot that throws becomes an event, never an exception', () => {
  let t = 0;
  const snd = fakeSender();
  const rec = new MeasureRecorder({ sender: snd, now: () => t, snapshot: () => { throw new Error('boom'); } });
  rec.start({}, '20261113-000000-00000000');
  t = 1500; rec.frame(16, 1, 1); rec.tick();
  assert.equal(snd.lines.at(-1).name, 'sample-failed');
});

test('recorder: at most MAX_INTERVALS intervals per sample', () => {
  let t = 0;
  const snd = fakeSender();
  const rec = new MeasureRecorder({ sender: snd, now: () => t, sampleMs: 100000, snapshot: () => ({ wrist: null }) });
  rec.start({}, '20261113-000000-00000000');
  for (let i = 0; i < MAX_INTERVALS + 50; i++) { t += 1; rec.frame(1, 0, -1); }
  rec.cut();
  assert.equal(snd.lines.at(-1).iv.length, MAX_INTERVALS);
});

// ── The sender ──────────────────────────────────────────────────────────────

function fakeTimers() {
  const list = [];
  return { list, setTimeout(f, ms) { const h = { f, ms }; list.push(h); return h; }, clearTimeout(h) { const i = list.indexOf(h); if (i >= 0) list.splice(i, 1); },
    async run() { while (list.length) { const h = list.shift(); await h.f(); } } };
}

test('sender: batches with numbers, the same number again after a failure, nothing lost', async () => {
  const posts = [];
  const answers = [{ status: 0 }, { status: 200, body: { file: 'vr-measure-x.jsonl' } }, { status: 200, body: {} }];
  const tm = fakeTimers();
  let now = 0;
  const s = new MeasureSender({ post: async (b) => { posts.push(b); return answers.shift(); }, now: () => now, timers: tm, batchLines: 3 });
  s.start('20261113-000000-00000000');
  for (let i = 0; i < 5; i++) s.push({ k: 's', i });
  await s.flush();                 // fails: kept as batch 0
  assert.equal(posts[0].seq, 0);
  assert.equal(s.inflight.seq, 0);
  now = 6000;
  await s.flush();                 // batch 0 again
  assert.equal(posts[1].seq, 0);
  assert.equal(JSON.parse(posts[1].lines).length, 3);
  assert.equal(s.file, 'vr-measure-x.jsonl');
  await s.flush();                 // batch 1: the last two
  assert.equal(posts[2].seq, 1);
  assert.deepEqual(JSON.parse(posts[2].lines).map((l) => l.i), [3, 4]);
  assert.equal(s.sentLines, 5);
  assert.equal(s.dropped, 0);
});

test('sender: 507 stops storing this record (the page carries on), 422 drops one batch, 401 pauses', async () => {
  const tm = fakeTimers();
  let ans = { status: 507, body: { detail: 'cannot write the record (No space left on device)' } };
  const s = new MeasureSender({ post: async () => ans, timers: tm, batchLines: 2 });
  s.start('20261113-000000-00000000');
  s.push({ k: 's' }); s.push({ k: 's' }); s.push({ k: 's' });
  await s.flush();
  assert.equal(s.stored, false);
  assert.match(s.status, /not stored: cannot write/);
  s.push({ k: 's' });               // ignored, never throws
  assert.equal(s.queue.length, 0);
  ans = { status: 422, body: { detail: 'Bad line' } };
  s.start('20261113-000000-00000001');
  s.push({ k: 's' }); s.push({ k: 's' }); s.push({ k: 'e' });
  await s.flush();
  assert.equal(s.dropped, 2);
  assert.equal(s.queue.length, 1);
  ans = { status: 401, body: {} };
  await s.flush();
  assert.equal(s.paused, true);
  ans = { status: 200, body: {} };
  s.resume();
  await tm.run();
  assert.equal(s.sentLines, 1);
});

test('sender: a bounded queue drops samples first, keeps the header and events', () => {
  const s = new MeasureSender({ post: async () => ({ status: 0 }), timers: fakeTimers(), maxQueue: 5, batchLines: 100 });
  s.start('20261113-000000-00000000');
  s.push({ k: 'h' });
  for (let i = 0; i < 6; i++) s.push({ k: 's', i });
  s.push({ k: 'e', name: 'x' });
  assert.equal(s.queue.length, 5);
  assert.deepEqual(s.queue.map((q) => q.kind), ['h', 's', 's', 's', 'e']);
  assert.equal(s.dropped, 3);
  // A line over 32 KiB is dropped, not sent.
  s.push({ k: 's', big: 'x'.repeat(40000) });
  assert.equal(s.dropped, 4);
});

test('sender: the answer to a batch of the previous record never counts for the next one', async () => {
  let release;
  const s = new MeasureSender({ post: () => new Promise((r) => { release = r; }), timers: fakeTimers() });
  s.start('20261113-000000-00000000');
  s.push({ k: 's' }); s.push({ k: 's' });
  const p = s.flush();
  s.start('20261113-000000-00000001');
  release({ status: 200, body: { file: 'old.jsonl' } });
  await p;
  assert.equal(s.sentLines, 0);
  assert.equal(s.file, '');
  assert.equal(s.busy, false);
});

test('sender: a post that throws is a failure kept for later, never an exception', async () => {
  const s = new MeasureSender({ post: async () => { throw new TypeError('the public loader did not answer'); }, timers: fakeTimers() });
  s.start('20261113-000000-00000000');
  s.push({ k: 's' });
  await s.flush();
  assert.equal(s.inflight.seq, 0);
  assert.match(s.lastError, /did not answer/);
});

// ── The protocol ────────────────────────────────────────────────────────────

test('plan: three conditions, four phases, lead-ins and measured parts; the second quality adds two', () => {
  const p = protocolPlan();
  assert.deepEqual([...new Set(p.map((s) => s.condition))], ['local', 'jpeg', 'webrtc']);
  assert.deepEqual(p.filter((s) => s.condition === 'local').map((s) => `${s.phase}:${s.part}`),
    ['settle:lead', 'still:lead', 'still:measure', 'turn:lead', 'turn:measure', 'stops:lead', 'stops:measure']);
  assert.equal(planSeconds(p), 3 * (5 + 3 + 15 + 3 + 15 + 3 + 20));
  assert.equal(protocolPlan({ second: true }).filter((s) => s.condition.endsWith('-small')).length, 14);
  assert.equal(CONDITIONS['jpeg-small'].scale, 0.6);
  assert.equal(planSeconds(protocolPlan({ scale: 0.1 })), planSeconds(p) / 10);
  assert.throws(() => protocolPlan({ conditions: ['nope'] }));
});

test('prompts: readable lines, a cancel button, in front during the lead-in then parked', () => {
  const p = protocolPlan();
  const i = p.findIndex((s) => s.condition === 'jpeg' && s.phase === 'turn' && s.part === 'lead');
  const n = promptNotice(p[i], i, p);
  assert.match(n.title, /Measuring 2\/3: Turn your head slowly/);
  assert.equal(n.parkAfterMs, 3000);
  assert.equal(n.sticky, true);
  assert.deepEqual(n.actions, [{ id: 'measure-cancel', label: 'Cancel' }]);
  assert.ok(n.lines.some((l) => /refinement over JPEG/.test(l)));
  assert.ok(n.lines.some((l) => /15 s measured/.test(l)));
  assert.ok(n.lines.every((l) => l.length < 100));
});

function drive(proto, { until = 1e7, step = 50, ready = () => true } = {}) {
  const log = [];
  for (let t = 0; t <= until && !proto.done; t += step) {
    for (const a of proto.update(t, { ready })) log.push({ t, ...a });
  }
  return log;
}

test('run: the condition is applied once, at its start; steps follow their times; pulses mark each measured part', () => {
  const plan = protocolPlan({ conditions: ['local', 'jpeg'] });
  const proto = new MeasureProtocol(plan, { now: 0 });
  const log = drive(proto);
  const applies = log.filter((a) => a.do === 'apply');
  assert.deepEqual(applies.map((a) => a.condition), ['local', 'jpeg']);
  const phases = log.filter((a) => a.do === 'phase');
  assert.equal(phases.length, plan.length);
  // Each step lasts its time (within one update step).
  for (let k = 1; k < phases.length; k++) {
    const dur = phases[k].t - phases[k - 1].t;
    assert.ok(Math.abs(dur - plan[k - 1].ms) <= 50, `${plan[k - 1].phase}:${plan[k - 1].part} ${dur}`);
  }
  // Nothing but tags, prompts and pulses between two applies: within a
  // condition the run changes nothing else.
  const kinds = new Set(log.map((a) => a.do));
  assert.deepEqual([...kinds].sort(), ['apply', 'done', 'phase', 'prompt', 'pulse']);
  assert.equal(log.at(-1).do, 'done');
  assert.equal(log.at(-1).cancelled, false);
  // The stops phase: alternating move and hold pulses, every 2 s.
  const cues = log.filter((a) => a.do === 'pulse' && a.kind !== 'start' && a.t < phases.find((p) => p.step.condition === 'jpeg').t);
  assert.equal(cues.length, 10);
  assert.deepEqual(cues.slice(0, 3).map((a) => a.kind), ['move', 'hold', 'move']);
  assert.equal(cues[1].t - cues[0].t, STOP_CYCLE.move);
  // Prompts only at lead-ins.
  assert.equal(log.filter((a) => a.do === 'prompt').length, plan.filter((s) => s.part === 'lead').length);
});

test('run: a refined condition is timed from when it is ready; one that fails or times out is skipped', () => {
  const plan = protocolPlan({ conditions: ['local', 'webrtc', 'jpeg'], scale: 0.1 });
  let cur = 0, asked = null;
  const ready = (c) => {
    if (c === 'webrtc') return 'failed: WebRTC video (no WebRTC in this browser)';
    if (c !== 'jpeg') return true;
    if (asked === null) asked = cur;
    return cur - asked >= 4000;            // connected 4 s after it was first asked
  };
  const proto = new MeasureProtocol(plan, { now: 0, readyTimeoutMs: 20000 });
  const log = [];
  for (let t = 0; !proto.done && t < 1e6; t += 50) { cur = t; for (const a of proto.update(t, { ready })) log.push({ t, ...a }); }
  const skip = log.find((a) => a.do === 'skip');
  assert.equal(skip.condition, 'webrtc');
  assert.match(skip.why, /no WebRTC/);
  assert.ok(!log.some((a) => a.do === 'phase' && a.step.condition === 'webrtc' && a.step.phase !== 'settle'));
  // jpeg waited 4 s to be ready, then its settle phase (0.5 s) ran in full.
  const jpegSettle = log.find((a) => a.do === 'phase' && a.step.condition === 'jpeg');
  const jpegStill = log.find((a) => a.do === 'phase' && a.step.condition === 'jpeg' && a.step.phase === 'still');
  const waited = jpegStill.t - jpegSettle.t;
  assert.ok(waited >= 4500 - 100 && waited <= 4500 + 150, `${waited}`);
  assert.deepEqual(proto.skipped, ['webrtc']);
  // Never ready at all: skipped after the timeout.
  const p2 = new MeasureProtocol(protocolPlan({ conditions: ['jpeg'], scale: 0.1 }), { now: 0, readyTimeoutMs: 1000 });
  const l2 = drive(p2, { ready: () => false });
  assert.match(l2.find((a) => a.do === 'skip').why, /not ready after 1 s/);
  assert.equal(l2.at(-1).do, 'done');
});

test('run: a refined condition that stops holding is skipped from there; a moment\'s gap is told once per step', () => {
  const plan = protocolPlan({ conditions: ['webrtc', 'local'], scale: 0.1 });
  let state = true;
  const proto = new MeasureProtocol(plan, { now: 0 });
  const log = [];
  for (let t = 0; !proto.done && t < 1e6; t += 50) {
    const s = proto.step;
    if (s && s.phase === 'still' && s.part === 'measure') state = t % 1000 < 500 ? false : true;    // gaps
    if (s && s.phase === 'turn' && s.part === 'measure') state = 'failed: WebRTC video (no video frame decoded for 3 s)';
    for (const a of proto.update(t, { ready: () => state })) log.push({ t, ...a });
    if (s && s.condition === 'local') state = true;
  }
  const degraded = log.filter((a) => a.do === 'degraded');
  const still = degraded.filter((a) => a.phase === 'still' && a.part === 'measure');
  assert.equal(still.length, 1);                              // many gaps in that step, told once
  assert.equal(new Set(degraded.map((a) => `${a.phase}:${a.part}`)).size, degraded.length);
  const skip = log.find((a) => a.do === 'skip');
  assert.match(skip.why, /^lost during turn: failed: WebRTC video/);
  assert.ok(!log.some((a) => a.do === 'phase' && a.step.condition === 'webrtc' && a.step.phase === 'stops'));
  assert.ok(log.some((a) => a.do === 'phase' && a.step.condition === 'local' && a.step.phase === 'stops'));
  assert.equal(log.at(-1).do, 'done');
});

test('run: cancel ends it at once, once', () => {
  const proto = new MeasureProtocol(protocolPlan(), { now: 0 });
  proto.update(0, {});
  const a = proto.cancel('cancelled by the viewer');
  assert.deepEqual(a, [{ do: 'done', cancelled: true, why: 'cancelled by the viewer' }]);
  assert.deepEqual(proto.cancel(), []);
  assert.deepEqual(proto.update(1000, {}), []);
});

test('run: the motion asked for (desktop auto motion)', () => {
  const plan = protocolPlan({ conditions: ['local'] });
  const proto = new MeasureProtocol(plan, { now: 0 });
  const at = {};
  for (let t = 0; !proto.done; t += 50) {
    proto.update(t, {});
    const s = proto.step;
    if (s) (at[`${s.phase}:${s.part}`] ||= new Set()).add(proto.motion(t));
  }
  assert.deepEqual([...at['still:measure']], ['still']);
  assert.deepEqual([...at['turn:measure']], ['turn']);
  assert.deepEqual([...at['stops:measure']].sort(), ['hold', 'move']);
  assert.deepEqual([...at['turn:lead']], ['still']);
  assert.equal(PHASES.settle.measure, 0);
});

// ── The report's fixture, as the page computes it ───────────────────────────

test('report fixture: each sample\'s frame figures are what FrameStats gives for its intervals', () => {
  const text = readFileSync(new URL('./measure_fixture.jsonl', import.meta.url), 'utf8');
  const lines = text.trim().split('\n').map((l) => JSON.parse(l));
  assert.equal(lines[0].k, 'h');
  let n = 0;
  for (const s of lines.filter((l) => l.k === 's')) {
    const fs = new FrameStats(1000);
    for (const v of s.iv) fs.push(v, s.frames.cpu_mean, s.frames.gpu_mean ?? -1);
    const f = frameFigures(fs, s.frames.target_hz);
    for (const k of ['n', 'fps', 'mean', 'p50', 'p95', 'missed_pct']) assert.equal(f[k], s.frames[k], `${k} at t=${s.t}`);
    n++;
  }
  assert.ok(n >= 6);
});
